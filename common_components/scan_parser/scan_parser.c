/**
 * @file scan_parser.c
 * @brief Scanner JSON parser implementation — shared component.
 *
 * COMPONENT LOCATION: common_components/scan_parser/
 *
 * ---- Zero-allocation JSON parsing -----------------------------------
 * This file implements a purpose-built, minimal JSON field extractor
 * instead of using cJSON (which allocates per-node) or JSMN (which
 * requires vendoring). The scanner's JSON schema is known and fixed:
 *
 *   {"type":"scan_result","module":"<name>","timestamp":<ms>,"data":[...]}
 *
 * Keys may appear in any order. The parser walks top-level key-value
 * pairs using a simple state machine and string cursor. It does not
 * recurse into the "data" array — that array is captured as a raw
 * substring and forwarded intact to the rule engine and output channel.
 *
 * Handles:
 *   - Keys in any order
 *   - "timestamp" field absent (defaults to 0)
 *   - Unknown top-level keys (skipped safely)
 *   - Strings with backslash-escaped characters (passed through, not decoded)
 *   - Arrays and objects as values for unknown keys (skipped with bracket counting)
 *   - Compact JSON (no extra whitespace, as produced by the scanner)
 *   - Whitespace-padded JSON (handled defensively with skip_ws)
 *
 * Does NOT handle:
 *   - Root-level arrays or primitives (rejected as malformed)
 *   - JSON with comments
 *   - Unicode escape sequences (\uXXXX) in key names
 *     (Our key names are plain ASCII; no issue in practice.)
 * -----------------------------------------------------------------------
 *
 * Configuration (set via project Kconfig or falls back to defaults):
 *   CONFIG_SCAN_PARSER_TASK_STACK_SIZE   — parser task stack in bytes
 *   CONFIG_SCAN_PARSER_DATA_BUF_SIZE     — max raw data[] array bytes
 *   CONFIG_SCAN_PARSER_QUEUE_DEPTH       — scan_event_t queue depth (C3+)
 *   CONFIG_SCAN_PARSER_WATCHDOG_INTERVAL_MS  — max heartbeat interval
 *   CONFIG_SCAN_PARSER_WDT_FEED_PERIOD_MS    — queue read timeout
 *   CONFIG_SCAN_RECEIVER_LINE_BUF        — input line buffer size
 *                                          (must match scan_receiver's value)
 */

#include "scan_parser.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include "output.h"
#include "watchdog.h"

/* scan_receiver's line buffer size — must match what scan_receiver uses. */
#include "scan_receiver.h"   /* for SCAN_RECEIVER_LINE_BUF */

#include <string.h>
#include <stdlib.h>   /* atoll() */
#include <stdio.h>    /* snprintf() */
#include <stdbool.h>

static const char *TAG = "scan_parser";

/* ---- Compile-time defaults ------------------------------------------- */

#ifndef CONFIG_SCAN_PARSER_TASK_STACK_SIZE
#define CONFIG_SCAN_PARSER_TASK_STACK_SIZE      4096
#endif

#ifndef CONFIG_SCAN_PARSER_DATA_BUF_SIZE
#define CONFIG_SCAN_PARSER_DATA_BUF_SIZE        800
#endif

#ifndef CONFIG_SCAN_PARSER_QUEUE_DEPTH
#define CONFIG_SCAN_PARSER_QUEUE_DEPTH          4
#endif

#ifndef CONFIG_SCAN_PARSER_WATCHDOG_INTERVAL_MS
#define CONFIG_SCAN_PARSER_WATCHDOG_INTERVAL_MS 10000
#endif

#ifndef CONFIG_SCAN_PARSER_WDT_FEED_PERIOD_MS
#define CONFIG_SCAN_PARSER_WDT_FEED_PERIOD_MS   3000
#endif

#ifndef CONFIG_OUTPUT_LINE_BUF_SIZE
#define CONFIG_OUTPUT_LINE_BUF_SIZE             2048
#endif

/* Task priority: one above scan_receiver so parsed events are flushed
 * quickly, but below the watchdog task. */
#define SCAN_PARSER_TASK_PRIORITY   3

/* ---- Module state ----------------------------------------------------- */

static bool           s_initialized  = false;
static QueueHandle_t  s_rx_queue     = NULL;
static QueueHandle_t  s_event_queue  = NULL;
static TaskHandle_t   s_task_handle  = NULL;

/* Static buffers: sized at compile time, zero heap usage after init. */
static char         s_line[SCAN_RECEIVER_LINE_BUF];   /* raw line from queue  */
static scan_event_t s_event;                           /* filled per parse     */
static char         s_out_body[CONFIG_OUTPUT_LINE_BUF_SIZE]; /* output_write body */

