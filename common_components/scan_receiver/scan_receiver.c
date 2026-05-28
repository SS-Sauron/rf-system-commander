#include "scan_receiver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "output.h"
#include "watchdog.h"
#include "sdkconfig.h"

static const char *TAG = "scan_receiver";

// UART configuration – from Kconfig and local default pin
#ifndef SCAN_RECEIVER_RX_PIN
#define SCAN_RECEIVER_RX_PIN 16 // match GPIO16 from board_config.h
#endif

#define UART_PORT CONFIG_SCANNER_UART_NUM
#define UART_BAUD CONFIG_SCANNER_UART_BAUD
#define UART_TX_PIN UART_PIN_NO_CHANGE
#define BUF_SIZE 1024
#define TASK_STACK_SIZE 4096

// Debug echo: set 0 to disable the scan_received wrapper line
#ifndef CONFIG_SCAN_RECEIVER_DEBUG_ECHO
#define CONFIG_SCAN_RECEIVER_DEBUG_ECHO 1
#endif

// Line buffer for accumulating received characters
static char s_line_buf[SCAN_RECEIVER_LINE_BUF];

// Queue for delivering raw scanner lines to C2 parser
static QueueHandle_t s_rx_queue = NULL;

static void scan_receiver_task(void *arg)
{
    uint8_t *data = malloc(BUF_SIZE);
    if (!data)
    {
        ESP_LOGE(TAG, "Failed to allocate RX buffer");
        vTaskDelete(NULL);
        return;
    }

    size_t line_pos = 0;
    bool discarding = false; // true when the current line is too long and must be skipped

    while (1)
    {
        // Heartbeat means the receiver loop is alive, not that scanner traffic arrived.
        watchdog_feed_task("scan_recv");

        int len = uart_read_bytes(UART_PORT, data, BUF_SIZE - 1, pdMS_TO_TICKS(100));
        if (len > 0)
        {
            data[len] = 0; // null-terminate for safe character loop

            for (int i = 0; i < len; i++)
            {
                char c = data[i];

                // If we are discarding, ignore everything until end of line
                if (discarding)
                {
                    if (c == '\n' || c == '\r')
                    {
                        discarding = false;
                        line_pos = 0;
                    }
                    continue;
                }

                if (c == '\n' || c == '\r')
                {
                    if (line_pos > 0)
                    {
                        s_line_buf[line_pos] = '\0';

                        // Basic JSON check
                        if (s_line_buf[0] == '{' && s_line_buf[line_pos - 1] == '}')
                        {

                            // ---- Deliver raw line to C2 parser ----
                            if (s_rx_queue != NULL)
                            {
                                if (xQueueSend(s_rx_queue, s_line_buf, 0) != pdTRUE)
                                {
                                    ESP_LOGW(TAG, "rx_queue full – scanner line dropped");
                                    output_write("{\"type\":\"warning\",\"msg\":\"scan_rx_queue_full\"}");
                                }
                            }

                            // ---- Debug echo (optional) ----
#if CONFIG_SCAN_RECEIVER_DEBUG_ECHO
                            char out[SCAN_RECEIVER_LINE_BUF];
                            int ret = snprintf(out, sizeof(out),
                                               "{\"type\":\"scan_received\",\"line\":%.*s}",
                                               (int)strlen(s_line_buf), s_line_buf);
                            if (ret > 0 && ret < sizeof(out))
                            {
                                output_write(out);
                            }
#endif
                        }
                        else
                        {
                            ESP_LOGW(TAG, "Non-JSON line ignored");
                        }
                        line_pos = 0;
                    }
                }
                else if (line_pos < sizeof(s_line_buf) - 1)
                {
                    s_line_buf[line_pos++] = c;
                }
                else
                {
                    // Line too long: start discarding the rest
                    ESP_LOGW(TAG, "Line too long, discarding");
                    discarding = true;
                    line_pos = 0;
                }
            }
        }
    }

    free(data);
    vTaskDelete(NULL);
}

esp_err_t scan_receiver_init(QueueHandle_t rx_queue)
{
    s_rx_queue = rx_queue;

    uart_config_t uart_config = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    esp_err_t ret = uart_driver_install(UART_PORT, BUF_SIZE, 0, 0, NULL, ESP_INTR_FLAG_IRAM);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "UART driver install failed");
        return ret;
    }

    ret = uart_param_config(UART_PORT, &uart_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "UART param config failed");
        uart_driver_delete(UART_PORT);
        return ret;
    }

    ret = uart_set_pin(UART_PORT, UART_TX_PIN, SCAN_RECEIVER_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "UART set pin failed");
        uart_driver_delete(UART_PORT);
        return ret;
    }

    ret = uart_set_rx_full_threshold(UART_PORT, 60);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "UART set RX threshold failed");
        uart_driver_delete(UART_PORT);
        return ret;
    }

    // Register with watchdog BEFORE creating the task, so the task's
    // first heartbeat feed (at the top of its loop) finds a valid entry.
    watchdog_register_task("scan_recv", 5000);

    BaseType_t created = xTaskCreate(scan_receiver_task, "scan_recv",
                                     TASK_STACK_SIZE, NULL, 2, NULL);
    if (created != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create receiver task");
        uart_driver_delete(UART_PORT);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Scanner receiver initialised (UART%d, RX GPIO%d, %d baud)",
             UART_PORT, SCAN_RECEIVER_RX_PIN, UART_BAUD);
    return ESP_OK;
}

esp_err_t scan_receiver_deinit(void)
{
    // TODO: Implement in a later stage when module lifecycle management is added.
    // For now, we simply return success.
    // A full implementation would:
    //   - Delete the receiver task
    //   - Uninstall the UART driver (uart_driver_delete)
    //   - Set s_rx_queue to NULL
    return ESP_OK;
}