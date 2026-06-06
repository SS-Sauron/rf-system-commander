/* common_components/scanner_link/scanner_link.h
 *
 * Scanner data and control link — UART1, bidirectional.
 *
 * TX (to Commander): raw scan_result JSON lines via scanner_link_send().
 * RX (from Commander): scanner_cmd JSON objects dispatched to scanner_core.
 *
 * scanner_link_send() is registered as the event callback in scanner_core
 * (wired in main.c) so that scan events reach the Commander without creating
 * a circular component dependency between scanner_core and scanner_link.
 *
 * Incoming command format:
 *   {"type":"scanner_cmd","cmd":"SCAN BLE START"}
 *
 * Supported "cmd" values:
 *   "SCAN BLE START"   — scanner_core_start_ble()
 *   "SCAN BLE STOP"    — scanner_core_stop_ble()
 *   "SCAN WIFI START"  — logged as not implemented, ignored
 *   "SCAN WIFI STOP"   — logged as not implemented, ignored
 *
 * Invalid or unrecognised JSON lines are silently ignored.
 * output_write() is never called from this component.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the scanner data/control link on UART1.
 *
 * Installs the UART1 driver, configures pins from Kconfig, creates a TX
 * mutex, registers the link task with the watchdog, and starts the task.
 *
 * Must be called before scanner_core_register_event_cb(scanner_link_send).
 *
 * @return ESP_OK, or ESP_FAIL if UART install or task creation fails.
 */
esp_err_t scanner_link_init(void);

/**
 * @brief Send a raw JSON line to the Commander over UART1.
 *
 * Appends a newline ('\n') after the JSON string. Thread-safe: protected
 * by an internal TX mutex. Non-blocking on the mutex (50 ms timeout);
 * drops the line silently on contention.
 *
 * This function is registered as the scanner_core event callback from
 * main.c. It may also be called directly for any other outbound message.
 *
 * @param json_line  Null-terminated raw JSON string. Must not be NULL.
 */
void scanner_link_send(const char *json_line);

#ifdef __cplusplus
}
#endif
