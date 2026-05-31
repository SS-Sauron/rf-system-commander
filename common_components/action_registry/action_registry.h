/**
 * @file action_registry.h
 * @brief Action module interface definition and registry API.
 *
 * COMPONENT LOCATION: common_components/action_registry/
 *
 * UPDATED AT C4: This header now contains three things that were separate
 * in C3:
 *   1. The action_module_t interface that every action module implements.
 *   2. ACTION_MODULE_API_VERSION — the version every module must declare.
 *   3. The registry's public API (init, register, execute).
 *
 * Every action module (#include "action_registry.h") gets both the type
 * definition and the API in a single include with no circular dependency.
 *
 * =========================================================================
 * action_module_t  INTERFACE CONTRACT
 * =========================================================================
 *
 * api_version  Must equal ACTION_MODULE_API_VERSION at definition time.
 *              action_registry_register() rejects modules with a mismatch.
 *              Increment ACTION_MODULE_API_VERSION whenever the struct
 *              layout or any function signature changes.
 *
 * name         Null-terminated module identifier used for name-based lookup.
 *              Must be a string literal or static buffer — never stack data.
 *              Examples: "dummy", "bt_media", "gpio".
 *
 * available    Set to true by action_registry_register() after a successful
 *              init() call. Set back to false by the registry after deinit().
 *              The MODULE MUST NOT write this flag itself. The registry owns
 *              the lifecycle to ensure execute() is never called before init()
 *              completes and never called after deinit() runs.
 *
 *              THREAD SAFETY NOTE (important at C6):
 *              At C4, all registrations happen at boot before any rule engine
 *              events arrive, so reads/writes of available are never concurrent.
 *              At C6, bt_media may set available=false from a Bluedroid callback
 *              running in a different task. That module must use a registry-
 *              provided mechanism (to be designed at C6) — it must NOT modify
 *              available directly. Flag this for the C6 design discussion.
 *
 * init()       Called once by action_registry_register(). Runs in the main
 *              task context at boot. Must complete quickly (no blocking I/O).
 *              For modules with internal tasks, create the task and queue here
 *              but do not wait for them to be ready. Return ESP_OK on success.
 *
 * deinit()     Stops the module's internal task (if any), deletes its queue,
 *              and releases all resources. Must leave the module in a state
 *              where init() can be called again without leaking.
 *
 * execute()    NON-BLOCKING CONTRACT — MANDATORY FOR ALL IMPLEMENTATIONS:
 *              Must return within microseconds. The rule engine calls
 *              execute() while holding rule_table_mutex. Any blocking here
 *              would block the command parser from editing rules.
 *
 *              For simple modules (dummy): call output_write() and return.
 *              For bt_media and other hardware modules: post the command to
 *              an internal FreeRTOS queue and return immediately. The real
 *              work happens in the module's own task.
 *
 *              Returns: ESP_OK          = command accepted for processing.
 *                       ESP_ERR_NO_MEM = internal queue full.
 *                       ESP_ERR_INVALID_ARG = json_command malformed.
 *
 * get_status() NULLABLE — set to NULL if not implemented.
 *              Writes a JSON status object into buf. *len is in/out:
 *              caller sets the buffer capacity; function sets bytes written
 *              (not including null terminator). The registry checks for NULL
 *              before calling. Useful for RULE TEST and STATUS commands (C5).
 *
 * =========================================================================
 * LOCK ORDERING  (must not be violated by any future module)
 * =========================================================================
 * The rule engine holds rule_table_mutex when it calls
 * action_registry_execute(), which in turn calls module->execute().
 * If execute() calls output_write(), it acquires output_mutex.
 *
 * Global lock order: rule_table_mutex → output_mutex
 *
 * No code path may acquire output_mutex and then rule_table_mutex.
 * Document this constraint in any module that touches both.
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Interface versioning --------------------------------------------- */

/**
 * Current action_module_t API version.
 * Increment this whenever the struct layout or any function signature changes.
 * action_registry_register() rejects modules whose api_version differs.
 */
