#include "watchdog.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "watchdog";

#define MAX_MONITORED_TASKS 8
#define WATCHDOG_CHECK_PERIOD_MS 2000 // Check every 2 seconds

typedef struct
{
    char name[16];
    uint32_t max_interval_ms;
    TickType_t last_feed_ticks;
    bool registered;
} monitored_task_t;

static monitored_task_t monitored_tasks[MAX_MONITORED_TASKS];
static portMUX_TYPE spinlock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t watchdog_register_task(const char *name, uint32_t max_interval_ms)
{
    portENTER_CRITICAL(&spinlock);
    for (int i = 0; i < MAX_MONITORED_TASKS; i++)
    {
        if (!monitored_tasks[i].registered)
        {
            strncpy(monitored_tasks[i].name, name, sizeof(monitored_tasks[i].name) - 1);
            monitored_tasks[i].name[sizeof(monitored_tasks[i].name) - 1] = '\0';
            monitored_tasks[i].max_interval_ms = max_interval_ms;
            monitored_tasks[i].last_feed_ticks = xTaskGetTickCount();
            monitored_tasks[i].registered = true;
            portEXIT_CRITICAL(&spinlock);
            // No ESP_LOGI here — caller reports success.
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&spinlock);
    // Keep the error log here as it's in the caller's context and not inside the watchdog task.
    // But to be safe, we can replace with return code; main.c can log if needed.
    return ESP_ERR_NO_MEM;
}

esp_err_t watchdog_feed_task(const char *name)
{
    portENTER_CRITICAL(&spinlock);
    for (int i = 0; i < MAX_MONITORED_TASKS; i++)
    {
        if (monitored_tasks[i].registered && strcmp(monitored_tasks[i].name, name) == 0)
        {
            monitored_tasks[i].last_feed_ticks = xTaskGetTickCount();
            portEXIT_CRITICAL(&spinlock);
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&spinlock);
    ESP_LOGW(TAG, "Task '%s' tried to feed but is not registered", name);
    return ESP_ERR_NOT_FOUND;
}

static void watchdog_monitor_task(void *pvParameters)
{
    esp_task_wdt_add(NULL);
    // No ESP_LOGI here

    while (1)
    {
        portENTER_CRITICAL(&spinlock);
        TickType_t now = xTaskGetTickCount();
        bool all_ok = true;
        for (int i = 0; i < MAX_MONITORED_TASKS; i++)
        {
            if (!monitored_tasks[i].registered)
                continue;
            uint32_t elapsed_ms = (now - monitored_tasks[i].last_feed_ticks) * portTICK_PERIOD_MS;
            if (elapsed_ms > monitored_tasks[i].max_interval_ms)
            {
                all_ok = false;
                break;
            }
        }
        portEXIT_CRITICAL(&spinlock);

        if (!all_ok)
        {
            esp_restart();
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_CHECK_PERIOD_MS));
    }
}

void watchdog_start(void)
{
    // Optionally remove this log:
    // ESP_LOGI(TAG, "Starting watchdog monitor task");
    xTaskCreate(watchdog_monitor_task, "watchdog_task", 4096, NULL, 5, NULL);
}
