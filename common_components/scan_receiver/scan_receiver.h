#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C"
{
#endif

/** Maximum length of one raw scanner JSON line, including null terminator. */
#define SCAN_RECEIVER_LINE_BUF 1024

/**
 * @brief Recommended queue depth for the rx_queue argument.
 * Create the queue as:
 *   xQueueCreate(SCAN_RECEIVER_QUEUE_DEPTH, SCAN_RECEIVER_LINE_BUF)
 */
#define SCAN_RECEIVER_QUEUE_DEPTH 8

    /**
     * @brief Initialise the hardened UART receiver for scanner JSON lines.
     *
     * @param rx_queue  Queue to which validated raw JSON lines are posted.
     *                  Item size must be SCAN_RECEIVER_LINE_BUF bytes.
     *                  If NULL, only the debug echo via output_write is active.
     * @return ESP_OK on success.
     */
    esp_err_t scan_receiver_init(QueueHandle_t rx_queue);

    /**
     * @brief Stop the receiver task and uninstall the UART driver.
     * Call only after a successful scan_receiver_init().
     * @return ESP_OK on success.
     */
    esp_err_t scan_receiver_deinit(void);

#ifdef __cplusplus
}
#endif