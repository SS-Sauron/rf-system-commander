/* common_components/scanner_console/scanner_console.h
 *
 * Local command console for the Scanner (UART2).
 *
 * Provides a human-readable line-based command interface on the scanner's
 * local UART2 port. Responses are written directly to UART2 via
 * uart_write_bytes(). output_write() is never called from this component.
 *
 * Supported commands (case-insensitive):
 *   HELP             — static help text
 *   STATUS           — health snapshot from health_get_snapshot()
 *   SCAN BLE START   — start dummy BLE scan events
 *   SCAN BLE STOP    — stop dummy BLE scan events
 *   SCAN WIFI START  — ERROR: not implemented
 *   SCAN WIFI STOP   — ERROR: not implemented
 *   <anything else>  — ERROR: unknown command
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise and start the local command console.
 *
 * Installs the UART2 driver, configures pins from Kconfig, registers the
 * console task with the watchdog, and creates the task.
 *
 * @return ESP_OK, or ESP_FAIL if UART install or task creation fails.
 */
esp_err_t scanner_console_init(void);

#ifdef __cplusplus
}
#endif