#define ACTION_MODULE_API_VERSION  1U

/* ---- action_module_t -------------------------------------------------- */

/**
 * @brief Interface that every action module must implement.
 *
 * Define one static instance of this struct per module (in the module's .c
 * file). Pass a pointer to that instance to action_registry_register().
 * The registry stores the pointer; the module owns the memory.
 *
 * Initialise all fields. Set available = false in the definition; the
 * registry sets it to true after a successful init() call.
 */
typedef struct action_module {
    uint16_t    api_version;    /* Must equal ACTION_MODULE_API_VERSION */
    const char *name;           /* Module identifier, e.g. "dummy"      */
    bool        available;      /* Owned by registry — do not write here */

    /**
     * Called once by action_registry_register(). Must not block.
     * @return ESP_OK on success; any error prevents registration.
     */
    esp_err_t (*init)(void);

    /**
     * Stops the module's internal task and releases all resources.
     * @return ESP_OK on success.
     */
    esp_err_t (*deinit)(void);

    /**
     * NON-BLOCKING. Accepts a resolved action command and returns immediately.
     * @param json_command  Null-terminated JSON object, e.g. {"action":"play",...}.
     *                      Must start with '{'. Valid for the duration of the call
     *                      only — do not store the pointer; copy if needed.
     * @return ESP_OK / ESP_ERR_NO_MEM / ESP_ERR_INVALID_ARG.
     */
    esp_err_t (*execute)(const char *json_command);

    /**
     * Optional. NULL if not implemented.
     * Writes a JSON status object into buf. *len: in=capacity, out=bytes written.
     * @return ESP_OK / ESP_ERR_INVALID_SIZE if buf is too small.
     */
    esp_err_t (*get_status)(char *buf, size_t *len);
} action_module_t;

/* ---- Registry API ----------------------------------------------------- */

/**
 * @brief Initialise the action registry.
 *
 * Zeroes the module table. Must be called once in main.c before any
 * action_registry_register() calls. A second call returns
 * ESP_ERR_INVALID_STATE (ignored, no harm).
 *
 * @return ESP_OK on success.
 */
esp_err_t action_registry_init(void);

/**
 * @brief Register an action module.
 *
 * Validates the module, calls module->init(), and — on success — stores
 * the module pointer and sets module->available = true.
 *
 * On init() failure: module->available remains false, the module is still
 * stored (so get_status() can report the failure), and the init error is
 * returned to the caller. main.c decides whether this is fatal.
 *
 * @param module  Pointer to a statically-allocated action_module_t. The
 *                pointer must remain valid for the lifetime of the firmware.
 *
 * @return  ESP_OK                Module registered and available.
 *          ESP_ERR_INVALID_ARG   module is NULL, api_version mismatch,
 *                                name is NULL, or execute is NULL.
 *          ESP_ERR_INVALID_STATE Registry not initialised, or table full,
 *                                or a module with the same name already exists.
 *          (init error code)     Module stored but available=false.
 */
esp_err_t action_registry_register(action_module_t *module);

/**
 * @brief Dispatch a command to a named action module.
 *
 * Called by the rule engine when a rule fires. MUST return quickly —
 * the rule engine holds rule_table_mutex during this call.
 *
 * On error, emits a {"type":"action_error",...} JSON line via output_write
 * and returns the error code. Does NOT abort rule evaluation; the rule
 * engine logs the error and continues.
 *
 * @param module_name   Name of the target module (e.g. "dummy", "bt_media").
 * @param command_json  Resolved JSON command string (starts with '{').
 *
 * @return  ESP_OK               command accepted by the module's execute().
 *          ESP_ERR_INVALID_ARG  module_name or command_json is NULL or malformed.
 *          ESP_ERR_NOT_FOUND    No module with that name is registered.
 *          ESP_ERR_INVALID_STATE Module found but available=false.
 *          (execute error code)  Module's execute() returned an error.
 */
esp_err_t action_registry_execute(const char *module_name,
                                  const char *command_json);

#ifdef __cplusplus
}
#endif
