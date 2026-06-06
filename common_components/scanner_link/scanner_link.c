/* common_components/scanner_link/scanner_link.c
 *
 * Scanner data and control link — UART1, bidirectional.
 *
 * ---- TX path (outbound scan events) ------------------------------------
 *
 * scanner_link_send() acquires s_tx_mutex, writes the JSON line + '\n' to
 * UART1 TX, and releases the mutex. The mutex prevents interleaving if
 * multiple tasks ever call scanner_link_send concurrently (currently only
 * scanner_core_task does, but the mutex is cheap insurance).
 *
 * ---- RX path (incoming commands from Commander) ------------------------
 *
 * scanner_link_task reads one byte at a time from UART1 RX into a static
 * line buffer. On newline, it parses the JSON using json_get_string_field
 * and dispatches to scanner_core_start/stop_ble. Invalid lines are ignored.
 *
 * The line buffer is stack-allocated (LINK_LINE_BUF_SIZE bytes). It is
 * deliberately larger than the console buffer because incoming JSON
 * commands from the Commander can carry arbitrary payloads in the future.
 *
 * ---- Watchdog -----------------------------------------------------------
 *
 * scanner_link_task is registered as "scanner_link" with a 2000 ms WDT
 * interval. uart_read_bytes uses a 100 ms timeout so the feed cadence
 * (at the top of each iteration) is ~100 ms — far within 2000 ms.
 *
 * ---- output_write() never called ----------------------------------------
 *
 * All diagnostic output goes through ESP_LOGI/W/E (UART0).
 */

#include "scanner_link.h"

#include "scanner_core.h"
#include "json_utils.h"
#include "watchdog.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/uart.h"
#include "esp_log.h"

#include <string.h>
#include <stddef.h>

static const char *TAG = "scanner_link";

/* ======================================================================
 * Compile-time defaults
 * ====================================================================== */

#ifndef CONFIG_SCANNER_DATA_UART_TX_PIN
#define CONFIG_SCANNER_DATA_UART_TX_PIN  17
#endif
#ifndef CONFIG_SCANNER_DATA_UART_RX_PIN
#define CONFIG_SCANNER_DATA_UART_RX_PIN  16
#endif
#ifndef CONFIG_SCANNER_DATA_UART_BAUD
#define CONFIG_SCANNER_DATA_UART_BAUD    115200
#endif

#define DATA_UART_NUM     UART_NUM_1
#define DATA_UART_RX_BUF  512       /* RX ring buffer for incoming commands */
#define LINK_TASK_STACK   4096
#define LINK_TASK_PRIO    3         /* higher than console; data link is time-sensitive */
#define LINK_WDT_MS       2000
#define LINK_READ_MS      100

/* Incoming line buffer — larger than console to handle future payloads. */
#define LINK_LINE_BUF_SIZE  512

/* Field buffers for JSON parsing. */
#define LINK_TYPE_BUF  32
#define LINK_CMD_BUF   64

/* ======================================================================
 * Module state
 * ====================================================================== */

static SemaphoreHandle_t s_tx_mutex = NULL;

/* ======================================================================
 * Public: scanner_link_send
 * ====================================================================== */

void scanner_link_send(const char *json_line)
{
    if (json_line == NULL || s_tx_mutex == NULL) { return; }

    /* 50 ms timeout — drop line silently if bus is busy.
     * In normal operation the mutex is uncontended. */
    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "TX mutex timeout — line dropped");
        return;
    }

    size_t len = strlen(json_line);
    uart_write_bytes(DATA_UART_NUM, json_line, len);
    uart_write_bytes(DATA_UART_NUM, "\n", 1);

    xSemaphoreGive(s_tx_mutex);
}

/* ======================================================================
 * Incoming command dispatcher
 * ====================================================================== */

static void dispatch_cmd(const char *cmd)
{
    /* Case-sensitive match — Commander sends canonical uppercase strings. */
    if (strcmp(cmd, "SCAN BLE START") == 0) {
        ESP_LOGI(TAG, "Remote cmd: SCAN BLE START");
        scanner_core_start_ble();
    } else if (strcmp(cmd, "SCAN BLE STOP") == 0) {
        ESP_LOGI(TAG, "Remote cmd: SCAN BLE STOP");
        scanner_core_stop_ble();
    } else if (strcmp(cmd, "SCAN WIFI START") == 0 ||
               strcmp(cmd, "SCAN WIFI STOP")  == 0) {
        ESP_LOGW(TAG, "Remote cmd '%s': not implemented", cmd);
    } else {
        ESP_LOGW(TAG, "Remote cmd unknown: '%s'", cmd);
    }
}

/* ======================================================================
 * Link task — reads incoming JSON commands from Commander via UART1 RX
 * ====================================================================== */

