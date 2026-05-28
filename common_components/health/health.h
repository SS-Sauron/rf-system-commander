/**
 * @file health.h
 * @brief System health monitor — shared component.
 *
 * Provides a single background FreeRTOS task that periodically reports:
 *   - Free heap and largest contiguous free block (MALLOC_CAP_DEFAULT)
 *   - Heap fragmentation index (1 - largest_free / total_free)
 *   - Stack high-water mark for the health task itself
 *
 * The task also feeds the Task Watchdog Timer (TWDT) for itself.
 * All output goes through ESP_LOGx, NOT through output_write(), so
 * this component has no dependency on output.c and can be started
 * before the output channel is ready.
 *
 * Shared by: scanner, commander.
 * Each project passes its own log tag so logs are distinguishable.
 *
 * COMPONENT LOCATION: common_components/health/
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the health monitor background task.
 *
 * Creates a low-priority FreeRTOS task that runs until the device resets.
 * Must be called only once per firmware image. Calling it a second time
 * returns ESP_ERR_INVALID_STATE without creating a duplicate task.
 *
 * @param log_tag  ESP_LOG tag string, e.g. "scanner_health" or "cmd_health".
 *                 The pointed-to string must remain valid indefinitely
 *                 (use a string literal or static buffer — not stack).
 *
 * @return  ESP_OK              Task created successfully.
 *          ESP_ERR_INVALID_ARG log_tag is NULL.
 *          ESP_ERR_INVALID_STATE Already started; second call ignored.
 *          ESP_FAIL            FreeRTOS task creation failed (out of heap).
 */
esp_err_t health_monitor_start(const char *log_tag);

#ifdef __cplusplus
}
#endif
