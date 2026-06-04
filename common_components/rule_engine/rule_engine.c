/**
 * @file rule_engine.c
 * @brief Rule engine implementation — shared component.
 *
 * COMPONENT LOCATION: common_components/rule_engine/
 *
 * ---- Memory model -------------------------------------------------------
 * All buffers are statically sized at compile time. No heap allocation
 * occurs after rule_engine_init() returns.
 *
 *   s_rules[]          — static rule_t array, CONFIG_MAX_RULES × ~420 bytes
 *   s_resolved[]       — static substitution buffer, CONFIG_ACTION_CMD_BUF_SIZE
 *   s_rule_json_buf[]  — static NVS load buffer (used only during init)
 *
 * Total static DRAM (at defaults: 16 rules, 256-byte template, 512-byte
 * resolved buffer): ~6.7 KB + 512 + 512 = ~7.7 KB.
 *
 * ---- Thread safety -------------------------------------------------------
 * s_rules[] is protected by s_rule_mutex (FreeRTOS mutex).
 *   - rule_engine_task: takes the mutex for the duration of one event
 *     evaluation (< 1 ms even at max rules × max items).
 *   - C5 command task: takes the mutex for in-RAM rule add/edit/delete
 *     (struct copy, microseconds).
 *   - NVS writes (C5) happen OUTSIDE the mutex (see header comments).
 *
 * ---- Operator evaluation -----------------------------------------------
 *   eq       : strcmp(field_val, cond_value) == 0        (case-sensitive)
 *   ne       : strcmp(field_val, cond_value) != 0
 *   contains : strstr(field_val, cond_value) != NULL
 *   gt       : strtol(field_val) > strtol(cond_value)
 *   lt       : strtol(field_val) < strtol(cond_value)
 *
 *   For gt/lt, both values are parsed with strtol(). If cond_value is not
 *   numeric, strtol returns 0 and the comparison is still performed —
 *   this is a rule authoring error caught at load time, not silently
 *   at evaluation time.
 */

#include "rule_engine.h"
#include "json_utils.h"
#include "action_registry.h"
#include "output.h"
#include "watchdog.h"
#include "scan_parser.h"   /* for scan_event_t */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include <string.h>
#include <stdlib.h>   /* strtol, atoll */
#include <stdio.h>    /* snprintf */
#include <stdbool.h>
#include <ctype.h>    /* isalnum */

static const char *TAG = "rule_engine";

/* ---- Compile-time defaults ------------------------------------------- */

#ifndef CONFIG_MAX_RULES
#define CONFIG_MAX_RULES                    16
#endif
#ifndef CONFIG_RULE_NAME_LEN
#define CONFIG_RULE_NAME_LEN                32
#endif
#ifndef CONFIG_RULE_CONDITION_VALUE_LEN
#define CONFIG_RULE_CONDITION_VALUE_LEN     64
#endif
#ifndef CONFIG_RULE_CMD_TMPL_LEN
#define CONFIG_RULE_CMD_TMPL_LEN            256
#endif
#ifndef CONFIG_ACTION_CMD_BUF_SIZE
#define CONFIG_ACTION_CMD_BUF_SIZE          512
#endif
#ifndef CONFIG_RULE_MAX_PLACEHOLDERS
#define CONFIG_RULE_MAX_PLACEHOLDERS        4
#endif
#ifndef CONFIG_RULE_ENGINE_TASK_STACK_SIZE
#define CONFIG_RULE_ENGINE_TASK_STACK_SIZE  4096
#endif
#ifndef CONFIG_RULE_ENGINE_WDT_INTERVAL_MS
#define CONFIG_RULE_ENGINE_WDT_INTERVAL_MS  10000
#endif
#ifndef CONFIG_RULE_ENGINE_WDT_FEED_PERIOD_MS
#define CONFIG_RULE_ENGINE_WDT_FEED_PERIOD_MS 3000
#endif

/* NVS namespace and key format for rule storage. */
#define RULES_NVS_NAMESPACE     "rules"
#define RULES_NVS_COUNT_KEY     "rule_count"
#define RULES_NVS_KEY_FMT       "rule_%u"
#define RULES_NVS_KEY_MAX_LEN   12   /* "rule_63\0" = 9 bytes; 12 is safe */

/* NVS load buffer: must hold the largest possible stored rule JSON.
 * name(32) + module fields(4×32) + value(64) + template(256) +
 * JSON structural overhead (~120) ≈ 600 bytes. Round up to 768. */
#define RULE_JSON_BUF_LEN       768

/* Task priority: above scan_parser (3), below watchdog. */
#define RULE_ENGINE_TASK_PRIORITY   4

/* Operator enum → string table. Index matches rule_operator_t values 0..4.
 * Used in load logging, get_all_rules, and rule_serialize_json. */
static const char * const s_op_names[] = {
    "eq", "ne", "contains", "gt", "lt"
};
#define S_OP_COUNT  5   /* number of valid entries in s_op_names */

/* ======================================================================
 * Internal types
 * ====================================================================== */

