/**
 * @file output.h
 * @brief Machine-readable JSON output channel — shared component.
 *
 * Provides a single, thread-safe function for writing JSON lines to the
 * machine-readable output stream (the channel read by a host parser or
 * the paired scanner/commander device).
 *
 * CONTRACT — all callers must honour this:
 *   1. output_init() MUST be called exactly once before output_write().
 *   2. output_write() accepts a JSON object string starting with '{'.
 *      It wraps the body in a standard envelope and appends '\n'.
 *   3. output_write() is thread-safe. It may be called from any task.
 *      It is NOT callable from an ISR.
 *   4. The caller's json_body string is read and released before the
 *      function returns; the caller may free or reuse the buffer immediately.
 *   5. output_write() is NOT re‑entrant. Do not call it from within
 *      an output_write callback, error handler, or any code that may
 *      already hold the internal output mutex.
 *
 * OUTPUT FORMAT (one line per call):
 *   {"source":"<src>","device_id":"<id>","uptime_ms":<ms>,"msg":<body>}\n
 *
 *   source    — set at init(), e.g. "scanner" or "commander"
 *   device_id — set at init(), e.g. "cmd_001" (from CONFIG_DEVICE_ID)
 *   uptime_ms — milliseconds since boot (esp_timer_get_time / 1000)
 *   msg       — the caller's json_body embedded as a nested JSON object
 *
 * NOTE (C0 → C1 transition):
 *   At Stage C0 the internal transport is printf(). Stage C1 replaces
 *   this with uart_write_bytes() once the commander's output UART driver
 *   is initialised. The public API does not change between stages.
 *
 * COMPONENT LOCATION: common_components/output/
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Initialise the output channel.
     *
     * Creates the internal mutex and stores the identity strings.
     * Must be called before any call to output_write().
     *
     * @param source     Short string identifying the device role.
     *                   E.g. "scanner" or "commander".
     *                   Must be a string literal or static buffer (permanent).
     * @param device_id  Unique device identifier (from CONFIG_DEVICE_ID).
     *                   Must be a string literal or static buffer (permanent).
     *
     * @return  ESP_OK              Initialised successfully.
     *          ESP_ERR_INVALID_ARG source or device_id is NULL.
     *          ESP_ERR_INVALID_STATE Already initialised (second call ignored).
     *          ESP_ERR_NO_MEM      Mutex creation failed.
     */
    esp_err_t output_init(const char *source, const char *device_id);

    /**
     * @brief Write a JSON body to the output channel.
     *
     * Envelopes json_body and writes one newline-terminated JSON line.
     * Thread-safe: protected by an internal mutex (100 ms acquire timeout).
     *
     * The assembled line must fit in CONFIG_OUTPUT_LINE_BUF_SIZE bytes
     * (including the envelope fields and the trailing '\n'). If the assembled
     * line would overflow the buffer the body is dropped and an error is logged
     * via ESP_LOGE — a broken JSON line is never written to the output stream.
     *
     * @param json_body  Null-terminated JSON object string. MUST start with '{'.
     *                   Strings that do not start with '{' are rejected silently
     *                   (error logged; nothing written).
     *
     * @return  ESP_OK                Output written successfully.
     *          ESP_ERR_INVALID_STATE output_init() not yet called.
     *          ESP_ERR_INVALID_ARG   json_body is NULL or does not start with '{'.
     *          ESP_ERR_INVALID_SIZE  Assembled line exceeds buffer — body dropped.
     *          ESP_ERR_TIMEOUT       Mutex not acquired within 100 ms.
     */
    esp_err_t output_write(const char *json_body);

#ifdef __cplusplus
}
#endif
