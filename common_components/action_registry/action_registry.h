/**
 * @file action_registry.h
 * @brief Action module registry — non-blocking dispatch interface.
 *
 * COMPONENT LOCATION: common_components/action_registry/
 *
 * PURPOSE
 * -------
 * Provides the single function through which the rule engine dispatches
 * matched actions. The rule engine has NO compile-time dependency on any
 * specific action module (bt_media, gpio, etc.) — it depends only on this
 * header.
 *
 * STAGE EVOLUTION
 * ---------------
 *   C3:  Stub implementation. Logs the dispatch via output_write() and
 *        returns ESP_OK. No action is actually performed. Used to verify
 *        that rule matching and placeholder substitution work correctly
 *        before any real action module exists.
 *
 *   C4:  Real implementation. action_registry_execute() looks up
 *        module_name in a registered module table, calls
 *        module->execute(command_json), and returns the result.
 *        The header is UNCHANGED between C3 and C4.
 *
 * NON-BLOCKING CONTRACT  ← enforced at all stages
 * -----------------------------------------------
 * action_registry_execute() MUST return within a few microseconds.
 * It is called by the rule engine while the rule_table_mutex is held.
 * Blocking here would block the command parser (C5) from editing rules.
 *
 * At C3: the only work done is snprintf + output_write (fast).
 * At C4: the real action modules queue their commands to an internal
 *        FreeRTOS task and return immediately. They NEVER block inline.
 *
 * This contract must be stated in any action module's execute() docstring:
 *   "execute() queues the command and returns ESP_OK. The command is
 *    processed asynchronously by the module's internal task."
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Dispatch a command to a named action module.
 *
 * The rule engine calls this function when a rule condition matches.
 * The function MUST return quickly (see NON-BLOCKING CONTRACT above).
 *
 * @param module_name   Null-terminated name of the target action module,
 *                      e.g. "bt_media", "gpio", "wifi_deauth".
 *                      Must match the name field of a registered
 *                      action_module_t (C4+).
 * @param command_json  Null-terminated JSON object string with the
 *                      resolved (placeholder-substituted) action command,
 *                      e.g. {"action":"play","target":"AA:BB:CC:DD:EE:FF"}.
 *                      Must start with '{'. The string is read and released
 *                      before the function returns; the caller may reuse
 *                      the buffer immediately after this call returns.
 *
 * @return  ESP_OK              Command accepted (C3: logged; C4+: queued).
 *          ESP_ERR_INVALID_ARG module_name or command_json is NULL, or
 *                              command_json does not start with '{'.
 *          ESP_ERR_NOT_FOUND   (C4+) No module with the given name is
 *                              registered or available.
 *          ESP_ERR_NO_MEM      (C4+) Module's internal command queue full.
 */
esp_err_t action_registry_execute(const char *module_name,
                                  const char *command_json);

#ifdef __cplusplus
}
#endif