typedef enum {
    RULE_OP_EQUALS       = 0,
    RULE_OP_NOT_EQUALS   = 1,
    RULE_OP_CONTAINS     = 2,
    RULE_OP_GREATER_THAN = 3,
    RULE_OP_LESS_THAN    = 4,
    RULE_OP_INVALID      = 0xFF,
} rule_operator_t;

typedef struct {
    bool             active;
    bool             fire_all_matches;  /* false = first match only (default) */
    char             name[CONFIG_RULE_NAME_LEN];
    /* condition */
    char             cond_module[32];
    char             cond_field[32];
    rule_operator_t  cond_op;
    char             cond_value[CONFIG_RULE_CONDITION_VALUE_LEN];
    /* action */
    char             act_module[32];
    char             act_cmd_tmpl[CONFIG_RULE_CMD_TMPL_LEN];
} rule_t;

/* ======================================================================
 * Module state — static, no heap after init
 * ====================================================================== */

static bool               s_initialized = false;
static QueueHandle_t      s_event_queue = NULL;
static TaskHandle_t       s_task_handle = NULL;
static SemaphoreHandle_t  s_rule_mutex  = NULL;

static rule_t  s_rules[CONFIG_MAX_RULES];
static char    s_resolved[CONFIG_ACTION_CMD_BUF_SIZE];  /* placeholder output buffer */
static char    s_rule_json_buf[RULE_JSON_BUF_LEN];      /* NVS load buffer           */

/* ======================================================================
 * Operator string → enum conversion
 * ====================================================================== */

static rule_operator_t parse_operator(const char *op_str)
{
    if (strcmp(op_str, "eq")       == 0) return RULE_OP_EQUALS;
    if (strcmp(op_str, "ne")       == 0) return RULE_OP_NOT_EQUALS;
    if (strcmp(op_str, "contains") == 0) return RULE_OP_CONTAINS;
    if (strcmp(op_str, "gt")       == 0) return RULE_OP_GREATER_THAN;
    if (strcmp(op_str, "lt")       == 0) return RULE_OP_LESS_THAN;
    return RULE_OP_INVALID;
}

/* ======================================================================
 * Condition evaluator
 * ====================================================================== */

static bool eval_condition(const char *field_val,
                           rule_operator_t op,
                           const char *cond_val)
{
    switch (op) {
    case RULE_OP_EQUALS:
        return strcmp(field_val, cond_val) == 0;

    case RULE_OP_NOT_EQUALS:
        return strcmp(field_val, cond_val) != 0;

    case RULE_OP_CONTAINS:
        return strstr(field_val, cond_val) != NULL;

    case RULE_OP_GREATER_THAN:
    case RULE_OP_LESS_THAN: {
        long fv = strtol(field_val, NULL, 10);
        long cv = strtol(cond_val,  NULL, 10);
        return (op == RULE_OP_GREATER_THAN) ? (fv > cv) : (fv < cv);
    }

    default:
        return false;
    }
}

/* ======================================================================
 * Placeholder resolution
 *
 * Scans the template string for $item.<fieldname> occurrences and
 * substitutes the corresponding field value from the matched JSON item.
 * Writes the result into out[0..out_size-1] (null-terminated).
 *
 * Returns true on success, false if the output buffer would overflow.
 * On overflow, no partial output is written and the rule is skipped.
 * ====================================================================== */

static bool resolve_placeholders(const char *tmpl,
                                 const char *item, size_t item_len,
                                 char *out, size_t out_size)
{
    const char *src     = tmpl;
    const char *src_end = tmpl + strlen(tmpl);
    char       *dst     = out;
    char       *dst_end = out + out_size - 1;   /* reserve 1 byte for '\0' */
    int         ph_count = 0;

    while (src < src_end && dst < dst_end) {

        /* Find the next placeholder '$' */
        const char *ph = src;
        while (ph < src_end && *ph != '$') { ph++; }

        /* Copy everything up to the placeholder (or end) */
        size_t prefix_len = (size_t)(ph - src);
        if (prefix_len > (size_t)(dst_end - dst)) {
            return false;   /* prefix alone overflows */
        }
        memcpy(dst, src, prefix_len);
        dst += prefix_len;
        src  = ph;

        if (src >= src_end) {
            break;          /* no more placeholders */
        }

        /* Check for "$item." prefix */
        if (src_end - src < 6 || memcmp(src, "$item.", 6) != 0) {
            /* Literal '$' not followed by "item." — copy it verbatim */
            if (dst < dst_end) { *dst++ = '$'; }
            src++;
            continue;
        }

        /* Extract field name: alphanumeric, underscore, or hyphen */
        const char *fn_start = src + 6;
        const char *fn_end   = fn_start;
        while (fn_end < src_end &&
               (isalnum((unsigned char)*fn_end) ||
                *fn_end == '_' || *fn_end == '-')) {
            fn_end++;
        }
        size_t fn_len = (size_t)(fn_end - fn_start);

        if (fn_len == 0 || fn_len >= 64) {
            /* Malformed placeholder name — copy "$item." literally */
            size_t copy = (size_t)(fn_end - src);
            if (copy > (size_t)(dst_end - dst)) return false;
            memcpy(dst, src, copy);
            dst += copy;
            src  = fn_end;
            continue;
        }

        /* Copy field name into a null-terminated local buffer for lookup */
        char field_name[64];
        memcpy(field_name, fn_start, fn_len);
        field_name[fn_len] = '\0';

        /* Extract the field value from the matched item */
        char field_val[CONFIG_RULE_CONDITION_VALUE_LEN + 32];
        bool found = json_get_string_field(item, item_len,
                                           field_name,
                                           field_val, sizeof(field_val));
        if (!found) {
            /* Field not present in this item — substitute empty string.
             * This is a rule authoring issue; warn at evaluate time. */
            ESP_LOGW(TAG, "Placeholder $item.%s not found in item", field_name);
            field_val[0] = '\0';
        }

        size_t val_len = strlen(field_val);
        if (val_len > (size_t)(dst_end - dst)) {
            return false;   /* substituted value overflows */
        }
        memcpy(dst, field_val, val_len);
        dst += val_len;
        src  = fn_end;

        if (++ph_count >= CONFIG_RULE_MAX_PLACEHOLDERS) {
            /* Safety cap: copy the remainder literally, no more substitutions */
            size_t remain = (size_t)(src_end - src);
            if (remain > (size_t)(dst_end - dst)) return false;
            memcpy(dst, src, remain);
            dst += remain;
            break;
        }
    }

    *dst = '\0';
    return true;
}

