/**
 * @file scan_parser.c
 * @brief Scanner JSON parser implementation — shared component.
 *
 * COMPONENT LOCATION: common_components/scan_parser/
 *
 * REFACTORED AT C3: The five inline parsing helper functions that were
 * static to this file have been extracted into common_components/json_utils/
 * and are now called via json_utils.h.
 *
 * PUBLIC API IS UNCHANGED. No caller changes are required.
 */

#include "scan_parser.h"
#include "json_utils.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "output.h"
#include "watchdog.h"
#include "scan_receiver.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

static const char *TAG = "scan_parser";

#ifndef CONFIG_SCAN_PARSER_TASK_STACK_SIZE
#define CONFIG_SCAN_PARSER_TASK_STACK_SIZE      4096
#endif
#ifndef CONFIG_SCAN_PARSER_DATA_BUF_SIZE
#define CONFIG_SCAN_PARSER_DATA_BUF_SIZE        800
#endif
#ifndef CONFIG_SCAN_PARSER_WATCHDOG_INTERVAL_MS
#define CONFIG_SCAN_PARSER_WATCHDOG_INTERVAL_MS 10000
#endif
#ifndef CONFIG_SCAN_PARSER_WDT_FEED_PERIOD_MS
#define CONFIG_SCAN_PARSER_WDT_FEED_PERIOD_MS   3000
#endif
#ifndef CONFIG_OUTPUT_LINE_BUF_SIZE
#define CONFIG_OUTPUT_LINE_BUF_SIZE             2048
#endif

#define SCAN_PARSER_TASK_PRIORITY   3

static bool           s_initialized  = false;
static QueueHandle_t  s_rx_queue     = NULL;
static QueueHandle_t  s_event_queue  = NULL;
static TaskHandle_t   s_task_handle  = NULL;

static char         s_line[SCAN_RECEIVER_LINE_BUF];
static scan_event_t s_event;
static char         s_out_body[CONFIG_OUTPUT_LINE_BUF_SIZE];

/* ---- Core parse function ---------------------------------------------- */

