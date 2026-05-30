/**
 * @file action_registry.c
 * @brief Action registry — Stage C3 stub implementation.
 *
 * COMPONENT LOCATION: common_components/action_registry/
 *
 * At C3 this file contains the stub. When C4 is implemented, this file
 * grows to include the module table and lookup logic. The header does
 * not change between C3 and C4.
 *
 * The C3 stub verifies the dispatch path end-to-end: when the rule engine
 * evaluates a matching event, the Serial Monitor will show:
 *
 *   {"source":"commander","device_id":"cmd_001","uptime_ms":NNN,
 *    "msg":{"type":"action_triggered","module":"bt_media",
 *           "cmd":{"action":"play","target":"AA:BB:CC:DD:EE:FF"}}}
 *
 * This confirms that rule matching, placeholder resolution, and dispatch
 * all work correctly before any real action module is written.
 *
 * C4 REPLACEMENT GUIDE
 * --------------------
 * 1. Add #include "action_module.h" and a static module table here.
 * 2. Replace the body of action_registry_execute() with a lookup loop
 *    that finds the module by name and calls module->execute(command_json).
 * 3. Keep the output_write() diagnostic call (guarded by a Kconfig flag
 *    CONFIG_ACTION_REGISTRY_VERBOSE_LOG if performance becomes a concern).
 * 4. The header stays identical; the rule engine recompiles without changes.
 */

#include "action_registry.h"
#include "output.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "action_registry";

/* Body buffer: static, protected by single-task use.
 * The rule engine calls this from rule_engine_task only.
 * Fixed overhead: ~60 chars envelope + module_name (max 31) +
 * command_json (max CONFIG_ACTION_CMD_BUF_SIZE).
 * Total bounded well within OUTPUT_LINE_BUF_SIZE (2048). */
#ifndef CONFIG_OUTPUT_LINE_BUF_SIZE
#define CONFIG_OUTPUT_LINE_BUF_SIZE 2048
#endif

#ifndef CONFIG_ACTION_CMD_BUF_SIZE
#define CONFIG_ACTION_CMD_BUF_SIZE 512
#endif

/* The output body buffer is sized to fit the action_triggered envelope.
 * It does not need to equal OUTPUT_LINE_BUF_SIZE exactly; it just must
 * fit within it after output.c wraps it. Use the same ceiling for safety. */
static char s_body[CONFIG_OUTPUT_LINE_BUF_SIZE];

esp_err_t action_registry_execute(const char *module_name,
                                  const char *command_json)
{
    /* ---- Input validation ---- */
    if (module_name == NULL || command_json == NULL) {
        ESP_LOGE(TAG, "action_registry_execute: NULL argument");
        return ESP_ERR_INVALID_ARG;
    }
    if (command_json[0] != '{') {
        ESP_LOGE(TAG, "command_json does not start with '{' (0x%02X)",
                 (unsigned char)command_json[0]);
        return ESP_ERR_INVALID_ARG;
    }

    /* ---- C3 STUB: log the dispatch via output_write ---- */
    /* command_json is already a JSON object, so we embed it directly
     * as a nested value (not a string) in the output body.
     * This produces:  "cmd":{"action":"play","target":"AA:BB:..."}
     * rather than:    "cmd":"{\"action\":\"play\",...}"             */
    int written = snprintf(s_body, sizeof(s_body),
        "{\"type\":\"action_triggered\","
        "\"module\":\"%s\","
        "\"cmd\":%s}",
        module_name,
        command_json);

    if (written < 0 || written >= (int)sizeof(s_body)) {
        /* Overflow: command_json is longer than our buffer allows.
         * Log a truncated warning and return error so the rule engine
         * can log the failure. Never emit a broken JSON line. */
        ESP_LOGE(TAG, "body overflow in action_triggered (%d bytes); "
                      "increase CONFIG_OUTPUT_LINE_BUF_SIZE", written);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = output_write(s_body);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "output_write failed: %s", esp_err_to_name(ret));
        /* Non-fatal for the stub — the action was 'dispatched' even if
         * the log line failed to emit. Return OK so the rule engine
         * does not count this as a dispatch failure. */
    }

    /* C4: replace the lines above with:
     *
     *   action_module_t *mod = registry_find(module_name);
     *   if (mod == NULL || !mod->available) return ESP_ERR_NOT_FOUND;
     *   return mod->execute(command_json);
     *
     * Keep the output_write() call (or make it conditional on a verbose
     * logging Kconfig flag) for observability in production.
     */

    return ESP_OK;
}