/* ======================================================================
 * Rule JSON parser  (NVS load path — runs only at init or on rule add)
 *
 * Parses the compact JSON schema into a rule_t struct using json_utils.
 * Returns ESP_OK if all required fields are valid.
 * ====================================================================== */

static esp_err_t parse_rule_json(const char *json, size_t json_len, rule_t *out)
{
    memset(out, 0, sizeof(*out));
    out->cond_op = RULE_OP_INVALID;

    /* Required-field tracking */
    bool has_cm = false, has_cf = false, has_co = false;
    bool has_cv = false, has_am = false, has_ac = false;

    const char *p   = json;
    const char *end = json + json_len;

    p = json_skip_ws(p, end);
    if (p >= end || *p != '{') return ESP_ERR_INVALID_ARG;
    p++;

    while (p < end) {
        p = json_skip_ws(p, end);
        if (p >= end) break;
        if (*p == '}') { p++; break; }

        const char *key; size_t kl;
        if (!json_parse_string(&p, end, &key, &kl)) return ESP_ERR_INVALID_ARG;

        p = json_skip_ws(p, end);
        if (p >= end || *p != ':') return ESP_ERR_INVALID_ARG;
        p++;
        p = json_skip_ws(p, end);
        if (p >= end) return ESP_ERR_INVALID_ARG;

        /* --- Match keys --- */

        if (kl == 4 && memcmp(key, "name", 4) == 0) {
            const char *v; size_t vl;
            if (!json_parse_string(&p, end, &v, &vl)) return ESP_ERR_INVALID_ARG;
            size_t cl = (vl >= sizeof(out->name)) ? sizeof(out->name)-1 : vl;
            memcpy(out->name, v, cl);
            out->name[cl] = '\0';

        } else if (kl == 2 && memcmp(key, "on", 2) == 0) {
            /* Integer 0 or 1 */
            const char *v = p;
            while (p < end && *p != ',' && *p != '}' &&
                   *p != ' ' && *p != '\t') p++;
            /* "1" → active, anything else → inactive */
            out->active = (p > v && v[0] == '1');

        } else if (kl == 2 && memcmp(key, "cm", 2) == 0) {
            const char *v; size_t vl;
            if (!json_parse_string(&p, end, &v, &vl)) return ESP_ERR_INVALID_ARG;
            size_t cl = (vl >= sizeof(out->cond_module)) ? sizeof(out->cond_module)-1 : vl;
            memcpy(out->cond_module, v, cl);
            out->cond_module[cl] = '\0';
            has_cm = true;

        } else if (kl == 2 && memcmp(key, "cf", 2) == 0) {
            const char *v; size_t vl;
            if (!json_parse_string(&p, end, &v, &vl)) return ESP_ERR_INVALID_ARG;
            size_t cl = (vl >= sizeof(out->cond_field)) ? sizeof(out->cond_field)-1 : vl;
            memcpy(out->cond_field, v, cl);
            out->cond_field[cl] = '\0';
            has_cf = true;

        } else if (kl == 2 && memcmp(key, "co", 2) == 0) {
            const char *v; size_t vl;
            if (!json_parse_string(&p, end, &v, &vl)) return ESP_ERR_INVALID_ARG;
            char op_str[16] = {0};
            size_t cl = (vl >= sizeof(op_str)) ? sizeof(op_str)-1 : vl;
            memcpy(op_str, v, cl);
            out->cond_op = parse_operator(op_str);
            if (out->cond_op == RULE_OP_INVALID) {
                ESP_LOGW(TAG, "Unknown operator '%s'", op_str);
                return ESP_ERR_INVALID_ARG;
            }
            has_co = true;

        } else if (kl == 2 && memcmp(key, "cv", 2) == 0) {
            /* Condition value may be a JSON string ("Speaker") or number (-70).
             * For strings: extract content without quotes.
             * For numbers: extract raw digits. */
            if (p < end && *p == '"') {
                const char *v; size_t vl;
                if (!json_parse_string(&p, end, &v, &vl)) return ESP_ERR_INVALID_ARG;
                size_t cl = (vl >= sizeof(out->cond_value)) ? sizeof(out->cond_value)-1 : vl;
                memcpy(out->cond_value, v, cl);
                out->cond_value[cl] = '\0';
            } else {
                const char *v = p;
                while (p < end && *p != ',' && *p != '}' &&
                       *p != ' ' && *p != '\t') p++;
                size_t vl = (size_t)(p - v);
                size_t cl = (vl >= sizeof(out->cond_value)) ? sizeof(out->cond_value)-1 : vl;
                memcpy(out->cond_value, v, cl);
                out->cond_value[cl] = '\0';
            }
            has_cv = true;

        } else if (kl == 2 && memcmp(key, "am", 2) == 0) {
            const char *v; size_t vl;
            if (!json_parse_string(&p, end, &v, &vl)) return ESP_ERR_INVALID_ARG;
            size_t cl = (vl >= sizeof(out->act_module)) ? sizeof(out->act_module)-1 : vl;
            memcpy(out->act_module, v, cl);
            out->act_module[cl] = '\0';
            has_am = true;

        } else if (kl == 2 && memcmp(key, "ac", 2) == 0) {
            /* Action command: a nested JSON object. Capture as raw substring
             * directly from the current cursor position (which points to '{').
             * json_skip_value advances p past the matching '}', giving us
             * the exact byte span to store in act_cmd_tmpl. */
            const char *ac_start = p;
            if (!json_skip_value(&p, end)) return ESP_ERR_INVALID_ARG;
            size_t ac_len = (size_t)(p - ac_start);
            if (ac_len == 0 || ac_start[0] != '{') {
                ESP_LOGW(TAG, "'ac' is not a JSON object");
                return ESP_ERR_INVALID_ARG;
            }
            if (ac_len >= sizeof(out->act_cmd_tmpl)) {
                ESP_LOGW(TAG, "'ac' template exceeds CMD_TMPL_LEN (%d)",
                         CONFIG_RULE_CMD_TMPL_LEN);
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(out->act_cmd_tmpl, ac_start, ac_len);
            out->act_cmd_tmpl[ac_len] = '\0';
            has_ac = true;

        } else if (kl == 2 && memcmp(key, "fm", 2) == 0) {
            const char *v = p;
            while (p < end && *p != ',' && *p != '}' &&
                   *p != ' ' && *p != '\t') p++;
            out->fire_all_matches = (p > v && v[0] == '1');

        } else {
            /* Unknown key: skip value */
            if (!json_skip_value(&p, end)) return ESP_ERR_INVALID_ARG;
        }

        p = json_skip_ws(p, end);
        if (p < end && *p == ',') p++;
    }

    /* ---- Validate required fields ---- */
    if (!has_cm || !has_cf || !has_co || !has_cv || !has_am || !has_ac) {
        ESP_LOGW(TAG, "Rule missing required field(s): "
                      "cm=%d cf=%d co=%d cv=%d am=%d ac=%d",
                 has_cm, has_cf, has_co, has_cv, has_am, has_ac);
        return ESP_ERR_INVALID_ARG;
    }

    /* Warn if numeric operator has a non-numeric condition value */
    if ((out->cond_op == RULE_OP_GREATER_THAN ||
         out->cond_op == RULE_OP_LESS_THAN) &&
        out->cond_value[0] != '-' && !isdigit((unsigned char)out->cond_value[0])) {
        ESP_LOGW(TAG, "Rule '%s': operator gt/lt but cond_value '%s' is not numeric",
                 out->name, out->cond_value);
        /* Non-fatal: strtol will return 0; comparison will still execute. */
    }

    return ESP_OK;
}

/* ======================================================================
 * NVS rule loading  (called once from rule_engine_init)
 * ====================================================================== */

static void load_rules_from_nvs(void)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(RULES_NVS_NAMESPACE, NVS_READONLY, &h);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No 'rules' NVS namespace — starting with empty table");
        return;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open('rules') failed: %s", esp_err_to_name(ret));
        return;
    }

    uint16_t count = 0;
    nvs_get_u16(h, RULES_NVS_COUNT_KEY, &count);
    if (count > CONFIG_MAX_RULES) {
        ESP_LOGW(TAG, "NVS rule_count %u > CONFIG_MAX_RULES %d — clamped",
                 count, CONFIG_MAX_RULES);
        count = CONFIG_MAX_RULES;
    }

    uint16_t loaded = 0;
    char key[RULES_NVS_KEY_MAX_LEN];

    for (uint16_t i = 0; i < count; i++) {
        snprintf(key, sizeof(key), RULES_NVS_KEY_FMT, (unsigned)i);

        size_t len = sizeof(s_rule_json_buf);
        ret = nvs_get_str(h, key, s_rule_json_buf, &len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Rule %u: nvs_get_str failed (%s) — slot skipped",
                     i, esp_err_to_name(ret));
            continue;
        }

        esp_err_t parse_ret = parse_rule_json(s_rule_json_buf, len - 1, &s_rules[i]);
        if (parse_ret != ESP_OK) {
            ESP_LOGW(TAG, "Rule %u: parse failed — slot inactive", i);
            s_rules[i].active = false;
        } else {
            uint8_t op_idx = (uint8_t)((s_rules[i].cond_op < S_OP_COUNT)
                                       ? s_rules[i].cond_op : 0U);
            ESP_LOGI(TAG, "Rule %u: '%s'  %s.%s %s '%s'  -> %s",
                     i, s_rules[i].name,
                     s_rules[i].cond_module, s_rules[i].cond_field,
                     s_op_names[op_idx],
                     s_rules[i].cond_value,
                     s_rules[i].act_module);
            loaded++;
        }
    }

    nvs_close(h);
    ESP_LOGI(TAG, "Loaded %u/%u rules from NVS", loaded, count);
}

