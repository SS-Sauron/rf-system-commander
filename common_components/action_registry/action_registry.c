/**
 * @file action_registry.c
 * @brief Action module registry — real implementation (replaces C3 stub).
 *
 * COMPONENT LOCATION: common_components/action_registry/
 *
 * CHANGED AT C4:
 *   - action_module_t struct and ACTION_MODULE_API_VERSION moved to header.
 *   - Static module pointer table replaces the single-function stub.
 *   - action_registry_init() and action_registry_register() are new.
 *   - action_registry_execute() now performs real name-based lookup and
 *     dispatch instead of logging a stub message.
 *   - The "action_triggered" output type is gone. Error paths emit
 *     "action_error"; success output comes from the module's own execute().
 *
 * C4 REPLACEMENT GUIDE (for any future reader comparing C3 vs C4):
 *   The entire body of action_registry_execute() was the C3 stub.
 *   The new body is the lookup + dispatch logic below. The header API
 *   (function signature and return values) is unchanged from C3.
 */

#include "action_registry.h"
#include "output.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "action_registry";

/* ---- Compile-time defaults ------------------------------------------- */

#ifndef CONFIG_MAX_ACTION_MODULES
#define CONFIG_MAX_ACTION_MODULES 8
#endif

/* Error output body buffer.
 * Max content: {"type":"action_error","module":"<31>","reason":"execute_failed",
 *               "esp_err":"ESP_ERR_INVALID_STATE"} ≈ 120 chars. 192 is safe. */
#define ACTION_ERR_BUF_LEN  192

/* ---- Module state ----------------------------------------------------- */

static bool              s_initialized  = false;
static action_module_t  *s_modules[CONFIG_MAX_ACTION_MODULES];
static uint8_t           s_module_count = 0;

/* Static error output buffer. Single-task use: action_registry_execute()
 * is only called from rule_engine_task, which is single-threaded. */
static char s_err_buf[ACTION_ERR_BUF_LEN];

/* ======================================================================
 * Internal helpers
 * ====================================================================== */

/** Emit a JSON error line via output_write. Never crashes on format overflow;
 *  falls back to a minimal fixed string instead. */
static void emit_error(const char *module_name, const char *reason,
                       const char *esp_err_str)
{
    int w;
    if (esp_err_str != NULL) {
        w = snprintf(s_err_buf, sizeof(s_err_buf),
            "{\"type\":\"action_error\",\"module\":\"%s\","
            "\"reason\":\"%s\",\"esp_err\":\"%s\"}",
            module_name, reason, esp_err_str);
    } else {
        w = snprintf(s_err_buf, sizeof(s_err_buf),
            "{\"type\":\"action_error\",\"module\":\"%s\","
            "\"reason\":\"%s\"}",
            module_name, reason);
    }

    if (w > 0 && w < (int)sizeof(s_err_buf)) {
        output_write(s_err_buf);
    } else {
        /* Overflow fallback — emit a minimal known-valid JSON line. */
        output_write("{\"type\":\"action_error\",\"reason\":\"buf_overflow\"}");
    }
}

/* ======================================================================
 * Public API
 * ====================================================================== */

esp_err_t action_registry_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "action_registry_init called more than once — ignored");
        return ESP_ERR_INVALID_STATE;
    }

    for (int i = 0; i < CONFIG_MAX_ACTION_MODULES; i++) {
        s_modules[i] = NULL;
    }
    s_module_count = 0;
    s_initialized  = true;

    ESP_LOGI(TAG, "Action registry ready (capacity: %d modules)",
             CONFIG_MAX_ACTION_MODULES);
    return ESP_OK;
}

