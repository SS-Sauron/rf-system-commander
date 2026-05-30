/**
 * @file main.c
 * @brief Commander ESP32 — application entry point (Stage C0 skeleton).
 *
 * This file is the orchestrator. Its only jobs are:
 *   1. Initialise infrastructure in the correct order.
 *   2. Spawn tasks (via module init functions).
 *   3. Yield to the scheduler.
 *
 * It does NOT contain business logic. Scan parsing, rule evaluation,
 * action dispatch, and command handling each live in their own modules.
 *
 * Initialisation order (and why it matters):
 *   a) NVS first — rules and settings live here; everything else may
 *      read NVS during its own init.
 *   b) output_init() second — from this point on, all user-visible output
 *      goes through output_write(). Modules started after this point can
 *      report errors via the JSON channel.
 *   c) watchdog_start() third — the watchdog monitor must be running
 *      before any task it monitors is started.
 *   d) health_monitor_start() / scan_receiver_init() — started after
 *      watchdog, and each registers itself with the watchdog only when
 *      the underlying task has been created successfully.
 *
 * STAGES: Future stages will insert calls between the existing ones.
 *   C1: scanner_input_init()   — UART RX driver for scanner JSON lines
 *   C2: scan_parser_init()     — JSON envelope parser
 *   C3: rule_engine_init()     — rule table load + evaluation task
 *   C4: action_registry_init() + dummy_action register
 *   C5: cmd_init()             — local serial command parser
 *   C6: bt_media_action_init() — Bluedroid AVRCP controller
 *
 * HARDWARE: See board_config.h for all pin assignments.
 */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"

#include "board_config.h"
#include "health.h"
#include "output.h"
#include "watchdog.h" /* <-- new: heartbeat-based task monitoring */
#include "scan_receiver.h"
#include "scan_parser.h" /* <-- new: JSON envelope parser for scanner lines */
#include "rule_engine.h"
#include "action_registry.h"

// #define SCAN_RX_QUEUE_DEPTH SCAN_RECEIVER_QUEUE_DEPTH // aliased for clarity

static const char *TAG = "main";

/* Temporary seed function for C3 testing — REMOVE at C5 */
static void seed_test_rule_if_empty(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open("rules", NVS_READWRITE, &h);
    if (ret != ESP_OK)
        return;

    uint16_t count = 0;
    nvs_get_u16(h, "rule_count", &count);

    if (count == 0)
    {
        /* Rule: when bt_classic scan finds a device whose name contains
         * "Speaker", trigger bt_media play action on that device. */
        const char *rule_json =
            "{\"name\":\"test_play\","
            "\"on\":1,"
            "\"cm\":\"bt_classic\","
            "\"cf\":\"name\","
            "\"co\":\"contains\","
            "\"cv\":\"Speaker\","
            "\"am\":\"bt_media\","
            "\"ac\":{\"action\":\"play\",\"target\":\"$item.bdaddr\"},"
            "\"fm\":0}";

        nvs_set_str(h, "rule_0", rule_json);
        uint16_t new_count = 1;
        nvs_set_u16(h, "rule_count", new_count);
        nvs_commit(h);
        ESP_LOGI("main", "Seeded test rule_0 into NVS");
    }

    nvs_close(h);
}

/* ======================================================================
 * app_main
 * ====================================================================== */