static esp_err_t parse_scan_line(const char *raw, size_t raw_len, scan_event_t *out)
{
    const char *p   = raw;
    const char *end = raw + raw_len;

    p = json_skip_ws(p, end);
    if (p >= end || *p != '{') return ESP_ERR_INVALID_ARG;
    p++;

    bool type_ok = false, module_ok = false, data_ok = false;
    memset(out, 0, sizeof(*out));

    while (p < end) {
        p = json_skip_ws(p, end);
        if (p >= end) break;
        if (*p == '}') { p++; break; }

        const char *key; size_t key_len;
        if (!json_parse_string(&p, end, &key, &key_len)) return ESP_ERR_INVALID_ARG;

        p = json_skip_ws(p, end);
        if (p >= end || *p != ':') return ESP_ERR_INVALID_ARG;
        p++;
        p = json_skip_ws(p, end);
        if (p >= end) return ESP_ERR_INVALID_ARG;

        if (key_len == 4 && memcmp(key, "type", 4) == 0) {
            const char *val; size_t vl;
            if (!json_parse_string(&p, end, &val, &vl)) return ESP_ERR_INVALID_ARG;
            if (vl == 11 && memcmp(val, "scan_result", 11) == 0) type_ok = true;
            else return ESP_ERR_NOT_SUPPORTED;

        } else if (key_len == 6 && memcmp(key, "module", 6) == 0) {
            const char *val; size_t vl;
            if (!json_parse_string(&p, end, &val, &vl)) return ESP_ERR_INVALID_ARG;
            size_t cl = (vl >= sizeof(out->module)) ? sizeof(out->module)-1 : vl;
            memcpy(out->module, val, cl);
            out->module[cl] = '\0';
            module_ok = true;

        } else if (key_len == 9 && memcmp(key, "timestamp", 9) == 0) {
            const char *ts = p;
            while (p < end && *p != ',' && *p != '}' &&
                   *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
            size_t tl = (size_t)(p - ts);
            if (tl > 0 && tl < 20) {
                char tb[20]; memcpy(tb, ts, tl); tb[tl] = '\0';
                out->timestamp_ms = (int64_t)atoll(tb);
            }

        } else if (key_len == 4 && memcmp(key, "data", 4) == 0) {
            const char *arr; size_t al;
            if (!json_capture_array(&p, end, &arr, &al)) return ESP_ERR_INVALID_ARG;
            if (al == 0) return ESP_ERR_INVALID_ARG;
            if (al >= CONFIG_SCAN_PARSER_DATA_BUF_SIZE) return ESP_ERR_INVALID_SIZE;
            memcpy(out->data_json, arr, al);
            out->data_json[al] = '\0';
            out->item_count = json_count_array_items(arr, al);
            data_ok = true;

        } else {
            if (!json_skip_value(&p, end)) return ESP_ERR_INVALID_ARG;
        }

        p = json_skip_ws(p, end);
        if (p < end && *p == ',') p++;
    }

    if (!type_ok)   return ESP_ERR_INVALID_ARG;
    if (!module_ok) { ESP_LOGW(TAG, "Missing 'module'"); return ESP_ERR_INVALID_ARG; }
    if (!data_ok)   { ESP_LOGW(TAG, "Missing 'data'");   return ESP_ERR_INVALID_ARG; }

    out->api_version = SCAN_PARSER_API_VERSION;
    return ESP_OK;
}

/* ---- Dispatch --------------------------------------------------------- */

static void dispatch_event(const scan_event_t *ev)
{
    int w = snprintf(s_out_body, sizeof(s_out_body),
        "{\"type\":\"scan_parsed\",\"module\":\"%s\","
        "\"item_count\":%lu,\"timestamp_ms\":%lld,\"data\":%s}",
        ev->module, (unsigned long)ev->item_count,
        (long long)ev->timestamp_ms, ev->data_json);

    if (w < 0 || w >= (int)sizeof(s_out_body)) {
        ESP_LOGE(TAG, "body overflow (%d); increase OUTPUT_LINE_BUF_SIZE", w);
        return;
    }
    esp_err_t ret = output_write(s_out_body);
    if (ret != ESP_OK) ESP_LOGW(TAG, "output_write: %s", esp_err_to_name(ret));

    if (s_event_queue != NULL) {
        if (xQueueSend(s_event_queue, ev, 0) != pdTRUE) {
            ESP_LOGW(TAG, "event_queue full — event dropped");
            output_write("{\"type\":\"warning\",\"msg\":\"scan_event_queue_full\"}");
        }
    }
}

/* ---- Task ------------------------------------------------------------- */

static void scan_parser_task(void *arg)
{
    (void)arg;
    while (1) {
        watchdog_feed_task("scan_parser");
        if (xQueueReceive(s_rx_queue, s_line,
                          pdMS_TO_TICKS(CONFIG_SCAN_PARSER_WDT_FEED_PERIOD_MS)) != pdTRUE) {
            continue;
        }
        if (s_line[0] != '{') {
            ESP_LOGW(TAG, "Non-object line dropped");
            continue;
        }
        esp_err_t ret = parse_scan_line(s_line, strnlen(s_line, SCAN_RECEIVER_LINE_BUF), &s_event);
        switch (ret) {
        case ESP_OK:               dispatch_event(&s_event); break;
        case ESP_ERR_NOT_SUPPORTED: break;
        case ESP_ERR_INVALID_SIZE:
            ESP_LOGW(TAG, "data[] exceeds DATA_BUF_SIZE (%d)", CONFIG_SCAN_PARSER_DATA_BUF_SIZE);
            break;
        default:
            ESP_LOGW(TAG, "Malformed line dropped (%s)", esp_err_to_name(ret));
            break;
        }
    }
    vTaskDelete(NULL);
}

/* ---- Public API ------------------------------------------------------- */

esp_err_t scan_parser_init(QueueHandle_t rx_queue, QueueHandle_t event_queue)
{
    if (!rx_queue)      { ESP_LOGE(TAG, "rx_queue is NULL"); return ESP_ERR_INVALID_ARG; }
    if (s_initialized)  { ESP_LOGW(TAG, "already init"); return ESP_ERR_INVALID_STATE; }

    s_rx_queue    = rx_queue;
    s_event_queue = event_queue;

    watchdog_register_task("scan_parser", CONFIG_SCAN_PARSER_WATCHDOG_INTERVAL_MS);

    if (xTaskCreate(scan_parser_task, "scan_parser",
                    CONFIG_SCAN_PARSER_TASK_STACK_SIZE,
                    NULL, SCAN_PARSER_TASK_PRIORITY, &s_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_FAIL;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "Scan parser ready");
    return ESP_OK;
}

esp_err_t scan_parser_deinit(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_task_handle)  { vTaskDelete(s_task_handle); s_task_handle = NULL; }
    s_rx_queue = s_event_queue = NULL;
    s_initialized = false;
    ESP_LOGI(TAG, "Scan parser stopped");
    return ESP_OK;
}
