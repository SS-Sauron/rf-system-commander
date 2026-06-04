/**
 * @file health.c
 * @brief System health monitor implementation — shared component.
 *
 * COMPONENT LOCATION: common_components/health/
 *
 * Configuration knobs (set via project Kconfig or fall back to defaults):
 *   CONFIG_HEALTH_TASK_STACK_SIZE  — stack in bytes  (default 2560)
 *   CONFIG_HEALTH_INTERVAL_MS      — report period   (default 30000)
 *   CONFIG_HEALTH_FRAG_WARN_THRESH — frag % as int   (default 75)
 *
 * Using #ifndef guards so the component compiles correctly even when
 * included in a project that has not defined these symbols.
 */

#include "health.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "watchdog.h"
#include "esp_timer.h" /* esp_timer_get_time() — added at C5 */

#include <stdio.h>  /* snprintf */
#include <string.h> /* strlen */

/* ---- Compile-time defaults (override via project Kconfig) ------------- */

#ifndef CONFIG_HEALTH_TASK_STACK_SIZE
#define CONFIG_HEALTH_TASK_STACK_SIZE 2560
#endif

#ifndef CONFIG_HEALTH_INTERVAL_MS
#define CONFIG_HEALTH_INTERVAL_MS 30000
#endif

/* Integer percentage 0-100; warn if fragmentation exceeds this. */
#ifndef CONFIG_HEALTH_FRAG_WARN_THRESH
#define CONFIG_HEALTH_FRAG_WARN_THRESH 75
#endif

/* Health task priority: deliberately the lowest application priority.
 * It must never starve other tasks; if it misses a cycle the device is
 * busy enough that missing a log line is acceptable. */
#define HEALTH_TASK_PRIORITY 1

/* ---- Module state ----------------------------------------------------- */

static bool s_started = false; /* Guard against duplicate start calls. */

/* ---- Task implementation --------------------------------------------- */

static void health_task(void *arg)
{
    const char *tag = (const char *)arg;

    /* This task does NOT subscribe to the hardware Task Watchdog Timer.
     * Instead it relies on the project's software watchdog monitor.
     *
     * Feeding the software watchdog ensures that if this task hangs,
     * the dedicated monitor task will detect the missing heartbeat
     * after the configured max interval and reset the system.
     *
     * This approach keeps the hardware TWDT exclusively for the
     * monitor task itself, as mandated by the project's watchdog
     * lifecycle standard (C4). */

    while (1)
    {
        /* Feed the software watchdog FIRST each cycle.  If a later call
         * within this loop hangs, the monitor will have already been
         * petted, so it can still detect the stall. */
        watchdog_feed_task(tag);

        /* ---- Heap metrics -------------------------------------------- */
        uint32_t free_heap = esp_get_free_heap_size();
        uint32_t largest_free = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

        /* Fragmentation index: 0 = perfectly contiguous, 1 = fully fragmented.
         * Guard against divide-by-zero if heap somehow reports zero free. */
        float frag = 0.0f;
        if (free_heap > 0U)
        {
            frag = 1.0f - ((float)largest_free / (float)free_heap);
        }

        /* ---- Stack watermark (health task only at C0) ----------------- */
        /* Later stages may iterate all tasks via uxTaskGetSystemState().  */
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);

        ESP_LOGI(tag,
                 "heap_free=%lu  largest_block=%lu  frag=%.2f  stack_hwm=%u",
                 (unsigned long)free_heap,
                 (unsigned long)largest_free,
                 (double)frag,
                 (unsigned int)hwm);

        /* Fragmentation warning: anything above the threshold suggests
         * a module is allocating and freeing irregular-sized blocks. */
        int frag_pct = (int)(frag * 100.0f);
        if (frag_pct > CONFIG_HEALTH_FRAG_WARN_THRESH)
        {
            ESP_LOGW(tag,
                     "High fragmentation: %d%% — investigate allocation patterns "
                     "(radio stack init/deinit cycling is the most common cause)",
                     frag_pct);
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_HEALTH_INTERVAL_MS));
    }

    /* Unreachable — the task runs forever. No cleanup needed. */
}

/* ---- Public API ------------------------------------------------------- */

esp_err_t health_monitor_start(const char *log_tag)
{
    if (log_tag == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_started)
    {
        /* Caller error: double-start. Log and return — do NOT create a
         * second task; duplicate TWDT registrations corrupt the watchdog. */
        ESP_LOGW(log_tag, "health_monitor_start called more than once — ignored");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t created = xTaskCreate(
        health_task,
        "health_task",
        CONFIG_HEALTH_TASK_STACK_SIZE,
        (void *)log_tag, /* String pointer passed as arg; caller owns it. */
        HEALTH_TASK_PRIORITY,
        NULL /* Task handle not retained; task runs forever. */
    );

    if (created != pdPASS)
    {
        /* Use printf here: ESP_LOG may not be routed yet, and this is a
         * critical boot failure that must be visible regardless. */
        printf("[health] FATAL: xTaskCreate failed — out of heap at boot\n");
        return ESP_FAIL;
    }

    s_started = true;
    return ESP_OK;
}

/* ======================================================================
 * C5 Snapshot API
 * ====================================================================== */

esp_err_t health_get_snapshot(char *buffer, size_t buf_size)
{
    if (buffer == NULL || buf_size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* ---- Heap metrics ---- */
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t largest_free = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    int64_t uptime_ms = esp_timer_get_time() / 1000LL;

    /* ---- Task state (static buffer — single-caller contract) ---------- */
    /* TaskStatus_t requires configUSE_TRACE_FACILITY=1 (ESP-IDF default).
     * Array raised to 24 at C6 to accommodate bt_media_task and any future
     * internal BT stack tasks that appear under uxTaskGetSystemState.
     * Caller (command_parser_task) is the only user; no concurrent access.*/
    static TaskStatus_t s_tasks[24];
    UBaseType_t task_count = uxTaskGetSystemState(s_tasks, 24, NULL);

    size_t pos = 0;
    int w;

    w = snprintf(buffer + pos, buf_size - pos,
                 "Uptime: %lld ms\r\n"
                 "Heap free: %lu | Largest block: %lu\r\n"
                 "Tasks (%u):\r\n",
                 (long long)uptime_ms,
                 (unsigned long)free_heap,
                 (unsigned long)largest_free,
                 (unsigned)task_count);
    if (w > 0 && pos + (size_t)w < buf_size)
    {
        pos += (size_t)w;
    }

    for (UBaseType_t i = 0; i < task_count && pos < buf_size - 1; i++)
    {
        w = snprintf(buffer + pos, buf_size - pos,
                     "  %-16s prio=%-2u hwm=%u\r\n",
                     s_tasks[i].pcTaskName,
                     (unsigned)s_tasks[i].uxCurrentPriority,
                     (unsigned)s_tasks[i].usStackHighWaterMark);
        if (w > 0 && pos + (size_t)w < buf_size)
        {
            pos += (size_t)w;
        }
    }

    return ESP_OK;
}