/* ======================================================================
 * Event evaluation  (called from rule_engine_task, holds s_rule_mutex)
 * ====================================================================== */

static void evaluate_event(const scan_event_t *ev)
{
    if (xSemaphoreTake(s_rule_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "evaluate_event: mutex timeout — event skipped");
        return;
    }

    for (int ri = 0; ri < CONFIG_MAX_RULES; ri++) {
        const rule_t *r = &s_rules[ri];

        if (!r->active) { continue; }

        /* Fast module-level reject: skip rules not relevant to this scan. */
        if (strcmp(r->cond_module, ev->module) != 0) { continue; }

        /* Iterate data_json items with an inline O(N) cursor.
         * We do NOT call json_get_array_item() in a loop (which would be O(N²)).
         * Instead, we advance the cursor item by item, skipping each one. */
        const char *arr     = ev->data_json;
        const char *arr_end = arr + strlen(arr);
        const char *p       = arr;

        p = json_skip_ws(p, arr_end);
        if (p >= arr_end || *p != '[') {
            ESP_LOGW(TAG, "Rule %d: data_json not an array", ri);
            continue;
        }
        p++;  /* skip '[' */

        bool rule_fired = false;

        while (p < arr_end) {
            p = json_skip_ws(p, arr_end);
            if (p >= arr_end || *p == ']') { break; }

            /* Capture this item as a raw slice. */
            const char *item_start = p;
            if (!json_skip_value(&p, arr_end)) { break; }
            size_t item_len = (size_t)(p - item_start);

            /* Extract the condition field from this item. */
            char field_val[CONFIG_RULE_CONDITION_VALUE_LEN + 16];
            bool found = json_get_string_field(item_start, item_len,
                                               r->cond_field,
                                               field_val, sizeof(field_val));
            if (found && eval_condition(field_val, r->cond_op, r->cond_value)) {

                /* Resolve placeholders into s_resolved[]. */
                bool ok = resolve_placeholders(r->act_cmd_tmpl,
                                               item_start, item_len,
                                               s_resolved,
                                               CONFIG_ACTION_CMD_BUF_SIZE);
                if (!ok) {
                    ESP_LOGW(TAG, "Rule %d: placeholder overflow — action skipped", ri);
                } else {
                    /* Dispatch. action_registry_execute MUST return quickly. */
                    esp_err_t ret = action_registry_execute(r->act_module, s_resolved);
                    if (ret != ESP_OK) {
                        ESP_LOGW(TAG, "Rule %d: dispatch failed: %s",
                                 ri, esp_err_to_name(ret));
                    }
                    rule_fired = true;
                    if (!r->fire_all_matches) {
                        break;  /* first-match mode: stop processing items */
                    }
                }
            }

            /* Advance past comma separator. */
            p = json_skip_ws(p, arr_end);
            if (p < arr_end && *p == ',') { p++; }
        }

        (void)rule_fired;  /* used for potential future per-rule counters */
    }

    xSemaphoreGive(s_rule_mutex);
}

