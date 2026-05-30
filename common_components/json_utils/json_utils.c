/**
 * @file json_utils.c
 * @brief Minimal, zero-allocation JSON field extraction utilities.
 *
 * COMPONENT LOCATION: common_components/json_utils/
 *
 * See json_utils.h for design rationale and usage contract.
 */

#include "json_utils.h"

#include <string.h>   /* memcmp, strlen */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ======================================================================
 * Low-level cursor primitives
 * ====================================================================== */

const char *json_skip_ws(const char *p, const char *end)
{
    while (p < end &&
           (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        p++;
    }
    return p;
}

bool json_parse_string(const char **p, const char *end,
                       const char **str_start, size_t *str_len)
{
    if (*p >= end || **p != '"') {
        return false;
    }
    (*p)++;                    /* skip opening '"' */
    *str_start = *p;

    while (*p < end) {
        if (**p == '\\') {
            (*p)++;            /* skip the backslash */
            if (*p < end) {
                (*p)++;        /* skip the escaped character */
            }
            continue;
        }
        if (**p == '"') {
            *str_len = (size_t)(*p - *str_start);
            (*p)++;            /* skip closing '"' */
            return true;
        }
        (*p)++;
    }
    return false;              /* unterminated string */
}

bool json_skip_value(const char **p, const char *end)
{
    *p = json_skip_ws(*p, end);
    if (*p >= end) {
        return false;
    }

    if (**p == '"') {
        const char *s;
        size_t l;
        return json_parse_string(p, end, &s, &l);
    }

    if (**p == '[' || **p == '{') {
        /* Balanced bracket scan with string awareness. */
        char open  = **p;
        char close = (open == '[') ? ']' : '}';
        int  depth   = 0;
        bool in_str  = false;

        while (*p < end) {
            char c = **p;
            if (in_str) {
                if (c == '\\') {
                    (*p)++;    /* skip escaped char */
                } else if (c == '"') {
                    in_str = false;
                }
            } else {
                if      (c == '"')   { in_str = true; }
                else if (c == open)  { depth++; }
                else if (c == close) {
                    depth--;
                    if (depth == 0) {
                        (*p)++;
                        return true;
                    }
                }
            }
            (*p)++;
        }
        return false;          /* unterminated bracket structure */
    }

    /* Number, boolean, null: scan to the next structural delimiter.
     * Whitespace also terminates so that "true}" and "null," are handled. */
    while (*p < end &&
           **p != ',' && **p != '}' && **p != ']' &&
           **p != ' ' && **p != '\t' && **p != '\r' && **p != '\n') {
        (*p)++;
    }
    return true;
}

bool json_capture_array(const char **p, const char *end,
                        const char **arr_start, size_t *arr_len)
{
    if (*p >= end || **p != '[') {
        return false;
    }
    *arr_start = *p;
    const char *before = *p;
    if (!json_skip_value(p, end)) {
        return false;
    }
    *arr_len = (size_t)(*p - before);
    return true;
}

/* ======================================================================
 * Array utilities
 * ====================================================================== */

uint32_t json_count_array_items(const char *arr, size_t arr_len)
{
    if (arr_len < 2 || arr[0] != '[') {
        return 0;
    }

    const char *p   = arr + 1;          /* skip '[' */
    const char *end = arr + arr_len - 1; /* stop before closing ']' */

    p = json_skip_ws(p, end);
    if (p >= end) {
        return 0;                        /* empty array "[]" */
    }

    uint32_t count  = 1;
    int      depth  = 0;
    bool     in_str = false;

    for (; p < end; p++) {
        char c = *p;
        if (in_str) {
            if (c == '\\') { p++; }     /* escaped char: skip next byte */
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

bool json_get_array_item(const char *arr, size_t arr_len, uint32_t index,
                         const char **item_start, size_t *item_len)
{
    const char *p   = arr;
    const char *end = arr + arr_len;

    p = json_skip_ws(p, end);
    if (p >= end || *p != '[') {
        return false;
    }
    p++;                               /* skip '[' */

    for (uint32_t i = 0; p < end; i++) {
        p = json_skip_ws(p, end);
        if (p >= end || *p == ']') {
            break;                     /* ran out of elements */
        }

        const char *elem = p;
        if (!json_skip_value(&p, end)) {
            return false;             /* malformed element */
        }

        if (i == index) {
            *item_start = elem;
            *item_len   = (size_t)(p - elem);
            return true;
        }

        p = json_skip_ws(p, end);
        if (p < end && *p == ',') { p++; }
    }
    return false;                      /* index out of range */
}

/* ======================================================================
 * Internal helper: walk top-level key-value pairs of a JSON object.
 *
 * Calls on_pair(key, key_len, val_p, end, user_data) for each pair.
 * on_pair returns true to stop iteration (key found), false to continue.
 *
 * Returns true if iteration completed or was stopped by on_pair.
 * Returns false if the JSON object is malformed.
 * ====================================================================== */

typedef bool (*kv_callback_t)(const char *key, size_t key_len,
                              const char **val_p, const char *end,
                              void *user_data);

static bool walk_object(const char *obj, size_t obj_len,
                        kv_callback_t on_pair, void *user_data)
{
    const char *p   = obj;
    const char *end = obj + obj_len;

    p = json_skip_ws(p, end);
    if (p >= end || *p != '{') {
        return false;
    }
    p++;                               /* skip '{' */

    while (p < end) {
        p = json_skip_ws(p, end);
        if (p >= end)        { return false; }
        if (*p == '}')       { break; }   /* end of object — OK */

        /* Parse key string */
        const char *key;
        size_t      key_len;
        if (!json_parse_string(&p, end, &key, &key_len)) {
            return false;
        }

        /* Expect ':' separator */
        p = json_skip_ws(p, end);
        if (p >= end || *p != ':') { return false; }
        p++;

        p = json_skip_ws(p, end);
        if (p >= end) { return false; }

        /* Let the callback inspect the value at p.
         * The callback must leave *p advanced past the value if it returns
         * false (continue); if it returns true (stop), p is ignored. */
        const char *val_p_before = p;
        bool stop = on_pair(key, key_len, &p, end, user_data);
        if (stop) {
            return true;
        }

        /* If the callback did not advance p (returned false without
         * consuming the value), skip the value now. */
        if (p == val_p_before) {
            if (!json_skip_value(&p, end)) { return false; }
        }

        p = json_skip_ws(p, end);
        if (p < end && *p == ',') { p++; }
    }
    return true;
}

/* ======================================================================
 * Object field extraction — callback-based implementations
 * ====================================================================== */

/* Shared state for the string-field callback. */
typedef struct {
    const char *target_key;
    size_t      target_key_len;
    char       *out_buf;
    size_t      out_size;
    bool        found;
} str_field_state_t;

static bool str_field_cb(const char *key, size_t key_len,
                         const char **val_p, const char *end,
                         void *user_data)
{
    str_field_state_t *st = (str_field_state_t *)user_data;

    if (key_len != st->target_key_len ||
        memcmp(key, st->target_key, key_len) != 0) {
        return false;  /* not our key; let walk_object skip the value */
    }

    /* Found our key. Extract the value. */
    if (*val_p < end && **val_p == '"') {
        /* JSON string: copy content without quotes */
        const char *s;
        size_t      l;
        if (!json_parse_string(val_p, end, &s, &l)) {
            return true;   /* stop; malformed */
        }
        if (l >= st->out_size) {
            return true;   /* stop; buffer too small — caller gets false */
        }
        memcpy(st->out_buf, s, l);
        st->out_buf[l] = '\0';
        st->found = true;
    } else if (*val_p < end &&
               **val_p != '[' && **val_p != '{') {
        /* Number / bool / null: copy raw bytes to next delimiter */
        const char *num_start = *val_p;
        while (*val_p < end &&
               **val_p != ',' && **val_p != '}' && **val_p != ']' &&
               **val_p != ' ' && **val_p != '\t' &&
               **val_p != '\r' && **val_p != '\n') {
            (*val_p)++;
        }
        size_t l = (size_t)(*val_p - num_start);
        if (l >= st->out_size) {
            return true;   /* stop; buffer too small */
        }
        memcpy(st->out_buf, num_start, l);
        st->out_buf[l] = '\0';
        st->found = true;
    }
    /* Array/object values: st->found remains false; caller gets false. */
    return true;           /* stop iteration */
}

bool json_get_string_field(const char *obj, size_t obj_len,
                           const char *key,
                           char *out_buf, size_t out_size)
{
    if (!obj || !key || !out_buf || out_size == 0) { return false; }

    str_field_state_t st = {
        .target_key     = key,
        .target_key_len = strlen(key),
        .out_buf        = out_buf,
        .out_size       = out_size,
        .found          = false,
    };
    walk_object(obj, obj_len, str_field_cb, &st);
    return st.found;
}

/* Shared state for the raw-field callback. */
typedef struct {
    const char *target_key;
    size_t      target_key_len;
    const char *val_start;
    size_t      val_len;
    bool        found;
} raw_field_state_t;

static bool raw_field_cb(const char *key, size_t key_len,
                         const char **val_p, const char *end,
                         void *user_data)
{
    raw_field_state_t *st = (raw_field_state_t *)user_data;

    if (key_len != st->target_key_len ||
        memcmp(key, st->target_key, key_len) != 0) {
        return false;
    }

    /* Found. Record the start, then skip to get the length. */
    st->val_start = *val_p;
    if (!json_skip_value(val_p, end)) {
        return true;   /* stop; malformed value */
    }
    st->val_len = (size_t)(*val_p - st->val_start);
    st->found   = true;
    return true;       /* stop iteration */
}

bool json_get_raw_field(const char *obj, size_t obj_len,
                        const char *key,
                        const char **val_start, size_t *val_len)
{
    if (!obj || !key || !val_start || !val_len) { return false; }

    raw_field_state_t st = {
        .target_key     = key,
        .target_key_len = strlen(key),
        .val_start      = NULL,
        .val_len        = 0,
        .found          = false,
    };
    walk_object(obj, obj_len, raw_field_cb, &st);
    if (st.found) {
        *val_start = st.val_start;
        *val_len   = st.val_len;
    }
    return st.found;
}