/* ======================================================================
 * Minimal JSON field extractor — internal implementation
 * ====================================================================== */

/** Advance past ASCII whitespace. Returns updated pointer. */
static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        p++;
    }
    return p;
}

/**
 * Parse a JSON string starting at *p (which must point AT the opening '"').
 *
 * On success:
 *   - *str_start points to the first character inside the quotes.
 *   - *str_len   is the number of characters before the closing '"'.
 *   - *p         is advanced to the character AFTER the closing '"'.
 *
 * Escaped characters are passed through without decoding (we only need
 * equality checks on known ASCII key names; values are forwarded as-is).
 *
 * Returns true on success, false if the string is unterminated.
 */
static bool parse_json_string(const char **p, const char *end,
                              const char **str_start, size_t *str_len)
{
    if (*p >= end || **p != '"') {
        return false;
    }
    (*p)++;              /* skip opening quote */
    *str_start = *p;

    while (*p < end) {
        if (**p == '\\') {
            (*p)++;      /* skip the escape character itself */
            if (*p < end) {
                (*p)++; /* skip the escaped character */
            }
            continue;
        }
        if (**p == '"') {
            *str_len = (size_t)(*p - *str_start);
            (*p)++;      /* skip closing quote */
            return true;
        }
        (*p)++;
    }
    return false;        /* unterminated string */
}

/**
 * Skip any JSON value starting at *p.
 *
 * Handles: strings, numbers/booleans/null, arrays, and objects.
 * For arrays and objects, uses bracket counting with string-awareness
 * so that brackets inside string values are correctly ignored.
 *
 * Returns true if a value was successfully skipped.
 * Returns false if *p is not the start of a recognizable value.
 */
static bool skip_json_value(const char **p, const char *end)
{
    *p = skip_ws(*p, end);
    if (*p >= end) {
        return false;
    }

    if (**p == '"') {
        /* String value */
        const char *s;
        size_t l;
        return parse_json_string(p, end, &s, &l);
    }

    if (**p == '[' || **p == '{') {
        /* Array or object: balanced bracket scan with string awareness. */
        char open  = **p;
        char close = (open == '[') ? ']' : '}';
        int  depth = 0;
        bool in_str = false;

        while (*p < end) {
            char c = **p;

            if (in_str) {
                if (c == '\\') {
                    (*p)++;  /* skip next char regardless of what it is */
                } else if (c == '"') {
                    in_str = false;
                }
            } else {
                if (c == '"') {
                    in_str = true;
                } else if (c == open) {
                    depth++;
                } else if (c == close) {
                    depth--;
                    if (depth == 0) {
                        (*p)++;
                        return true;
                    }
                }
            }
            (*p)++;
        }
        return false;  /* unterminated array or object */
    }

    /* Number, boolean, or null: scan to the next structural delimiter. */
    while (*p < end &&
           **p != ',' && **p != '}' && **p != ']' &&
           **p != ' '  && **p != '\t' && **p != '\r' && **p != '\n') {
        (*p)++;
    }
    return true;
}

/**
 * Capture a JSON array as a raw substring, starting at *p (which must
 * point AT the opening '[').
 *
 * On success:
 *   - *arr_start points to the '['.
 *   - *arr_len   is the number of bytes from '[' to the matching ']'
 *                inclusive.
 *   - *p         is advanced past the closing ']'.
 *
 * Returns true on success.
 */
static bool capture_json_array(const char **p, const char *end,
                               const char **arr_start, size_t *arr_len)
{
    if (*p >= end || **p != '[') {
        return false;
    }
    *arr_start = *p;
    const char *before = *p;

    if (!skip_json_value(p, end)) {
        return false;
    }

    *arr_len = (size_t)(*p - before);
    return true;
}

/**
 * Count the number of direct (top-level) elements in a JSON array string.
 *
 * Examples:
 *   "[]"            → 0
 *   "[1]"           → 1
 *   "[1,2,3]"       → 3
 *   "[{...},{...}]" → 2
 *
 * Counts commas at depth 1 (direct children of the outer array).
 * String-aware: commas inside string values are not counted.
 */
