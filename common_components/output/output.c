/**
 * @file output.c
 * @brief Machine-readable JSON output channel implementation.
 *
 * COMPONENT LOCATION: common_components/output/
 *
 * Configuration (set via project Kconfig or falls back to default):
 *   CONFIG_OUTPUT_LINE_BUF_SIZE — max bytes per assembled output line
 *                                 (default 1024, must cover envelope +
 *                                 the largest body the caller will produce)
 *
 * ---- C0 → C1 transport note ----------------------------------------
 * The transport inside output_write() is currently printf(). This is
 * intentional at Stage C0: the UART driver for the commander's output
 * channel is not yet configured. At Stage C1 this printf() call will be
 * replaced with uart_write_bytes() and the UART driver will be installed
 * in output_init(). The change is local to this file; callers are unaffected.
 *
 * Search for the string "C1-REPLACE" to locate every line that must change.
 *
 * ---- Memory safety note (Stage C1) ----------------------------------
 * The envelope assembly buffer was originally stack‑allocated inside
 * output_write(). That caused a stack overflow in the scan_recv task when
 * two 1024‑byte buffers were live simultaneously.  The buffer is now
 * **static** and protected by the same mutex.  No task stack is consumed
 * here, and the mutex prevents concurrent access.
 * --------------------------------------------------------------------
 */

#include "output.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "output";

/* ---- Compile-time defaults (override via project Kconfig) ------------- */

#ifndef CONFIG_OUTPUT_LINE_BUF_SIZE
#define CONFIG_OUTPUT_LINE_BUF_SIZE 1024
#endif

/* Mutex acquisition timeout. 100 ms is generous for a UART write;
 * if the mutex is held longer than this, something has deadlocked. */
#define OUTPUT_MUTEX_TIMEOUT_MS  100

/* ---- Module state ----------------------------------------------------- */

static SemaphoreHandle_t s_mutex     = NULL;
static const char       *s_source    = NULL;
static const char       *s_device_id = NULL;
static bool              s_initialized = false;

/* ---- Static assembly buffer ------------------------------------------- */
/*
 * Formerly a stack‑local inside output_write().  Now a single, shared
 * buffer guarded by s_mutex.  Callers no longer need to reserve 1 KB
 * of stack space just to print a JSON line.
 */
static char s_line[CONFIG_OUTPUT_LINE_BUF_SIZE];

/* ---- Public API ------------------------------------------------------- */

esp_err_t output_init(const char *source, const char *device_id)
{
    if (source == NULL || device_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_initialized) {
        ESP_LOGW(TAG, "output_init called more than once — ignored");
        return ESP_ERR_INVALID_STATE;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        /* printf: ESP_LOG may not be routed yet at this point in boot. */
        printf("[output] FATAL: mutex creation failed — out of heap at boot\n");
        return ESP_ERR_NO_MEM;
    }

    s_source    = source;
    s_device_id = device_id;
    s_initialized = true;

    return ESP_OK;
}

esp_err_t output_write(const char *json_body)
{
    /* ---- Pre-condition checks ---------------------------------------- */
    if (!s_initialized) {
        ESP_LOGE(TAG, "output_write called before output_init — dropping line");
        return ESP_ERR_INVALID_STATE;
    }

    if (json_body == NULL) {
        ESP_LOGE(TAG, "output_write: json_body is NULL — dropping");
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate that the body looks like a JSON object. A string that does
     * not start with '{' is a caller bug; reject it rather than corrupt the
     * output stream with a malformed envelope. */
    if (json_body[0] != '{') {
        ESP_LOGE(TAG, "output_write: body does not start with '{' (got 0x%02X) — dropping",
                 (unsigned char)json_body[0]);
        return ESP_ERR_INVALID_ARG;
    }

    /* ---- Acquire mutex ------------------------------------------------ */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(OUTPUT_MUTEX_TIMEOUT_MS)) != pdTRUE) {
        /* Timeout: another task is holding the output channel. This is
         * abnormal — log via ESP_LOGE (which does NOT go through this path)
         * and return rather than block forever. */
        ESP_LOGE(TAG, "output_write: mutex timeout after %d ms — dropping line",
                 OUTPUT_MUTEX_TIMEOUT_MS);
        return ESP_ERR_TIMEOUT;
    }

    /* ---- Assemble envelope (now in static buffer, not on stack) ------- */
    int64_t uptime_ms = esp_timer_get_time() / 1000LL;

    int written = snprintf(s_line, sizeof(s_line),
        "{\"source\":\"%s\",\"device_id\":\"%s\",\"uptime_ms\":%lld,\"msg\":%s}\n",
        s_source,
        s_device_id,
        (long long)uptime_ms,
        json_body);

    /* snprintf return value is the number of bytes that WOULD have been
     * written if the buffer were large enough (excluding '\0').
     * If written >= sizeof(s_line), truncation occurred. */
    if (written < 0 || written >= (int)sizeof(s_line)) {
        ESP_LOGE(TAG,
                 "output_write: line overflow (need %d bytes, buf is %d) — dropping. "
                 "Increase CONFIG_OUTPUT_LINE_BUF_SIZE or shorten the body.",
                 written, CONFIG_OUTPUT_LINE_BUF_SIZE);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_SIZE;
    }

    /* ---- Write to transport ------------------------------------------ */
    /* C1-REPLACE: replace printf with uart_write_bytes(UART_NUM, s_line, written) */
    printf("%s", s_line);   /* Temporary C0 transport — see file header note. */

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}
