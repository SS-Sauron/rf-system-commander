/**
 * @file dummy_action.c
 * @brief Dummy action module implementation.
 *
 * COMPONENT LOCATION: common_components/dummy_action/
 *
 * PURPOSE
 * -------
 * The simplest valid action_module_t implementation. It has:
 *   - No internal FreeRTOS task.
 *   - No queue.
 *   - No hardware dependency.
 *
 * When execute() is called it immediately calls output_write() and returns.
 * This satisfies the non-blocking contract because output_write() acquires
 * output_mutex (100 ms timeout), writes a fixed-size string, and releases
 * the mutex — all bounded and fast.
 *
 * The output type "action_executed" (distinct from the C3 stub's
 * "action_triggered") proves unambiguously in the serial monitor that
 * the real registry is active and this module received the command.
 *
 * LOCK ORDERING
 * -------------
 * execute() is called by action_registry_execute() which is called by
 * rule_engine_task while holding rule_table_mutex. execute() calls
 * output_write() which acquires output_mutex.
 * Order: rule_table_mutex → output_mutex. Must not be inverted.
 *
 * EXTENDING THIS MODULE (for future testing)
 * ------------------------------------------
 * To simulate a blocking module (for testing the non-blocking contract):
 *   1. Add a FreeRTOS queue (created in init(), deleted in deinit()).
 *   2. execute() posts to the queue instead of calling output_write().
 *   3. An internal task reads the queue and calls output_write().
 * This is exactly the pattern bt_media will use at C6.
 */

#include "dummy_action.h"
#include "output.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "dummy_action";

/* ---- Compile-time defaults ------------------------------------------- */

/* Body buffer: fixed overhead (~80 chars) + command JSON (max ACTION_CMD_BUF).
 * Using OUTPUT_LINE_BUF_SIZE as the ceiling keeps us within output_write's
 * envelope capacity by construction. */
#ifndef CONFIG_OUTPUT_LINE_BUF_SIZE
#define CONFIG_OUTPUT_LINE_BUF_SIZE 2048
#endif

#ifndef CONFIG_ACTION_CMD_BUF_SIZE
#define CONFIG_ACTION_CMD_BUF_SIZE 512
#endif

/* Largest body we will ever produce:
 *   {"type":"action_executed","module":"dummy","exec_num":4294967295,"cmd":<cmd>}
 *   Fixed overhead: ~78 chars. Cmd: up to CONFIG_ACTION_CMD_BUF_SIZE.
 * Cap at OUTPUT_LINE_BUF_SIZE to guarantee output_write never overflows. */
#define DUMMY_BODY_BUF_SIZE  CONFIG_OUTPUT_LINE_BUF_SIZE

/* ---- Module state ----------------------------------------------------- */

/* Execution counter. Incremented on every successful execute() call.
 * No thread safety needed at C4: execute() is only called from
 * rule_engine_task (single caller). Revisit if multiple callers are added. */
static uint32_t s_exec_count = 0;

/* Static body buffer. Single-task use — see note above. */
static char s_body[DUMMY_BODY_BUF_SIZE];

/* ======================================================================
 * Interface implementation
 * ====================================================================== */

static esp_err_t dummy_init(void)
{
    s_exec_count = 0;
    ESP_LOGI(TAG, "Dummy action module initialised");
    return ESP_OK;
}

static esp_err_t dummy_deinit(void)
{
    ESP_LOGI(TAG, "Dummy action module stopped (total executions: %lu)",
             (unsigned long)s_exec_count);
    /* Nothing to clean up — no task, no queue, no hardware. */
    return ESP_OK;
}

static esp_err_t dummy_execute(const char *json_command)
{
    /* Validate — the registry checks this too, but defence-in-depth. */
    if (json_command == NULL || json_command[0] != '{') {
        ESP_LOGE(TAG, "execute: json_command is NULL or not a JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    /* Format the output body.
     * embed json_command as a nested object (not a string) so the host
     * parser sees:  "cmd":{"action":"play","target":"AA:BB:..."}
     * not:          "cmd":"{\"action\":\"play\",...}"               */
    int written = snprintf(s_body, sizeof(s_body),
        "{\"type\":\"action_executed\","
        "\"module\":\"dummy\","
        "\"exec_num\":%lu,"
        "\"cmd\":%s}",
        (unsigned long)(s_exec_count + 1),
        json_command);

    if (written < 0 || written >= (int)sizeof(s_body)) {
        /* json_command is larger than our buffer allows. This means
         * CONFIG_ACTION_CMD_BUF_SIZE > CONFIG_OUTPUT_LINE_BUF_SIZE - 80,
         * which is a Kconfig misconfiguration. Log and report. */
        ESP_LOGE(TAG, "execute: body overflow (%d bytes). "
                      "Check CONFIG_OUTPUT_LINE_BUF_SIZE >= "
                      "CONFIG_ACTION_CMD_BUF_SIZE + 80", written);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = output_write(s_body);
    if (ret == ESP_OK) {
        s_exec_count++;
    } else {
        ESP_LOGW(TAG, "execute: output_write failed (%s)", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t dummy_get_status(char *buf, size_t *len)
{
    if (buf == NULL || len == NULL || *len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* dummy_action_module.available is read here (not written).
     * The registry owns writes; we only observe. Safe for single-task use. */
    int written = snprintf(buf, *len,
        "{\"module\":\"dummy\","
        "\"available\":%s,"
        "\"executions\":%lu}",
        dummy_action_module.available ? "true" : "false",
        (unsigned long)s_exec_count);

    if (written < 0 || written >= (int)*len) {
        return ESP_ERR_INVALID_SIZE;
    }
    *len = (size_t)written;   /* out: bytes written, not including '\0' */
    return ESP_OK;
}

/* ======================================================================
 * Module definition — the one symbol exported by this component
 * ====================================================================== */

/**
 * The dummy action module instance.
 *
 * - available is initialised to false; the registry sets it to true after
 *   a successful init() call.
 * - name must be a string literal (static storage) — never stack data.
 * - All function pointers must be non-NULL except get_status (nullable).
 */
action_module_t dummy_action_module = {
    .api_version = ACTION_MODULE_API_VERSION,
    .name        = "dummy",
    .available   = false,   /* registry sets this to true after init() */
    .init        = dummy_init,
    .deinit      = dummy_deinit,
    .execute     = dummy_execute,
    .get_status  = dummy_get_status,
};
