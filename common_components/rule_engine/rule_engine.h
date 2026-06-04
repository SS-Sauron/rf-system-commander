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
#include <stdbool.h>
#include <stddef.h>

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

/* ======================================================================
 * Rule Management API  (added at C5 for the command parser)
 *
 * All functions that modify s_rules[] acquire s_rule_mutex for the
 * in-memory operation. NVS writes happen OUTSIDE the mutex so that the
 * evaluation task is not blocked during flash I/O.
 *
 * "Free slot" detection: a slot is free when name[0] == '\0'.
 *   active=true,  name[0]!='\0' → occupied and enabled
 *   active=false, name[0]!='\0' → occupied but disabled (RULE DISABLE)
 *   active=false, name[0]=='\0' → free (never used, or deleted)
 * ====================================================================== */

/**
 * @brief Add a new rule from a JSON string.
 *
 * Parses and validates json, finds the first free slot in the in-memory
 * table, writes to NVS, then populates the slot. Two separate mutex
 * acquisitions keep the mutex hold time short and NVS I/O outside.
 *
 * @param json       Null-terminated rule JSON (compact NVS format).
 * @param out_index  Set to the assigned rule index on success.
 *
 * @return  ESP_OK              Rule added.
 *          ESP_ERR_INVALID_ARG json is NULL or fails parse/validation.
 *          ESP_ERR_NO_MEM      Rule table is full (all slots occupied).
 *          ESP_ERR_TIMEOUT     Mutex acquisition timed out.
 *          (NVS error)         JSON validated but NVS write failed.
 */
esp_err_t rule_engine_add_rule(const char *json, int *out_index);

/**
 * @brief Delete the rule at the given index.
 *
 * Clears the in-memory slot (name[0]='\0') and erases the NVS key.
 *
 * @return  ESP_OK              Deleted (or NVS erase failed but memory cleared).
 *          ESP_ERR_INVALID_ARG index out of range.
 *          ESP_ERR_NOT_FOUND   Slot was already free.
 *          ESP_ERR_TIMEOUT     Mutex acquisition timed out.
 */
esp_err_t rule_engine_delete_rule(int index);

/**
 * @brief Enable or disable the rule at the given index.
 *
 * Updates the active flag in memory and re-serialises the rule to NVS
 * with the updated "on" field. Does not affect the rule's other fields.
 *
 * @param enabled  true = enable, false = disable.
 *
 * @return  ESP_OK              Updated.
 *          ESP_ERR_INVALID_ARG index out of range.
 *          ESP_ERR_NOT_FOUND   Slot is free (no rule at that index).
 *          ESP_ERR_TIMEOUT     Mutex acquisition timed out.
 *          ESP_ERR_INVALID_SIZE Serialized JSON too large for buffer.
 */
esp_err_t rule_engine_set_enabled(int index, bool enabled);

/**
 * @brief Format a human-readable summary of all occupied rule slots.
 *
 * Acquires the mutex, iterates s_rules[], and writes formatted lines
 * into buffer. Thread-safe; safe to call from command_parser_task.
 *
 * @param buffer    Caller-provided output buffer.
 * @param buf_size  Size of buffer in bytes.
 *
 * @return  ESP_OK on success, ESP_ERR_TIMEOUT on mutex timeout.
 */
esp_err_t rule_engine_get_all_rules(char *buffer, size_t buf_size);

/**
 * @brief Evaluate an ad-hoc event JSON against all enabled rules (dry run).
 *
 * Does NOT dispatch any actions. Reports which rules would fire and what
 * commands would be sent.
 *
 * Event JSON format: {"module":"<name>","data":[{...},{...}]}
 * The "type" field is not required (unlike the full scan_result envelope).
 *
 * @param event_json  Null-terminated event JSON string.
 * @param buffer      Caller-provided output buffer for the report.
 * @param buf_size    Size of buffer in bytes.
 *
 * @return  ESP_OK on success, ESP_ERR_INVALID_ARG if JSON is malformed.
 */
esp_err_t rule_engine_test_event(const char *event_json,
                                 char *buffer, size_t buf_size);

/**
 * @brief Return the number of occupied (non-free) rule slots.
 *
 * Counts slots where name[0] != '\0' (both enabled and disabled rules).
 *
 * @return Number of occupied slots, or -1 on mutex timeout.
 */
int rule_engine_get_count(void);

#ifdef __cplusplus
}
#endif