/* ======================================================================
 * Rule engine task
 * ====================================================================== */

static void rule_engine_task(void *arg)
{
    (void)arg;
    static scan_event_t s_ev;  /* static: avoids large stack frame */

    while (1) {
        /* Feed watchdog BEFORE blocking. Essential when no scanner data
         * is present — the task must still heartbeat every feed period. */
        watchdog_feed_task("rule_engine");

        BaseType_t got = xQueueReceive(s_event_queue, &s_ev,
                                       pdMS_TO_TICKS(CONFIG_RULE_ENGINE_WDT_FEED_PERIOD_MS));
        if (got != pdTRUE) {
            continue;   /* timeout: loop back and feed watchdog */
        }

        /* Validate api_version before processing. */
        if (s_ev.api_version != SCAN_PARSER_API_VERSION) {
            ESP_LOGW(TAG, "scan_event_t api_version mismatch (%u != %u) — dropped",
                     s_ev.api_version, SCAN_PARSER_API_VERSION);
            continue;
        }

        evaluate_event(&s_ev);
    }

    vTaskDelete(NULL);
}

/* ======================================================================
 * Public API
 * ====================================================================== */

esp_err_t rule_engine_init(QueueHandle_t event_queue)
{
    if (event_queue == NULL) {
        ESP_LOGE(TAG, "event_queue must not be NULL");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) {
        ESP_LOGW(TAG, "rule_engine_init called more than once — ignored");
        return ESP_ERR_INVALID_STATE;
    }

    /* Create mutex before loading rules (C5 command handler may call
     * before init completes in theory, though main.c prevents it). */
    s_rule_mutex = xSemaphoreCreateMutex();
    if (s_rule_mutex == NULL) {
        ESP_LOGE(TAG, "mutex creation failed — out of heap");
        return ESP_FAIL;
    }

    s_event_queue = event_queue;

    /* Load rules from NVS into the static s_rules[] table. */
    memset(s_rules, 0, sizeof(s_rules));
    load_rules_from_nvs();

    /* Register with watchdog BEFORE task creation. */
    watchdog_register_task("rule_engine", CONFIG_RULE_ENGINE_WDT_INTERVAL_MS);

    if (xTaskCreate(rule_engine_task, "rule_engine",
                    CONFIG_RULE_ENGINE_TASK_STACK_SIZE,
                    NULL, RULE_ENGINE_TASK_PRIORITY,
                    &s_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed — out of heap");
        vSemaphoreDelete(s_rule_mutex);
        s_rule_mutex = NULL;
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Rule engine ready (max_rules=%d, stack=%d)",
             CONFIG_MAX_RULES, CONFIG_RULE_ENGINE_TASK_STACK_SIZE);
    return ESP_OK;
}

esp_err_t rule_engine_deinit(void)
{
    if (!s_initialized) { return ESP_ERR_INVALID_STATE; }

    if (s_task_handle) {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    }
    if (s_rule_mutex) {
        vSemaphoreDelete(s_rule_mutex);
        s_rule_mutex = NULL;
    }

    s_event_queue = NULL;
    s_initialized = false;
    ESP_LOGI(TAG, "Rule engine stopped");
    return ESP_OK;
}

/* ======================================================================
 * C5 Rule Management API
 * ====================================================================== */

/* ---- Internal serializer (not exposed in header) ---------------------- */

/**
 * Regenerate the compact NVS JSON for a rule from its struct fields.
 * Output format matches the input format expected by parse_rule_json().
 * The act_cmd_tmpl field is already a JSON object string ({...}); it is
 * embedded directly as a nested value, not as a quoted string.
 *
 * Called while holding s_rule_mutex (snprintf only, no I/O — safe).
 * Also called from rule_engine_add_rule to validate round-trip before
 * NVS write; in that path the mutex is NOT held.
 */
static esp_err_t rule_serialize_json(const rule_t *r, char *buf, size_t sz)
{
    if (r == NULL || buf == NULL || sz == 0) { return ESP_ERR_INVALID_ARG; }

    const char *op_str = (r->cond_op < S_OP_COUNT)
                         ? s_op_names[r->cond_op] : "eq";

    int w = snprintf(buf, sz,
        "{\"name\":\"%s\","
        "\"on\":%d,"
        "\"cm\":\"%s\","
        "\"cf\":\"%s\","
        "\"co\":\"%s\","
        "\"cv\":\"%s\","
        "\"am\":\"%s\","
        "\"ac\":%s,"
        "\"fm\":%d}",
        r->name,
        r->active ? 1 : 0,
        r->cond_module,
        r->cond_field,
        op_str,
        r->cond_value,          /* always quoted as string — parse_rule_json
                                 * accepts both quoted and unquoted cv */
        r->act_module,
        r->act_cmd_tmpl,        /* already a JSON object; embed directly */
        r->fire_all_matches ? 1 : 0);

    if (w < 0 || w >= (int)sz) { return ESP_ERR_INVALID_SIZE; }
    return ESP_OK;
}

/* ---- NVS open/close helpers ------------------------------------------ */

static esp_err_t nvs_open_rules(nvs_open_mode_t mode, nvs_handle_t *h)
{
    esp_err_t ret = nvs_open(RULES_NVS_NAMESPACE, mode, h);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open('%s') failed: %s",
                 RULES_NVS_NAMESPACE, esp_err_to_name(ret));
    }
    return ret;
}

