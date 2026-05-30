/**
 * @file rule_engine.h
 * @brief Rule engine — public API.
 *
 * COMPONENT LOCATION: common_components/rule_engine/
 *
 * The rule engine is a FreeRTOS task that:
 *   1. Consumes scan_event_t structs from a queue (produced by scan_parser).
 *   2. Evaluates each event against a static in-RAM rule table loaded from NVS.
 *   3. For each matching rule, resolves $item.field placeholders in the
 *      action command template using values from the matched scan item.
 *   4. Dispatches the resolved command via action_registry_execute().
 *
 * RULE SCHEMA (stored in NVS namespace "rules", one key per rule)
 * ---------------------------------------------------------------
 *   {"name":"play_on_speaker",
 *    "on":1,
 *    "cm":"bt_classic",
 *    "cf":"name",
 *    "co":"contains",
 *    "cv":"Speaker",
 *    "am":"bt_media",
 *    "ac":{"action":"play","target":"$item.bdaddr"},
 *    "fm":0}
 *
 *   on=1 enabled, on=0 disabled (kept in NVS but skipped during evaluation)
 *   cm   condition module ("wifi", "bt_classic", "ble", ...)
 *   cf   condition field  (any key present in a data[] item object)
 *   co   operator: "eq" | "ne" | "contains" | "gt" | "lt"
 *   cv   comparison value (string or number as string, e.g. "-70")
 *   am   action module name ("bt_media", "gpio", ...)
 *   ac   action command template (nested JSON object; placeholders allowed)
 *   fm   fire mode: 0 = first matching item only (default), 1 = all matches
 *
 * PLACEHOLDER SYNTAX
 * ------------------
 *   $item.<field>  — replaced with the value of <field> from the matched
 *                    scan item (e.g. $item.bdaddr, $item.ssid, $item.rssi).
 *   Up to CONFIG_RULE_MAX_PLACEHOLDERS placeholders per command template.
 *
 * C5 EXTENSION POINTS
 * -------------------
 *   rule_engine_add()    — add/overwrite a rule slot from the command parser
 *   rule_engine_delete() — delete a rule slot
 *   rule_engine_list()   — iterate active rules (for RULE LIST command)
 *   These functions will be added to this header at C5.
 */

#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the rule engine component.
 *
 * Loads rules from NVS ("rules" namespace), populates the in-RAM rule
 * table, registers with the watchdog, and starts the evaluation task.
 *
 * If NVS contains no rules the engine starts with an empty table and
 * runs idle (feeding the watchdog until rules are added via C5 commands).
 *
 * Rules that fail to parse at load time are logged and marked inactive;
 * other rule slots continue loading normally.
 *
 * Must be called after:
 *   - output_init()
 *   - watchdog_start()
 *   - nvs_flash_init()
 *
 * @param event_queue  Queue from which scan_event_t structs are consumed.
 *                     Item size must be sizeof(scan_event_t).
 *                     Must not be NULL.
 *
 * @return  ESP_OK              Ready and running.
 *          ESP_ERR_INVALID_ARG event_queue is NULL.
 *          ESP_ERR_INVALID_STATE Already initialised.
 *          ESP_FAIL            Task or mutex creation failed.
 */
esp_err_t rule_engine_init(QueueHandle_t event_queue);

/**
 * @brief Stop the rule engine task and release internal state.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not initialised.
 */
esp_err_t rule_engine_deinit(void);

#ifdef __cplusplus
}
#endif
