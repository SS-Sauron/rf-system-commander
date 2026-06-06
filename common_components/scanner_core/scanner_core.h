/* common_components/scanner_core/scanner_core.h
 *
 * Scanner core — dummy BLE scan event generator.
 *
 * Provides a FreeRTOS software timer that fires at a configurable interval
 * (CONFIG_SCANNER_DUMMY_SCAN_INTERVAL_MS). On each tick the core task
 * invokes the registered event callback with a synthetic scan_result JSON
 * line.
 *
 * The event callback is set by the caller (typically main.c) to decouple
 * scanner_core from the transport layer (scanner_link) and avoid a circular
 * component dependency.
 *
 * Usage:
 *   scanner_core_init();
 *   scanner_core_register_event_cb(scanner_link_send);  // wired in main.c
 *   scanner_core_start_ble();   // starts emitting scan events
 *   ...
 *   scanner_core_stop_ble();    // stops emission
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback type invoked by scanner_core when a scan event is ready.
 *
 * The callback receives a null-terminated raw JSON string and must not block.
 * It is called from scanner_core_task context (not from a timer callback),
 * so acquiring mutexes with a short timeout is safe.
 *
 * @param json_line  Null-terminated raw scan_result JSON object. The string
 *                   is valid only for the duration of the call.
 */
typedef void (*scanner_event_cb_t)(const char *json_line);

/**
 * @brief Initialise the scanner core.
 *
 * Creates the scan timer (stopped) and the core task. Must be called once
 * during boot before scanner_core_start_ble().
 *
 * @return ESP_OK, or ESP_FAIL if timer or task creation fails.
 */
esp_err_t scanner_core_init(void);

/**
 * @brief Register the callback that receives scan events.
 *
 * Called once from main.c after both scanner_core_init() and
 * scanner_link_init() have succeeded. Replaces any previously registered
 * callback. Thread-safe (written atomically as a pointer).
 *
 * @param cb  Callback function, or NULL to disable event dispatch.
 */
void scanner_core_register_event_cb(scanner_event_cb_t cb);

/**
 * @brief Start emitting dummy BLE scan events.
 *
 * Starts the scan timer with CONFIG_SCANNER_DUMMY_SCAN_INTERVAL_MS period,
 * auto-reload. Safe to call when already started (no-op).
 */
void scanner_core_start_ble(void);

/**
 * @brief Stop emitting dummy BLE scan events.
 *
 * Stops the scan timer. Safe to call when already stopped (no-op).
 */
void scanner_core_stop_ble(void);

#ifdef __cplusplus
}
#endif
