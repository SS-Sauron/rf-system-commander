#pragma once

#include "esp_err.h"

/**
 * @brief Register a task to be monitored by the watchdog.
 *
 * @param name          Unique string identifier for the task (max 16 chars).
 * @param max_interval_ms  Maximum allowed time between calls to watchdog_feed_task().
 *                         If the task does not feed for longer than this, the system
 *                         will be reset.
 * @return ESP_OK on success, ESP_ERR_NO_MEM if the monitor table is full.
 */
esp_err_t watchdog_register_task(const char *name, uint32_t max_interval_ms);

/**
 * @brief Called by a monitored task to report it is still alive.
 *
 * @param name  Same name used during registration.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if the task was not registered.
 */
esp_err_t watchdog_feed_task(const char *name);

/**
 * @brief Start the dedicated watchdog monitoring task.
 *
 * Must be called once during system initialization.
 * The watchdog task will add itself to the hardware TWDT and begin
 * checking all registered tasks.
 */
void watchdog_start(void);