static uint32_t count_array_items(const char *arr, size_t arr_len)
{
    if (arr_len < 2 || arr[0] != '[') {
        return 0;
    }

    /* Empty array */
    const char *p = arr + 1;
    const char *end = arr + arr_len - 1;  /* stop before closing ']' */
    p = skip_ws(p, end);
    if (p >= end) {
        return 0;
    }

    uint32_t count   = 1;
    int      depth   = 0;
    bool     in_str  = false;

    for (; p < end; p++) {
        char c = *p;
        if (in_str) {
            if (c == '\\') { p++; }  /* escape: skip next char */
            else if (c == '"') { in_str = false; }
        } else {
            if      (c == '"')               { in_str = true; }
            else if (c == '[' || c == '{')   { depth++; }
            else if (c == ']' || c == '}')   { depth--; }
            else if (c == ',' && depth == 0) { count++; }
        }
    }
    return count;
}

/* ======================================================================
 * Core parse function
 * ====================================================================== */

/**
 * @brief Parse one raw scanner JSON line into a scan_event_t.
 *
 * Expected input format (keys may be in any order, "timestamp" optional):
 *   {"type":"scan_result","module":"wifi","timestamp":1234,"data":[...]}
 *
 * @param raw      Null-terminated raw scanner JSON line.
 * @param raw_len  strlen(raw) — caller pre-computed for efficiency.
 * @param out      Caller-allocated scan_event_t to fill. Partially filled
 *                 on error; caller should not use it if non-ESP_OK.
 *
 * @return  ESP_OK                Parsed successfully; out is valid.
 *          ESP_ERR_NOT_SUPPORTED Line is valid JSON but "type" != "scan_result".
 *                                Caller should silently discard (e.g. "boot" msgs).
 *          ESP_ERR_INVALID_ARG  JSON is structurally malformed (missing fields,
 *                               bad syntax). Caller should log and discard.
 *          ESP_ERR_INVALID_SIZE data[] array exceeds CONFIG_SCAN_PARSER_DATA_BUF_SIZE.
 *                               Caller should log and discard.
 */
