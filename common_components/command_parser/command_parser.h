/**
 * @file command_parser.h
 * @brief Serial command parser — public API.
 *
 * COMPONENT LOCATION: common_components/command_parser/
 *
 * Provides a dedicated UART2 console that accepts text commands and
 * responds with plain-text results. All machine-readable structured output
 * (JSON lines consumed by the host) still goes through output_write()
 * on UART0. This console is for human operators only.
 *
 * Default pins: RX=GPIO18, TX=GPIO17 (configurable via Kconfig).
 * Default baud: 115200 8N1.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the command parser.
 *
 * Installs the UART2 driver, configures pins and baud rate, registers
 * the task with the watchdog, and creates the command_parser_task.
 *
 * Must be called after rule_engine_init(), action_registry_init(), and
 * health_monitor_start() — the command handlers call into those components.
 *
 * @return  ESP_OK    Ready and running.
 *          ESP_FAIL  UART driver install or task creation failed.
 *                    Caller should log and esp_restart().
 */
esp_err_t command_parser_init(void);

#ifdef __cplusplus
}
#endif
