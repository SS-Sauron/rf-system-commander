/**
 * @file scan_parser.h
 * @brief Scanner JSON parser — shared component.
 *
 * Reads raw JSON lines from a FreeRTOS queue (produced by scan_receiver),
 * validates that each line is a well-formed "scan_result" envelope,
 * extracts the module name and data array, and:
 *   a) Writes a structured {"type":"scan_parsed",...} line via output_write().
 *   b) Posts a scan_event_t to the rule engine's event queue (C3+).
 *
 * COMPONENT LOCATION: common_components/scan_parser/
 *
 * DATA FLOW:
 *   scan_receiver → [scan_rx_queue] → scan_parser → output_write()
 *                                                  → [scan_event_queue] → rule_engine (C3+)
 *
 * MEMORY MODEL: All buffers are statically allocated inside scan_parser.c.
 * No heap allocation occurs after scan_parser_init() returns. The caller's
 * queue handles are stored by reference; the caller owns the queue lifetime.
 *
 * SIZE CONSTRAINT:
 *   CONFIG_SCAN_PARSER_DATA_BUF_SIZE + ~200 bytes of body overhead must fit
 *   within CONFIG_OUTPUT_LINE_BUF_SIZE. If scan results are truncated in the
 *   output, increase CONFIG_OUTPUT_LINE_BUF_SIZE (recommend 2048 minimum).
 */

#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Compile-time defaults (override via project Kconfig) ------------- */

/** Current API version of scan_event_t.
 *  The rule engine (C3) checks this on every event to detect component skew.
 *  Increment whenever scan_event_t's binary layout changes. */
#define SCAN_PARSER_API_VERSION     1U

/** Maximum module name length including null terminator (e.g. "bt_classic"). */
#define SCAN_PARSER_MODULE_NAME_LEN 32U

/** Default data buffer size if Kconfig value is absent. */
#ifndef CONFIG_SCAN_PARSER_DATA_BUF_SIZE
#define CONFIG_SCAN_PARSER_DATA_BUF_SIZE 800
#endif

/* ---- Public types ----------------------------------------------------- */

/**
 * @brief Parsed representation of one scanner event.
 *
 * Produced by scan_parser_task and posted to the rule engine queue (C3+).
 * All storage is fixed-size; no pointers into temporary buffers.
 *
 * Field notes:
 *   api_version  — checked by rule engine before use; drop events where this
 *                  does not equal SCAN_PARSER_API_VERSION.
 *   module       — null-terminated scanner module name ("wifi", "ble",
 *                  "bt_classic", "dummy", ...).
 *   item_count   — number of top-level elements in the data[] array.
 *                  0 is valid (scanner produced an empty result).
 *   timestamp_ms — value of the "timestamp" field in the scanner JSON,
 *                  in milliseconds. 0 if the field was absent.
 *   data_json    — raw JSON array string including brackets, e.g.
 *                  [{"ssid":"X","rssi":-70},{"ssid":"Y","rssi":-65}].
 *                  Null-terminated. May be "[]" for an empty scan.
 */
typedef struct {
    uint16_t api_version;
    char     module[SCAN_PARSER_MODULE_NAME_LEN];
    uint32_t item_count;
    int64_t  timestamp_ms;
    char     data_json[CONFIG_SCAN_PARSER_DATA_BUF_SIZE];
} scan_event_t;

/* ---- Public API ------------------------------------------------------- */

/**
 * @brief Initialise the scan parser component.
 *
 * Creates a FreeRTOS task that reads raw JSON strings from rx_queue,
 * validates them, parses the scanner envelope, and outputs structured
 * JSON via output_write(). Optionally posts parsed scan_event_t structs
 * to event_queue for the rule engine.
 *
 * Must be called after output_init() and watchdog_start().
 * Must be called only once; a second call returns ESP_ERR_INVALID_STATE.
 *
 * @param rx_queue     Queue from which raw scanner lines are consumed.
 *                     Item size must be SCAN_RECEIVER_LINE_BUF bytes.
 *                     Must not be NULL.
 * @param event_queue  Queue to which scan_event_t structs are posted for
 *                     the rule engine. Pass NULL until C3 is implemented.
 *                     Item size must be sizeof(scan_event_t).
 *
 * @return  ESP_OK              Task created and running.
 *          ESP_ERR_INVALID_ARG rx_queue is NULL.
 *          ESP_ERR_INVALID_STATE Already initialised.
 *          ESP_FAIL            Task creation failed (out of heap at boot).
 */
esp_err_t scan_parser_init(QueueHandle_t rx_queue, QueueHandle_t event_queue);

/**
 * @brief Stop the parser task and release internal state.
 *
 * Safe to call only after a successful scan_parser_init().
 * After this returns, scan_parser_init() may be called again.
 *
 * @return ESP_OK on success.
 */
esp_err_t scan_parser_deinit(void);

#ifdef __cplusplus
}
#endif