static esp_err_t parse_scan_line(const char *raw, size_t raw_len,
                                 scan_event_t *out)
{
    const char *p   = raw;
    const char *end = raw + raw_len;

    /* ---- Pre-condition: must start with '{' ---- */
    p = skip_ws(p, end);
    if (p >= end || *p != '{') {
        return ESP_ERR_INVALID_ARG;
    }
    p++;  /* skip '{' */

    /* ---- State for required fields ---- */
    bool type_ok   = false;
    bool module_ok = false;
    bool data_ok   = false;

    /* Zero the output struct so missing optional fields have clean defaults. */
    memset(out, 0, sizeof(*out));

    /* ---- Walk top-level key-value pairs ---- */
    while (p < end) {
        p = skip_ws(p, end);
        if (p >= end) { break; }

        /* End of object */
        if (*p == '}') { p++; break; }

        /* Expect a key string */
        const char *key;
        size_t      key_len;
        if (!parse_json_string(&p, end, &key, &key_len)) {
            return ESP_ERR_INVALID_ARG;
        }

        /* Expect ':' separator */
        p = skip_ws(p, end);
        if (p >= end || *p != ':') {
            return ESP_ERR_INVALID_ARG;
        }
        p++;

        p = skip_ws(p, end);
        if (p >= end) {
            return ESP_ERR_INVALID_ARG;
        }

        /* ---- Match known keys ---- */

        if (key_len == 4 && memcmp(key, "type", 4) == 0) {
            /* ---- "type": must be "scan_result" ---- */
            const char *val;
            size_t      val_len;
            if (!parse_json_string(&p, end, &val, &val_len)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (val_len == 11 && memcmp(val, "scan_result", 11) == 0) {
                type_ok = true;
            } else {
                /* Valid JSON, but not a scan_result — tell caller to discard
                 * silently (e.g. a "boot" or "health" envelope from the scanner). */
                return ESP_ERR_NOT_SUPPORTED;
            }

        } else if (key_len == 6 && memcmp(key, "module", 6) == 0) {
            /* ---- "module": e.g. "wifi", "ble", "bt_classic" ---- */
            const char *val;
            size_t      val_len;
            if (!parse_json_string(&p, end, &val, &val_len)) {
                return ESP_ERR_INVALID_ARG;
            }
            /* Clamp to buffer; truncation is safe — we own the output. */
            size_t copy_len = val_len;
            if (copy_len >= sizeof(out->module)) {
                copy_len = sizeof(out->module) - 1;
            }
            memcpy(out->module, val, copy_len);
            out->module[copy_len] = '\0';
            module_ok = true;

        } else if (key_len == 9 && memcmp(key, "timestamp", 9) == 0) {
            /* ---- "timestamp": optional integer (ms since scanner epoch) ---- */
            /* Read the numeric token directly from the cursor. */
            const char *ts_start = p;
            /* Advance past digits, sign, and dot (defensive for floats). */
            while (p < end && *p != ',' && *p != '}' &&
                   *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
                p++;
            }
            size_t ts_len = (size_t)(p - ts_start);
            if (ts_len > 0 && ts_len < 20) {
                /* Safe: 20 bytes covers all int64_t values (max 19 digits + sign). */
                char ts_buf[20];
                memcpy(ts_buf, ts_start, ts_len);
                ts_buf[ts_len] = '\0';
                out->timestamp_ms = (int64_t)atoll(ts_buf);
            }
            /* If ts_len == 0 or oversized, timestamp_ms remains 0 — not fatal. */

        } else if (key_len == 4 && memcmp(key, "data", 4) == 0) {
            /* ---- "data": must be a JSON array ---- */
            const char *arr_start;
            size_t      arr_len;
            if (!capture_json_array(&p, end, &arr_start, &arr_len)) {
                return ESP_ERR_INVALID_ARG;
            }
            if (arr_len == 0) {
                return ESP_ERR_INVALID_ARG;
            }
            if (arr_len >= CONFIG_SCAN_PARSER_DATA_BUF_SIZE) {
                /* Array is too large for our static buffer.
                 * Return a distinct error so the caller can log the size. */
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(out->data_json, arr_start, arr_len);
            out->data_json[arr_len] = '\0';
            out->item_count = count_array_items(arr_start, arr_len);
            data_ok = true;

        } else {
            /* ---- Unknown key: skip its value safely ---- */
            if (!skip_json_value(&p, end)) {
                return ESP_ERR_INVALID_ARG;
            }
        }

        /* ---- Advance past comma or end of object ---- */
        p = skip_ws(p, end);
        if (p < end && *p == ',') {
            p++;
        }
    }

    /* ---- Validate required fields ---- */
    if (!type_ok) {
        /* "type" was never seen — structurally incomplete JSON. */
        return ESP_ERR_INVALID_ARG;
    }
    if (!module_ok) {
        ESP_LOGW(TAG, "scan_result missing required 'module' field");
        return ESP_ERR_INVALID_ARG;
    }
    if (!data_ok) {
        ESP_LOGW(TAG, "scan_result missing required 'data' field");
        return ESP_ERR_INVALID_ARG;
    }

    out->api_version = SCAN_PARSER_API_VERSION;
    return ESP_OK;
}

/* ======================================================================
 * Output and event dispatch
 * ====================================================================== */

/**
 * @brief Format and emit the structured output line, then post to the
 *        rule engine queue if one is configured.
 *
 * Uses s_out_body (static, protected by single-task use).
 */
static void dispatch_event(const scan_event_t *ev)
{
    /* ---- Format the output body ---- */
    /* Fixed overhead:
     *   {"type":"scan_parsed","module":"","item_count":4294967295,
     *   "timestamp_ms":-9223372036854775808,"data":}
     * ≈ 115 characters + module name (max 31) ≈ 146 chars max overhead.
     * CONFIG_OUTPUT_LINE_BUF_SIZE must be >= CONFIG_SCAN_PARSER_DATA_BUF_SIZE + 150. */
    int written = snprintf(s_out_body, sizeof(s_out_body),
        "{\"type\":\"scan_parsed\","
        "\"module\":\"%s\","
        "\"item_count\":%lu,"
        "\"timestamp_ms\":%lld,"
        "\"data\":%s}",
        ev->module,
        (unsigned long)ev->item_count,
        (long long)ev->timestamp_ms,
        ev->data_json);

    if (written < 0 || written >= (int)sizeof(s_out_body)) {
        /* Body overflowed the buffer. This means either the data[] array
         * is larger than CONFIG_SCAN_PARSER_DATA_BUF_SIZE allows, or
         * CONFIG_OUTPUT_LINE_BUF_SIZE is too small. Log and drop. */
        ESP_LOGE(TAG,
                 "output body overflow (need %d bytes, buf is %d). "
                 "Increase CONFIG_OUTPUT_LINE_BUF_SIZE (recommend >= 2048).",
                 written, (int)sizeof(s_out_body));
        return;
    }

    esp_err_t ret = output_write(s_out_body);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "output_write failed: %s", esp_err_to_name(ret));
    }

    /* ---- Post to rule engine queue (C3+) ---- */
    if (s_event_queue != NULL) {
        BaseType_t sent = xQueueSend(s_event_queue, ev, 0);
        if (sent != pdTRUE) {
            /* Queue full: rule engine is not keeping up. Log and continue;
             * dropping an event is preferable to blocking the parser. */
            ESP_LOGW(TAG, "scan_event_queue full — event dropped for rule engine");
            output_write("{\"type\":\"warning\","
                         "\"msg\":\"scan_event_queue_full\"}");
        }
    }
}