void app_main(void)
{
    /* ---- Boot banner -------------------------------------------------- */
    /* printf only: output_init has not run yet, output_write is unavailable.
     * This is the ONLY printf in the codebase after C0. Everything else
     * routes through output_write (JSON) or ESP_LOGx (diagnostics). */
    printf("Commander Boot\n");

    /* ================================================================== */
    /* a) NVS Flash                                                        */
    /* ================================================================== */
    /* Do NOT call nvs_flash_erase() on failure. An erase here would
     * silently destroy all user rules stored from a previous session.
     * Instead, restart the device so the bootloader can retry. The
     * user can manually erase NVS if this becomes a persistent loop. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGE(TAG,
                 "NVS init failed: %s. Restarting to retry.",
                 esp_err_to_name(ret));
        esp_restart();
    }
    ESP_LOGI(TAG, "NVS OK");

    /* ================================================================== */
    /* b) Output channel                                                   */
    /* ================================================================== */
    /* Must be initialised before any module that calls output_write().
     * source:    "commander" — identifies this device role in the JSON stream.
     * device_id: CONFIG_DEVICE_ID — from Kconfig, unique per unit.           */
    ret = output_init("commander", CONFIG_DEVICE_ID);
    if (ret != ESP_OK)
    {
        /* output channel is critical — without it the commander cannot report
         * any scan events or action results. Log the error and restart so the
         * bootloader can retry. */
        printf("[main] FATAL: output_init failed (%s) — restarting\n",
               esp_err_to_name(ret));
        esp_restart();
    }
    ESP_LOGI(TAG, "Output channel ready (C0: printf transport)");

    /* ================================================================== */
    /* c) Watchdog monitor                                                 */
    /* ================================================================== */
    /* The dedicated watchdog monitor task (watchdog_task) must be running
     * before any task it monitors. It feeds the hardware TWDT and checks
     * heartbeats from all registered tasks.
     *
     * Registrations for individual tasks are done AFTER the corresponding
     * task has been created successfully, to avoid monitoring a task that
     * never starts. */
    watchdog_start();
    ESP_LOGI(TAG, "Watchdog monitor started");

    /* ================================================================== */
    /* d) Health monitor                                                   */
    /* ================================================================== */
    /* Register with watchdog BEFORE creating the health task.
     * This avoids the "tried to feed but is not registered" warning
     * and guarantees the watchdog is watching from the very first
     * heartbeat. */
    esp_err_t wd_ret = watchdog_register_task("cmd_health", 60000);
    if (wd_ret != ESP_OK)
    {
        output_write("{\"type\":\"warning\",\"msg\":\"watchdog registration for health failed\"}");
        ESP_LOGE(TAG, "watchdog_register_task('cmd_health'): %s", esp_err_to_name(wd_ret));
        /* Continue — the system runs without watchdog protection for the
         * health task.  A hung health task will not be detected. */
    }

    ret = health_monitor_start("cmd_health");
    if (ret != ESP_OK)
    {
        output_write("{\"type\":\"warning\",\"msg\":\"health monitor failed to start\"}");
        ESP_LOGE(TAG, "health_monitor_start: %s", esp_err_to_name(ret));
        /* The watchdog was already registered but the task never started,
         * so the watchdog will reset the device after 60 seconds.
         * This is the safe fallback for a critical task-creation failure. */
    }

    /* ================================================================== */
    /* e) Scanner data receiver & parser (Stages C1 / C2)                 */
    /* ================================================================== */

    /* Create the queue that carries raw scanner lines to the parser.
     * The same queue is used by scan_receiver (producer) and scan_parser
     * (consumer). */
    QueueHandle_t scan_rx_queue = xQueueCreate(SCAN_RECEIVER_QUEUE_DEPTH,
                                               SCAN_RECEIVER_LINE_BUF);
    if (scan_rx_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create scan_rx_queue — restarting");
        esp_restart();
    }

    ret = scan_receiver_init(scan_rx_queue);
    if (ret != ESP_OK)
    {
        output_write("{\"type\":\"fatal\",\"msg\":\"scan_receiver_init failed\"}");
        ESP_LOGE(TAG, "scan_receiver_init: %s", esp_err_to_name(ret));
        esp_restart();
    }

    /* ================================================================== */
    /* f) Scan parser + Rule engine (Stage C2 / C3)                       */
    /* ================================================================== */

    /* Create the queue that carries parsed events from parser to rule engine */
    QueueHandle_t scan_event_queue = xQueueCreate(CONFIG_RULE_ENGINE_QUEUE_DEPTH,
                                                  sizeof(scan_event_t));
    if (scan_event_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create scan_event_queue — restarting");
        esp_restart();
    }

    ret = scan_parser_init(scan_rx_queue, scan_event_queue);
    if (ret != ESP_OK)
    {
        output_write("{\"type\":\"fatal\",\"msg\":\"scan_parser_init failed\"}");
        ESP_LOGE(TAG, "scan_parser_init: %s", esp_err_to_name(ret));
        esp_restart();
    }

    ret = rule_engine_init(scan_event_queue);
    if (ret != ESP_OK)
    {
        output_write("{\"type\":\"fatal\",\"msg\":\"rule_engine_init failed\"}");
        ESP_LOGE(TAG, "rule_engine_init: %s", esp_err_to_name(ret));
        esp_restart();
    }

    /* ---- C3: seed test rule (remove at C5) ---- */
    seed_test_rule_if_empty();

    /* ================================================================== */
    /* System ready                                                        */
    /* ================================================================== */
    /* First structured JSON line on the output stream. The host parser
     * uses this to detect a fresh boot and resync its state. */
    ret = output_write("{\"type\":\"boot\",\"status\":\"ready\"}");
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to write boot-ready message: %s", esp_err_to_name(ret));
    }

    /* ================================================================== */
    /* Main loop                                                           */
    /* ================================================================== */
    /* app_main becomes a low-priority idle backstop. All real work runs
     * in FreeRTOS tasks spawned by the module init functions above.
     * The loop has no work to do; it exists to keep the task alive so the
     * scheduler does not delete the main stack prematurely.
     *
     * This task does NOT register with the TWDT — the health task and the
     * watchdog monitor handle all watchdog concerns. */
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}