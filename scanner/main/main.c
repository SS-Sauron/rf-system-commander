/* scanner/main/main.c
 *
 * Scanner firmware entry point.
 *
 * ---- Init order ----------------------------------------------------------
 *
 *  1. printf boot banner          (before any driver is ready)
 *  2. nvs_flash_init              (required by NVS-backed components)
 *  3. output_init                 (fatal: sets device identity for JSON lines)
 *  4. watchdog_start              (starts the hardware TWDT monitor task)
 *  5. watchdog_register_task +    (register health task before creating it)
 *     health_monitor_start
 *  6. scanner_core_init           (creates timer + core task, timer not started)
 *  7. scanner_link_init           (opens UART1, creates link task)
 *  8. scanner_core_register_event_cb(scanner_link_send)
 *                                 (wires scan events → UART1 without creating
 *                                  a circular component dependency)
 *  9. scanner_console_init        (opens UART2, creates console task)
 * 10. output_write boot-ready     (goes to printf/UART0 — diagnostics only;
 *                                  Commander does not need this line)
 * 11. while(1) vTaskDelay(1000)   (app_main must not return)
 *
 * ---- Output channels -----------------------------------------------------
 *
 *  UART0 (printf / output_write): diagnostic JSON for development monitoring.
 *  UART1 (scanner_link_send):     raw scan_result JSON read by Commander.
 *  UART2 (scanner_console):       human-readable local console responses.
 *
 * output_write() uses printf and therefore targets UART0. It is NOT wired to
 * UART1 because the Commander's scan_parser expects bare scan_result objects
 * at the top level, but output_write wraps everything in an envelope keyed
 * {"source":...,"msg":{...}}. scanner_link_send() bypasses that envelope and
 * writes raw JSON directly to UART1.
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "output.h"
#include "watchdog.h"
#include "health.h"
#include "scanner_core.h"
#include "scanner_link.h"
#include "scanner_console.h"

static const char *TAG = "scanner_main";

void app_main(void)
{
    /* 1. Early boot banner before any subsystem is initialised. */
    printf("\r\n=== Scanner Boot ===\r\n");

    /* 2. NVS — required by health and any future NVS-backed component. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Partition was truncated or version changed — erase and retry. */
        if (nvs_flash_erase() != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase failed — restarting");
            esp_restart();
        }
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s — restarting",
                 esp_err_to_name(ret));
        esp_restart();
    }

    /* 3. Output subsystem — establishes device identity in JSON envelopes.
     *    Fatal: without it subsequent output_write calls have no identity. */
    ret = output_init("scanner", CONFIG_SCANNER_DEVICE_ID);
    if (ret != ESP_OK) {
        /* output_init failed before output_write is usable; use printf. */
        printf("FATAL: output_init failed (%s) — restarting\r\n",
               esp_err_to_name(ret));
        esp_restart();
    }

    /* 4. Watchdog monitor — must be running before any task is registered. */
    watchdog_start();

    /* 5. Health monitor — register BEFORE health_monitor_start creates task. */
    watchdog_register_task("scanner_health", 60000);
    ret = health_monitor_start("scanner_health");
    if (ret != ESP_OK) {
        output_write("{\"type\":\"warning\","
                     "\"msg\":\"health_monitor_start failed\"}");
        /* Non-fatal: scanner can run without health reports. */
        ESP_LOGW(TAG, "health_monitor_start failed: %s", esp_err_to_name(ret));
    }

    /* 6. Scanner core — creates the dummy scan timer and core task.
     *    Timer is created but NOT started here; SCAN BLE START does that. */
    ret = scanner_core_init();
    if (ret != ESP_OK) {
        output_write("{\"type\":\"fatal\",\"msg\":\"scanner_core_init failed\"}");
        esp_restart();
    }

    /* 7. Scanner link — opens UART1, creates the link task for incoming
     *    scanner_cmd JSON objects from the Commander. */
    ret = scanner_link_init();
    if (ret != ESP_OK) {
        output_write("{\"type\":\"fatal\",\"msg\":\"scanner_link_init failed\"}");
        esp_restart();
    }

    /* 8. Wire scan events to the link send function.
     *    scanner_core and scanner_link are independent components; main.c
     *    connects them via a callback so neither imports the other. */
    scanner_core_register_event_cb(scanner_link_send);

    /* 9. Local command console on UART2. */
    ret = scanner_console_init();
    if (ret != ESP_OK) {
        output_write("{\"type\":\"warning\","
                     "\"msg\":\"scanner_console_init failed\"}");
        /* Non-fatal: scanner transmits data without a local console. */
        ESP_LOGW(TAG, "scanner_console_init failed: %s", esp_err_to_name(ret));
    }

    /* 10. Boot-ready signal. Goes to printf/UART0 for development monitoring.
     *     Commander ignores it (no scan_result type at top level). */
    output_write("{\"type\":\"boot\",\"status\":\"ready\"}");
    ESP_LOGI(TAG, "Scanner ready — device_id=%s", CONFIG_SCANNER_DEVICE_ID);

    /* 11. app_main must not return; park in a low-frequency idle loop. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
