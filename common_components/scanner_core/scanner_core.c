/* common_components/scanner_core/scanner_core.c
 *
 * Scanner core — dummy BLE scan event generator.
 *
 * ---- Timer → task notification pattern ----------------------------------
 *
 * FreeRTOS software timer callbacks run in the timer service task context.
 * Calling output_write() or scanner_link_send() from a timer callback is
 * unsafe because those functions can block (mutex acquisition). Instead:
 *
 *   1. The scan timer callback calls xTaskNotify() — non-blocking.
 *   2. scanner_core_task wakes on the notification, calls the registered
 *      event callback (scanner_link_send in normal operation), then sleeps.
 *
 * This keeps the timer callback truly non-blocking and correctly routes
 * scan events through the registered transport.
 *
 * ---- Watchdog -----------------------------------------------------------
 *
 * scanner_core_task feeds the watchdog at the top of every loop iteration.
 * xTaskNotifyWait() uses a 2000 ms timeout so the watchdog is fed at least
 * that frequently regardless of timer activity. The watchdog registration
 * uses a 8000 ms interval (well above the 2000 ms feed period).
 *
 * ---- No dynamic allocation after init ----------------------------------
 *
 * The timer handle and task handle are created once in scanner_core_init().
 * No memory is allocated at runtime.
 */

#include "scanner_core.h"

#include "watchdog.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_log.h"

#include <stddef.h>

static const char *TAG = "scanner_core";

/* ======================================================================
 * Compile-time defaults
 * ====================================================================== */

#ifndef CONFIG_SCANNER_DUMMY_SCAN_INTERVAL_MS
#define CONFIG_SCANNER_DUMMY_SCAN_INTERVAL_MS  5000
#endif

/* Core task watchdog interval — must exceed the xTaskNotifyWait timeout. */
#define CORE_WDT_INTERVAL_MS   8000
#define CORE_NOTIFY_TIMEOUT_MS 2000
#define CORE_TASK_STACK        3072
#define CORE_TASK_PRIORITY     2

/* Notification bit set by the timer callback. */
#define NOTIFY_BLE_SCAN  (1u << 0)

/* ======================================================================
 * Module state
 * ====================================================================== */

static TimerHandle_t         s_ble_timer  = NULL;
static TaskHandle_t          s_core_task  = NULL;
static volatile scanner_event_cb_t s_event_cb = NULL;

/* Static scan result JSON sent on every timer tick.
 * Format matches what Commander's scan_parser expects at the top level. */
static const char s_scan_result_json[] =
    "{\"type\":\"scan_result\",\"module\":\"ble\","
    "\"data\":[{\"name\":\"S1_Test_Device\",\"rssi\":-50}]}";

/* ======================================================================
 * Timer callback — must not block
 * ====================================================================== */

static void ble_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    /* xTaskNotify is safe from timer callback context (no blocking). */
    if (s_core_task != NULL) {
        xTaskNotify(s_core_task, NOTIFY_BLE_SCAN, eSetBits);
    }
}

/* ======================================================================
 * Core task
 * ====================================================================== */

static void scanner_core_task(void *arg)
{
    (void)arg;

    while (1) {
        /* Feed watchdog FIRST — before any blocking wait. */
        watchdog_feed_task("scanner_core");

        uint32_t bits = 0;
        xTaskNotifyWait(0, 0xFFFFFFFFu, &bits,
                        pdMS_TO_TICKS(CORE_NOTIFY_TIMEOUT_MS));

        if ((bits & NOTIFY_BLE_SCAN) == 0) {
            /* Timeout with no notification — loop and re-feed watchdog. */
            continue;
        }

        /* Invoke the registered event callback if one is set. */
        scanner_event_cb_t cb = s_event_cb;   /* snapshot volatile pointer */
        if (cb != NULL) {
            cb(s_scan_result_json);
            ESP_LOGI(TAG, "BLE scan event dispatched");
        } else {
            ESP_LOGW(TAG, "BLE scan event ready but no callback registered");
        }
    }

    /* Unreachable. */
    vTaskDelete(NULL);
}

/* ======================================================================
 * Public API
 * ====================================================================== */

esp_err_t scanner_core_init(void)
{
    /* Create the BLE scan timer — stopped, auto-reload. */
    s_ble_timer = xTimerCreate(
        "ble_scan",
        pdMS_TO_TICKS(CONFIG_SCANNER_DUMMY_SCAN_INTERVAL_MS),
        pdTRUE,           /* auto-reload */
        NULL,
        ble_timer_cb);

    if (s_ble_timer == NULL) {
        ESP_LOGE(TAG, "xTimerCreate failed");
        return ESP_FAIL;
    }

    /* Register with watchdog BEFORE creating the task. */
    watchdog_register_task("scanner_core", CORE_WDT_INTERVAL_MS);

    if (xTaskCreate(scanner_core_task, "scanner_core",
                    CORE_TASK_STACK, NULL,
                    CORE_TASK_PRIORITY, &s_core_task) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        xTimerDelete(s_ble_timer, 0);
        s_ble_timer = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Scanner core ready (interval=%d ms)",
             CONFIG_SCANNER_DUMMY_SCAN_INTERVAL_MS);
    return ESP_OK;
}

void scanner_core_register_event_cb(scanner_event_cb_t cb)
{
    s_event_cb = cb;
    ESP_LOGI(TAG, "Event callback %s", cb ? "registered" : "cleared");
}

void scanner_core_start_ble(void)
{
    if (s_ble_timer == NULL) {
        ESP_LOGE(TAG, "start_ble: timer not created");
        return;
    }
    if (xTimerIsTimerActive(s_ble_timer)) {
        ESP_LOGI(TAG, "BLE scan already active");
        return;
    }
    xTimerStart(s_ble_timer, 0);
    ESP_LOGI(TAG, "BLE scan started (interval=%d ms)",
             CONFIG_SCANNER_DUMMY_SCAN_INTERVAL_MS);
}

void scanner_core_stop_ble(void)
{
    if (s_ble_timer == NULL) {
        ESP_LOGE(TAG, "stop_ble: timer not created");
        return;
    }
    if (!xTimerIsTimerActive(s_ble_timer)) {
        ESP_LOGI(TAG, "BLE scan already stopped");
        return;
    }
    xTimerStop(s_ble_timer, 0);
    ESP_LOGI(TAG, "BLE scan stopped");
}