static void scanner_link_task(void *arg)
{
    (void)arg;

    /* Stack-allocated line buffer for incoming JSON from Commander. */
    char     line_buf[LINK_LINE_BUF_SIZE];
    int      pos          = 0;
    bool     line_too_long = false;
    bool     last_was_cr   = false;

    while (1) {
        /* Feed watchdog FIRST — before any blocking operation. */
        watchdog_feed_task("scanner_link");

        uint8_t ch = 0;
        int n = uart_read_bytes(DATA_UART_NUM, &ch, 1,
                                pdMS_TO_TICKS(LINK_READ_MS));
        if (n <= 0) { last_was_cr = false; continue; }

        /* CR+LF suppression — same pattern as command_parser and
         * scanner_console to handle all terminal line endings. */
        bool is_cr     = (ch == '\r');
        bool is_lf     = (ch == '\n');
        bool suppress  = (is_lf && last_was_cr);
        last_was_cr    = is_cr;

        if (suppress) { continue; }

        if (is_cr || is_lf) {
            line_buf[pos] = '\0';

            if (line_too_long) {
                /* Overlong line — silently discard. */
                ESP_LOGW(TAG, "Incoming line too long — discarded");
                line_too_long = false;
                pos = 0;
                continue;
            }

            /* Trim trailing whitespace. */
            while (pos > 0 &&
                   (line_buf[pos - 1] == ' ' ||
                    line_buf[pos - 1] == '\t')) {
                pos--;
            }
            line_buf[pos] = '\0';

            if (pos == 0) { continue; }   /* blank line */

            /* Must start with '{' to be candidate JSON. */
            if (line_buf[0] != '{') {
                ESP_LOGW(TAG, "Non-JSON line ignored");
                pos = 0;
                continue;
            }

            /* Parse "type" and "cmd" using json_utils. */
            char type_buf[LINK_TYPE_BUF] = {0};
            char cmd_buf[LINK_CMD_BUF]   = {0};

            json_get_string_field(line_buf, (size_t)pos,
                                  "type", type_buf, sizeof(type_buf));
            json_get_string_field(line_buf, (size_t)pos,
                                  "cmd",  cmd_buf,  sizeof(cmd_buf));

            if (strcmp(type_buf, "scanner_cmd") == 0 && cmd_buf[0] != '\0') {
                dispatch_cmd(cmd_buf);
            } else if (type_buf[0] == '\0') {
                ESP_LOGW(TAG, "JSON missing 'type' field — ignored");
            }
            /* Any other 'type' is silently ignored (future extensibility). */

            pos = 0;
            continue;
        }

        /* Regular character — accumulate into line buffer. */
        if (pos >= LINK_LINE_BUF_SIZE - 1) {
            line_too_long = true;
            continue;
        }
        line_buf[pos++] = (char)ch;
    }

    /* Unreachable. */
    vTaskDelete(NULL);
}

/* ======================================================================
 * Public API
 * ====================================================================== */

esp_err_t scanner_link_init(void)
{
    /* TX mutex — protects uart_write_bytes calls in scanner_link_send(). */
    s_tx_mutex = xSemaphoreCreateMutex();
    if (s_tx_mutex == NULL) {
        ESP_LOGE(TAG, "TX mutex creation failed");
        return ESP_FAIL;
    }

    /* UART1 driver — RX buffer for incoming commands; no TX buffer
     * (direct write mode keeps TX latency minimal). */
    esp_err_t ret = uart_driver_install(DATA_UART_NUM,
                                        DATA_UART_RX_BUF, 0, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_tx_mutex);
        s_tx_mutex = NULL;
        return ESP_FAIL;
    }

    uart_config_t cfg = {
        .baud_rate  = CONFIG_SCANNER_DATA_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ret = uart_param_config(DATA_UART_NUM, &cfg);
    if (ret != ESP_OK) {
        uart_driver_delete(DATA_UART_NUM);
        vSemaphoreDelete(s_tx_mutex);
        s_tx_mutex = NULL;
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    ret = uart_set_pin(DATA_UART_NUM,
                       CONFIG_SCANNER_DATA_UART_TX_PIN,
                       CONFIG_SCANNER_DATA_UART_RX_PIN,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        uart_driver_delete(DATA_UART_NUM);
        vSemaphoreDelete(s_tx_mutex);
        s_tx_mutex = NULL;
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    /* Register with watchdog BEFORE task creation. */
    watchdog_register_task("scanner_link", LINK_WDT_MS);

    if (xTaskCreate(scanner_link_task, "scanner_link",
                    LINK_TASK_STACK, NULL,
                    LINK_TASK_PRIO, NULL) != pdPASS) {
        uart_driver_delete(DATA_UART_NUM);
        vSemaphoreDelete(s_tx_mutex);
        s_tx_mutex = NULL;
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Link ready: UART%d TX=GPIO%d RX=GPIO%d %d baud",
             DATA_UART_NUM,
             CONFIG_SCANNER_DATA_UART_TX_PIN,
             CONFIG_SCANNER_DATA_UART_RX_PIN,
             CONFIG_SCANNER_DATA_UART_BAUD);
    return ESP_OK;
}