esp_err_t action_registry_register(action_module_t *module)
{
    /* ---- Pre-condition checks ---- */
    if (!s_initialized) {
        ESP_LOGE(TAG, "register called before action_registry_init");
        return ESP_ERR_INVALID_STATE;
    }
    if (module == NULL) {
        ESP_LOGE(TAG, "register: module pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    if (module->api_version != ACTION_MODULE_API_VERSION) {
        ESP_LOGE(TAG, "register: api_version mismatch (got %u, expected %u)",
                 module->api_version, ACTION_MODULE_API_VERSION);
        return ESP_ERR_INVALID_ARG;
    }
    if (module->name == NULL) {
        ESP_LOGE(TAG, "register: module->name is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    if (module->execute == NULL) {
        ESP_LOGE(TAG, "register: module '%s' has NULL execute pointer", module->name);
        return ESP_ERR_INVALID_ARG;
    }
    if (module->init == NULL) {
        ESP_LOGE(TAG, "register: module '%s' has NULL init pointer", module->name);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_module_count >= CONFIG_MAX_ACTION_MODULES) {
        ESP_LOGE(TAG, "register: table full (%d/%d) — increase CONFIG_MAX_ACTION_MODULES",
                 s_module_count, CONFIG_MAX_ACTION_MODULES);
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- Duplicate name check ---- */
    for (int i = 0; i < s_module_count; i++) {
        if (strcmp(s_modules[i]->name, module->name) == 0) {
            ESP_LOGE(TAG, "register: module '%s' already registered at slot %d",
                     module->name, i);
            return ESP_ERR_INVALID_STATE;
        }
    }

    /* ---- Call module->init() ---- */
    /* Store the pointer first so the module is visible for status queries
     * even if init fails (available=false). Callers can then call get_status()
     * to report the failure condition. */
    module->available = false;
    s_modules[s_module_count] = module;
    s_module_count++;

    esp_err_t init_ret = module->init();

    if (init_ret == ESP_OK) {
        /* Registry owns the available flag. Set it only after successful init. */
        module->available = true;
        ESP_LOGI(TAG, "Registered '%s' (slot %u, api_version=%u)",
                 module->name, (unsigned)(s_module_count - 1), module->api_version);
        return ESP_OK;
    } else {
        /* init() failed: module is stored but not available.
         * The caller (main.c) decides whether this is fatal. */
        ESP_LOGW(TAG, "Module '%s' init failed (%s) — stored but not available",
                 module->name, esp_err_to_name(init_ret));
        return init_ret;
    }
}

esp_err_t action_registry_execute(const char *module_name,
                                  const char *command_json)
{
    /* ---- Input validation ---- */
    if (module_name == NULL) {
        ESP_LOGE(TAG, "execute: module_name is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    if (command_json == NULL || command_json[0] != '{') {
        ESP_LOGE(TAG, "execute: command_json is NULL or does not start with '{'");
        emit_error(module_name ? module_name : "?",
                   "invalid_command", NULL);
        return ESP_ERR_INVALID_ARG;
    }

    /* ---- Name-based linear lookup ---- */
    /* O(N) over CONFIG_MAX_ACTION_MODULES (default 8). With N ≤ 8 this is
     * faster than any hash-table overhead and fits the no-heap constraint. */
    action_module_t *found = NULL;
    for (int i = 0; i < s_module_count; i++) {
        if (s_modules[i] != NULL &&
            strcmp(s_modules[i]->name, module_name) == 0) {
            found = s_modules[i];
            break;
        }
    }

    if (found == NULL) {
        ESP_LOGW(TAG, "execute: module '%s' not registered", module_name);
        emit_error(module_name, "not_registered", NULL);
        return ESP_ERR_NOT_FOUND;
    }

    /* ---- Availability check ---- */
    /* NOTE (C6): When bt_media can set available=false asynchronously from
     * a Bluedroid callback, this read of found->available requires a memory
     * barrier or atomic access. At C4, all state changes to available happen
     * only at boot (in main task context), so this is safe. Revisit at C6. */
    if (!found->available) {
        ESP_LOGW(TAG, "execute: module '%s' is not available (init failed?)",
                 module_name);
        emit_error(module_name, "not_available", NULL);
        return ESP_ERR_INVALID_STATE;
    }

    /* ---- Dispatch ---- */
    /* LOCK ORDERING: caller (rule_engine_task) holds rule_table_mutex.
     * module->execute() may call output_write() which acquires output_mutex.
     * Order enforced: rule_table_mutex → output_mutex.
     * No code path may acquire output_mutex first and then rule_table_mutex. */
    esp_err_t exec_ret = found->execute(command_json);

    if (exec_ret != ESP_OK) {
        ESP_LOGW(TAG, "execute: module '%s' returned %s",
                 module_name, esp_err_to_name(exec_ret));
        emit_error(module_name, "execute_failed", esp_err_to_name(exec_ret));
    }

    /* No "action_triggered" or "action_executed" log here.
     * The module's own execute() is responsible for its output.
     * The registry only emits output on error paths. */
    return exec_ret;
}

/* ======================================================================
 * C5 Accessor API
 * ====================================================================== */

int action_registry_get_module_count(void)
{
    /* s_module_count is only written during boot-time registration;
     * safe to read without a lock from any task afterwards. */
    return (int)s_module_count;
}

const action_module_t *action_registry_get_module(const char *name)
{
    if (name == NULL) { return NULL; }
    for (int i = 0; i < (int)s_module_count; i++) {
        if (s_modules[i] != NULL &&
            strcmp(s_modules[i]->name, name) == 0) {
            return s_modules[i];
        }
    }
    return NULL;
}