/* ======================================================================
 * Parser task
 * ====================================================================== */

static void scan_parser_task(void *arg)
{
    (void)arg;

    while (1) {
        /* ---- Feed watchdog FIRST ---- */
        /* Must happen before any blocking call so the watchdog sees a
         * heartbeat even when the queue is empty (no scanner connected). */
        watchdog_feed_task("scan_parser");

        /* ---- Wait for a raw line from scan_receiver ---- */
        /* Use a timeout shorter than the watchdog interval so we return to
         * the top of the loop — and feed the watchdog — even when idle. */
        BaseType_t got = xQueueReceive(s_rx_queue, s_line,
                                       pdMS_TO_TICKS(CONFIG_SCAN_PARSER_WDT_FEED_PERIOD_MS));
        if (got != pdTRUE) {
            /* Timeout: no scanner data. Loop back and feed watchdog. */
            continue;
        }

        /* ---- Basic pre-validation ---- */
        /* The scan_receiver already checked that the line starts with '{'.
         * This is a belt-and-suspenders guard in case the queue is shared
         * with other producers in the future. */
        if (s_line[0] != '{') {
            ESP_LOGW(TAG, "Dropped non-object line (first byte: 0x%02X)",
                     (unsigned char)s_line[0]);
            continue;
        }

        size_t line_len = strnlen(s_line, SCAN_RECEIVER_LINE_BUF);

        /* ---- Parse ---- */
        esp_err_t ret = parse_scan_line(s_line, line_len, &s_event);

        switch (ret) {
        case ESP_OK:
            /* Valid scan_result: dispatch output and event. */
            dispatch_event(&s_event);
            break;

        case ESP_ERR_NOT_SUPPORTED:
            /* Valid JSON but not a scan_result (e.g. scanner boot message).
             * Discard silently — this is expected during scanner startup. */
            break;

        case ESP_ERR_INVALID_SIZE:
            /* data[] array too large for our buffer. */
            ESP_LOGW(TAG,
                     "data[] array exceeds CONFIG_SCAN_PARSER_DATA_BUF_SIZE (%d). "
                     "Increase the config or reduce scanner output density.",
                     CONFIG_SCAN_PARSER_DATA_BUF_SIZE);
            break;

        case ESP_ERR_INVALID_ARG:
        default:
            /* Malformed JSON or missing required field. */
            ESP_LOGW(TAG, "Malformed scan_result dropped (%s)",
                     esp_err_to_name(ret));
            break;
        }
    }

    /* Unreachable under normal operation. */
    vTaskDelete(NULL);
}

/* ======================================================================
 * Public API
 * ====================================================================== */

esp_err_t scan_parser_init(QueueHandle_t rx_queue, QueueHandle_t event_queue)
{
    if (rx_queue == NULL) {
        ESP_LOGE(TAG, "scan_parser_init: rx_queue must not be NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_initialized) {
        ESP_LOGW(TAG, "scan_parser_init called more than once — ignored");
        return ESP_ERR_INVALID_STATE;
    }

    s_rx_queue    = rx_queue;
    s_event_queue = event_queue;  /* NULL is valid until C3 */

    /* Register with watchdog BEFORE creating the task so the watchdog
     * knows about "scan_parser" before the task starts feeding. */
    watchdog_register_task("scan_parser",
                           CONFIG_SCAN_PARSER_WATCHDOG_INTERVAL_MS);

    BaseType_t created = xTaskCreate(
        scan_parser_task,
        "scan_parser",
        CONFIG_SCAN_PARSER_TASK_STACK_SIZE,
        NULL,
        SCAN_PARSER_TASK_PRIORITY,
        &s_task_handle
    );

    if (created != pdPASS) {
        ESP_LOGE(TAG, "scan_parser_init: xTaskCreate failed — out of heap");
        /* De-register from watchdog since the task was never created. */
        /* watchdog_deregister_task("scan_parser"); — add if supported in C5+ */
        return ESP_FAIL;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Scan parser ready (data_buf=%d, tokens=%d, stack=%d)",
             CONFIG_SCAN_PARSER_DATA_BUF_SIZE,
             0,  /* no JSMN; included for log parity */
             CONFIG_SCAN_PARSER_TASK_STACK_SIZE);

    return ESP_OK;
}

esp_err_t scan_parser_deinit(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_task_handle != NULL) {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    }

    s_rx_queue     = NULL;
    s_event_queue  = NULL;
    s_initialized  = false;

    ESP_LOGI(TAG, "Scan parser stopped");
    return ESP_OK;
}