/* ---- rule_engine_add_rule -------------------------------------------- */

esp_err_t rule_engine_add_rule(const char *json, int *out_index)
{
    if (json == NULL || out_index == NULL) { return ESP_ERR_INVALID_ARG; }

    /* Step 1: Parse and validate the JSON before touching the mutex.
     * parse_rule_json reads s_rule_json_buf so we must copy first. */
    size_t jlen = strnlen(json, RULE_JSON_BUF_LEN);
    if (jlen == 0 || jlen >= RULE_JSON_BUF_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Use a local stack copy so s_rule_json_buf remains free for NVS ops. */
    rule_t new_rule;
    esp_err_t ret = parse_rule_json(json, jlen, &new_rule);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "add_rule: JSON parse failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Step 2: Find first free slot under mutex.
     * Free = name[0] == '\0' (slot never filled, or previously deleted). */
    if (xSemaphoreTake(s_rule_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    int free_idx = -1;
    for (int i = 0; i < CONFIG_MAX_RULES; i++) {
        if (s_rules[i].name[0] == '\0') { free_idx = i; break; }
    }
    xSemaphoreGive(s_rule_mutex);

    if (free_idx < 0) {
        ESP_LOGW(TAG, "add_rule: rule table full (%d slots)", CONFIG_MAX_RULES);
        return ESP_ERR_NO_MEM;
    }

    /* Step 3: Write to NVS OUTSIDE mutex.
     * If this fails we return the error; the in-memory slot stays free. */
    char key[RULES_NVS_KEY_MAX_LEN];
    snprintf(key, sizeof(key), RULES_NVS_KEY_FMT, (unsigned)free_idx);

    nvs_handle_t h;
    ret = nvs_open_rules(NVS_READWRITE, &h);
    if (ret != ESP_OK) { return ret; }

    ret = nvs_set_str(h, key, json);
    if (ret == ESP_OK) {
        /* Update high-water-mark count if this index extends the range. */
        uint16_t count = 0;
        nvs_get_u16(h, RULES_NVS_COUNT_KEY, &count);
        if ((uint16_t)(free_idx + 1) > count) {
            nvs_set_u16(h, RULES_NVS_COUNT_KEY, (uint16_t)(free_idx + 1));
        }
        ret = nvs_commit(h);
    }
    nvs_close(h);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "add_rule: NVS write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Step 4: Populate in-memory slot under mutex.
     * Double-check the slot is still free (future-proof for multi-writer). */
    if (xSemaphoreTake(s_rule_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        /* NVS has the rule; memory does not. Self-corrects on next reboot. */
        ESP_LOGE(TAG, "add_rule: second mutex timeout — NVS/RAM desync until reboot");
        return ESP_ERR_TIMEOUT;
    }
    if (s_rules[free_idx].name[0] != '\0') {
        /* Slot was taken between our two lock windows (shouldn't happen at C5). */
        xSemaphoreGive(s_rule_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(&s_rules[free_idx], &new_rule, sizeof(rule_t));
    xSemaphoreGive(s_rule_mutex);

    *out_index = free_idx;
    ESP_LOGI(TAG, "Rule added at index %d: '%s'", free_idx, new_rule.name);
    return ESP_OK;
}

/* ---- rule_engine_delete_rule ----------------------------------------- */

esp_err_t rule_engine_delete_rule(int index)
{
    if (index < 0 || index >= CONFIG_MAX_RULES) { return ESP_ERR_INVALID_ARG; }

    /* Clear in-memory slot first (under mutex). */
    if (xSemaphoreTake(s_rule_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_rules[index].name[0] == '\0') {
        xSemaphoreGive(s_rule_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    char deleted_name[CONFIG_RULE_NAME_LEN];
    strlcpy(deleted_name, s_rules[index].name, sizeof(deleted_name));
    memset(&s_rules[index], 0, sizeof(rule_t));   /* name[0] = '\0' → free */
    xSemaphoreGive(s_rule_mutex);

    /* Erase NVS key OUTSIDE mutex. */
    char key[RULES_NVS_KEY_MAX_LEN];
    snprintf(key, sizeof(key), RULES_NVS_KEY_FMT, (unsigned)index);

    nvs_handle_t h;
    if (nvs_open_rules(NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, key);      /* non-fatal if key was already absent */
        nvs_commit(h);
        nvs_close(h);
    }

    ESP_LOGI(TAG, "Rule %d ('%s') deleted", index, deleted_name);
    return ESP_OK;
}

/* ---- rule_engine_set_enabled ----------------------------------------- */

esp_err_t rule_engine_set_enabled(int index, bool enabled)
{
    if (index < 0 || index >= CONFIG_MAX_RULES) { return ESP_ERR_INVALID_ARG; }

    if (xSemaphoreTake(s_rule_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_rules[index].name[0] == '\0') {
        xSemaphoreGive(s_rule_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    s_rules[index].active = enabled;

    /* Serialize the updated rule to the shared NVS buffer.
     * snprintf inside the mutex is fine — no blocking I/O. */
    esp_err_t ser = rule_serialize_json(&s_rules[index],
                                        s_rule_json_buf,
                                        sizeof(s_rule_json_buf));
    xSemaphoreGive(s_rule_mutex);

    if (ser != ESP_OK) {
        ESP_LOGW(TAG, "set_enabled: serialize failed, NVS not updated");
        return ser;
    }

    /* Write to NVS OUTSIDE mutex. */
    char key[RULES_NVS_KEY_MAX_LEN];
    snprintf(key, sizeof(key), RULES_NVS_KEY_FMT, (unsigned)index);

    nvs_handle_t h;
    esp_err_t ret = nvs_open_rules(NVS_READWRITE, &h);
    if (ret != ESP_OK) { return ret; }

    ret = nvs_set_str(h, key, s_rule_json_buf);
    if (ret == ESP_OK) { ret = nvs_commit(h); }
    nvs_close(h);

    ESP_LOGI(TAG, "Rule %d %s", index, enabled ? "enabled" : "disabled");
    return ret;
}

/* ---- rule_engine_get_all_rules --------------------------------------- */

esp_err_t rule_engine_get_all_rules(char *buffer, size_t buf_size)
{
    if (buffer == NULL || buf_size == 0) { return ESP_ERR_INVALID_ARG; }

    if (xSemaphoreTake(s_rule_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    size_t pos   = 0;
    int    count = 0;

    for (int i = 0; i < CONFIG_MAX_RULES; i++) {
        const rule_t *r = &s_rules[i];
        if (r->name[0] == '\0') { continue; }   /* free slot */

        uint8_t op_idx = (uint8_t)((r->cond_op < S_OP_COUNT)
                                   ? r->cond_op : 0U);
        int w = snprintf(buffer + pos, buf_size - pos,
            "[%d] '%s' | %s.%s %s '%s' -> %s | %s\r\n",
            i,
            r->name,
            r->cond_module, r->cond_field,
            s_op_names[op_idx],
            r->cond_value,
            r->act_module,
            r->active ? "enabled" : "disabled");

        if (w <= 0 || pos + (size_t)w >= buf_size) { break; }
        pos  += (size_t)w;
        count++;
    }

    /* Summary line. */
    snprintf(buffer + pos, buf_size - pos, "Total: %d rule(s)\r\n", count);

    xSemaphoreGive(s_rule_mutex);
    return ESP_OK;
}

/* ---- rule_engine_test_event ------------------------------------------ */

esp_err_t rule_engine_test_event(const char *event_json,
                                 char *buffer, size_t buf_size)
{
    if (event_json == NULL || buffer == NULL || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ej_len = strlen(event_json);

    /* Extract "module" field from event JSON. */
    char ev_module[32] = {0};
    if (!json_get_string_field(event_json, ej_len, "module",
                               ev_module, sizeof(ev_module))) {
        snprintf(buffer, buf_size, "ERROR: event missing 'module' field\r\n");
        return ESP_ERR_INVALID_ARG;
    }

    /* Extract "data" array as a raw substring (includes '[' and ']'). */
    const char *data_start = NULL;
    size_t      data_len   = 0;
    if (!json_get_raw_field(event_json, ej_len, "data",
                            &data_start, &data_len)) {
        snprintf(buffer, buf_size, "ERROR: event missing 'data' field\r\n");
        return ESP_ERR_INVALID_ARG;
    }
    if (data_start == NULL || data_len < 2 || data_start[0] != '[') {
        snprintf(buffer, buf_size, "ERROR: 'data' is not an array\r\n");
        return ESP_ERR_INVALID_ARG;
    }

    /* Evaluate against all enabled rules — dry run (no dispatch). */
    if (xSemaphoreTake(s_rule_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    size_t pos       = 0;
    int    fire_count = 0;

    for (int ri = 0; ri < CONFIG_MAX_RULES; ri++) {
        const rule_t *r = &s_rules[ri];
        if (!r->active)                              { continue; }
        if (strcmp(r->cond_module, ev_module) != 0)  { continue; }

        /* Inline O(N) item cursor — mirrors evaluate_event. */
        const char *arr     = data_start;
        const char *arr_end = data_start + data_len;
        const char *p       = arr;

        p = json_skip_ws(p, arr_end);
        if (p >= arr_end || *p != '[') { continue; }
        p++;

        while (p < arr_end) {
            p = json_skip_ws(p, arr_end);
            if (p >= arr_end || *p == ']') { break; }

            const char *item_start = p;
            if (!json_skip_value(&p, arr_end)) { break; }
            size_t item_len = (size_t)(p - item_start);

            char field_val[CONFIG_RULE_CONDITION_VALUE_LEN + 16];
            if (json_get_string_field(item_start, item_len,
                                      r->cond_field,
                                      field_val, sizeof(field_val))) {

                if (eval_condition(field_val, r->cond_op, r->cond_value)) {
                    /* Resolve placeholders for display in s_resolved[]. */
                    bool ok = resolve_placeholders(r->act_cmd_tmpl,
                                                   item_start, item_len,
                                                   s_resolved,
                                                   CONFIG_ACTION_CMD_BUF_SIZE);
                    int w = snprintf(buffer + pos, buf_size - pos,
                        "MATCH: Rule %d ('%s') -> %s %s\r\n",
                        ri, r->name, r->act_module,
                        ok ? s_resolved : "(placeholder overflow)");
                    if (w > 0 && pos + (size_t)w < buf_size) {
                        pos += (size_t)w;
                    }
                    fire_count++;
                    if (!r->fire_all_matches) { break; }
                }
            }

            p = json_skip_ws(p, arr_end);
            if (p < arr_end && *p == ',') { p++; }
        }
    }

    if (fire_count == 0) {
        snprintf(buffer + pos, buf_size - pos, "No rules would fire.\r\n");
    } else {
        snprintf(buffer + pos, buf_size - pos,
                 "%d rule(s) would fire.\r\n", fire_count);
    }

    xSemaphoreGive(s_rule_mutex);
    return ESP_OK;
}

/* ---- rule_engine_get_count ------------------------------------------- */

int rule_engine_get_count(void)
{
    if (xSemaphoreTake(s_rule_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return -1;
    }
    int count = 0;
    for (int i = 0; i < CONFIG_MAX_RULES; i++) {
        if (s_rules[i].name[0] != '\0') { count++; }
    }
    xSemaphoreGive(s_rule_mutex);
    return count;
}
