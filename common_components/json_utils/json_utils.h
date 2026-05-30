/**
 * @file json_utils.h
 * @brief Minimal, zero-allocation JSON field extraction utilities.
 *
 * COMPONENT LOCATION: common_components/json_utils/
 *
 * PURPOSE
 * -------
 * Provides cursor-based JSON parsing primitives shared between any component
 * that needs to read JSON fields without dynamic allocation. Originally these
 * functions lived as static helpers inside scan_parser.c; they were extracted
 * here when rule_engine.c needed the same logic.
 *
 * DESIGN CONSTRAINTS
 * ------------------
 *   - Zero heap allocation. All functions operate on caller-provided buffers.
 *   - No global state. All state is in the caller's local variables.
 *   - Single-pass, cursor-based. Callers advance a `const char **p` pointer.
 *   - Does NOT handle Unicode escape sequences (\uXXXX) in key comparisons.
 *     All keys in this project are plain ASCII.
 *   - Backslash-escaped characters inside string values are skipped correctly
 *     for structural purposes but are NOT decoded (e.g. \n stays as \n).
 *     Values forwarded to other systems are passed through as-is.
 *
 * USAGE PATTERN
 * -------------
 * Most callers use the high-level helpers (json_get_string_field,
 * json_get_raw_field, json_get_array_item) and never touch the low-level
 * cursor primitives directly. The primitives are public so that components
 * with custom parsing loops (e.g. scan_parser's top-level envelope walk)
 * can call them directly without duplicating the logic.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ======================================================================
 * Low-level cursor primitives
 * These operate on a [p, end) byte range. The caller advances *p.
 * ====================================================================== */

/**
 * @brief Skip ASCII whitespace characters (space, tab, CR, LF).
 * @return Pointer to the first non-whitespace byte, or end if none found.
 *         Does NOT modify any pointer indirectly — returns new position.
 */
const char *json_skip_ws(const char *p, const char *end);

/**
 * @brief Parse a JSON string token starting at *p (which must point AT '"').
 *
 * On success:
 *   *str_start → first byte inside the quotes (may be empty string).
 *   *str_len   → byte count between the quotes (excluding both '"' chars).
 *   *p         → advanced to the byte AFTER the closing '"'.
 *
 * Escaped characters (\", \\, \n, etc.) are skipped correctly so that an
 * escaped '"' inside the string does not terminate parsing prematurely.
 * The raw escaped bytes are left in place; callers receive the raw slice.
 *
 * @return true on success, false if *p is not '"' or the string is unterminated.
 */
bool json_parse_string(const char **p, const char *end,
                       const char **str_start, size_t *str_len);

/**
 * @brief Skip any JSON value starting at *p.
 *
 * Handles: strings, numbers, booleans (true/false), null, arrays, objects.
 * For arrays and objects, uses balanced bracket counting with string awareness
 * so that structural characters inside string values are ignored.
 *
 * On success, *p is advanced to the first byte after the skipped value.
 *
 * @return true if a value was skipped, false if *p does not start a value
 *         or the value is unterminated (malformed JSON).
 */
bool json_skip_value(const char **p, const char *end);

/**
 * @brief Capture a JSON array as a raw substring, starting at *p (AT '[').
 *
 * On success:
 *   *arr_start → the '[' character.
 *   *arr_len   → byte count from '[' through the matching ']', inclusive.
 *   *p         → advanced to the byte AFTER the closing ']'.
 *
 * @return true on success, false if *p is not '[' or the array is unterminated.
 */
bool json_capture_array(const char **p, const char *end,
                        const char **arr_start, size_t *arr_len);

/* ======================================================================
 * Array utilities
 * ====================================================================== */

/**
 * @brief Count the number of direct (top-level) elements in a JSON array.
 *
 * "Direct" means elements at depth 1 inside the outer '[...]'; nested
 * arrays and objects do not contribute additional counts.
 *
 * Examples:
 *   "[]"                    → 0
 *   "[1]"                   → 1
 *   "[1,2,3]"               → 3
 *   "[[1,2],[3,4]]"         → 2
 *   "[{\"a\":1},{\"b\":2}]" → 2
 *
 * @param arr     Pointer to the '[' of the array string.
 * @param arr_len Byte length including both '[' and ']'.
 * @return Number of direct elements, or 0 if arr is malformed or empty.
 */
uint32_t json_count_array_items(const char *arr, size_t arr_len);

/**
 * @brief Return a raw pointer and length for the Nth element of a JSON array.
 *
 * Elements are zero-indexed. The returned slice spans the raw value bytes
 * (e.g. for an object element: from '{' through the matching '}').
 *
 * @param arr        Pointer to the '[' of the array.
 * @param arr_len    Byte length including '[' and ']'.
 * @param index      Zero-based element index.
 * @param item_start On success, set to the first byte of the element.
 * @param item_len   On success, set to the byte count of the element.
 *
 * @return true if element at index exists and was located.
 *         false if index is out of range or arr is malformed.
 */
bool json_get_array_item(const char *arr, size_t arr_len, uint32_t index,
                         const char **item_start, size_t *item_len);

/* ======================================================================
 * Object field extraction
 * ====================================================================== */

/**
 * @brief Extract the string content of a named field from a JSON object.
 *
 * Searches the top-level key-value pairs of the object at *obj* for a key
 * matching *key*. For the matching value:
 *   - If the value is a JSON string  → copies the content WITHOUT quotes.
 *   - If the value is a number/bool/null → copies the raw bytes as-is.
 *   - If the value is an array or object → returns false (use
 *     json_get_raw_field instead to capture those as raw substrings).
 *
 * The output is always null-terminated if the function returns true.
 *
 * @param obj       Pointer to the '{' of the JSON object.
 * @param obj_len   Byte length of the object including '{' and '}'.
 * @param key       Plain C string (no quotes) of the key to find.
 * @param out_buf   Caller-allocated buffer to receive the value string.
 * @param out_size  Size of out_buf in bytes including space for '\0'.
 *
 * @return true  Key found and value copied into out_buf.
 *         false Key not found, value is array/object, or out_buf too small.
 */
bool json_get_string_field(const char *obj, size_t obj_len,
                           const char *key,
                           char *out_buf, size_t out_size);

/**
 * @brief Return a raw pointer and length for the value of a named field.
 *
 * Like json_get_string_field but does NOT copy or strip quotes. The returned
 * slice points directly into the source *obj* buffer and includes all
 * structural characters:
 *   - String value:  slice includes the surrounding '"' characters.
 *   - Number value:  slice is the raw digit/sign bytes.
 *   - Array value:   slice spans from '[' through matching ']'.
 *   - Object value:  slice spans from '{' through matching '}'.
 *
 * This is the correct function to use when capturing the 'ac' (action
 * command template) field from a stored rule, since that field is a nested
 * JSON object that must be forwarded intact.
 *
 * @param obj       Pointer to the '{' of the JSON object.
 * @param obj_len   Byte length including '{' and '}'.
 * @param key       Plain C string (no quotes) of the key to find.
 * @param val_start On success, set to the first byte of the raw value.
 * @param val_len   On success, set to the byte count of the raw value.
 *
 * @return true if key found, false otherwise.
 */
bool json_get_raw_field(const char *obj, size_t obj_len,
                        const char *key,
                        const char **val_start, size_t *val_len);

#ifdef __cplusplus
}
#endif
