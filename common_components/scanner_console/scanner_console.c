/* common_components/scanner_console/scanner_console.c
 *
 * Local command console — UART2, human operator interface.
 *
 * ---- Design notes -------------------------------------------------------
 *
 * All responses go to UART2 via uart_write_bytes(). output_write() is
 * never called from this file.
 *
 * Line ending handling: terminals send \r, \n, or \r\n. The task handles
 * all three using a last_was_cr flag to suppress the \n that follows a \r
 * in CR+LF sequences, preventing double-processing.
 *
 * The line buffer is stack-allocated (CONFIG_SCANNER_CONSOLE_LINE_MAX bytes).
 * Lines exceeding that limit produce "ERROR line too long" and are flushed.
 *
 * Watchdog: registered as "scanner_console" with 1000 ms interval.
 * Fed at the top of every task loop iteration before uart_read_bytes.
 * uart_read_bytes uses a 100 ms timeout so the feed cadence is ~100 ms,
 * well within the 1000 ms watchdog interval.
 */

#include "scanner_console.h"

#include "scanner_core.h"
#include "health.h"
#include "watchdog.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include "esp_log.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>

static const char *TAG = "scanner_console";

/* ======================================================================
 * Compile-time defaults
 * ====================================================================== */

#ifndef CONFIG_SCANNER_CMD_UART_RX_PIN
#define CONFIG_SCANNER_CMD_UART_RX_PIN  18
#endif
#ifndef CONFIG_SCANNER_CMD_UART_TX_PIN
#define CONFIG_SCANNER_CMD_UART_TX_PIN  21
#endif
#ifndef CONFIG_SCANNER_CMD_UART_BAUD
#define CONFIG_SCANNER_CMD_UART_BAUD    115200
#endif
#ifndef CONFIG_SCANNER_CONSOLE_LINE_MAX
#define CONFIG_SCANNER_CONSOLE_LINE_MAX 256
#endif

#define CONSOLE_UART_NUM    UART_NUM_2
#define CONSOLE_UART_RX_BUF 512
#define CONSOLE_TASK_STACK  4096
#define CONSOLE_TASK_PRIO   2
#define CONSOLE_WDT_MS      1000
#define CONSOLE_READ_MS     100

/* Response buffer for STATUS output (stack in process_cmd). */
#define STATUS_BUF_SIZE     2048

/* ======================================================================
 * UART write helpers
 * ====================================================================== */

static void con_send(const char *str)
{
    if (str == NULL) return;
    uart_write_bytes(CONSOLE_UART_NUM, str, strlen(str));
}

static void con_prompt(void)
{
    uart_write_bytes(CONSOLE_UART_NUM, "\r\n> ", 4);
}

/* ======================================================================
 * Token extraction — uppercase next whitespace-delimited token
 * ====================================================================== */

static size_t next_token_upper(const char **pp, char *out, size_t out_size)
{
    /* Skip leading whitespace. */
    while (**pp == ' ' || **pp == '\t') { (*pp)++; }
    if (**pp == '\0') { out[0] = '\0'; return 0; }

    size_t len = 0;
    while (**pp && **pp != ' ' && **pp != '\t' && len < out_size - 1) {
        out[len++] = (char)toupper((unsigned char)**pp);
        (*pp)++;
    }
    out[len] = '\0';

    /* Skip trailing whitespace after token. */
    while (**pp == ' ' || **pp == '\t') { (*pp)++; }
    return len;
}

/* ======================================================================
 * Command handlers
 * ====================================================================== */

static void handle_help(void)
{
    con_send(
        "\r\nScanner Command Reference\r\n"
        "--------------------------\r\n"
        "SCAN BLE START   Start dummy BLE scan events\r\n"
        "SCAN BLE STOP    Stop  dummy BLE scan events\r\n"
        "SCAN WIFI START  ERROR: not implemented\r\n"
        "SCAN WIFI STOP   ERROR: not implemented\r\n"
        "STATUS           System health snapshot\r\n"
        "HELP             This help text\r\n"
    );
}

static void handle_status(void)
{
    /* Static buffer — single-caller contract (scanner_console_task only). */
    static char s_status_buf[STATUS_BUF_SIZE];
    esp_err_t ret = health_get_snapshot(s_status_buf, sizeof(s_status_buf));
    if (ret == ESP_OK) {
        con_send(s_status_buf);
    } else {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "ERROR health_get_snapshot: %s",
                 esp_err_to_name(ret));
        con_send(tmp);
    }
}

static void handle_scan(const char *args)
{
    char medium[16];
    char action[16];
    const char *p = args;

    if (next_token_upper(&p, medium, sizeof(medium)) == 0) {
        con_send("ERROR missing scan medium (BLE/WIFI)");
        return;
    }
    if (next_token_upper(&p, action, sizeof(action)) == 0) {
        con_send("ERROR missing action (START/STOP)");
        return;
    }

    if (strcmp(medium, "WIFI") == 0) {
        con_send("ERROR not implemented");
        return;
    }

    if (strcmp(medium, "BLE") != 0) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "ERROR unknown medium '%s'", medium);
        con_send(tmp);
        return;
    }

    if (strcmp(action, "START") == 0) {
        scanner_core_start_ble();
        con_send("OK");
    } else if (strcmp(action, "STOP") == 0) {
        scanner_core_stop_ble();
        con_send("OK");
    } else {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "ERROR unknown action '%s'", action);
        con_send(tmp);
    }
}

/* ======================================================================
 * Main command dispatcher
 * ====================================================================== */

static void process_cmd(char *line)
{
    const char *p = (const char *)line;
    char cmd[16];

    if (next_token_upper(&p, cmd, sizeof(cmd)) == 0) {
        return;   /* blank line */
    }

    if      (strcmp(cmd, "HELP")   == 0) { handle_help(); }
    else if (strcmp(cmd, "STATUS") == 0) { handle_status(); }
    else if (strcmp(cmd, "SCAN")   == 0) { handle_scan(p); }
    else {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "ERROR unknown command '%s'", cmd);
        con_send(tmp);
    }
}

/* ======================================================================
 * Console task
 * ====================================================================== */

static void scanner_console_task(void *arg)
{
    (void)arg;

    char line_buf[CONFIG_SCANNER_CONSOLE_LINE_MAX];
    int  pos          = 0;
    bool line_too_long = false;
    bool last_was_cr   = false;

    con_send("\r\nScanner console ready. Type HELP.\r\n> ");

    while (1) {
        /* Feed watchdog FIRST — before any blocking operation. */
        watchdog_feed_task("scanner_console");

        uint8_t ch = 0;
        int n = uart_read_bytes(CONSOLE_UART_NUM, &ch, 1,
                                pdMS_TO_TICKS(CONSOLE_READ_MS));
        if (n <= 0) { last_was_cr = false; continue; }

        /* Detect CR+LF: suppress the \n that immediately follows a \r. */
        bool is_cr = (ch == '\r');
        bool is_lf = (ch == '\n');
        bool suppress = (is_lf && last_was_cr);
        last_was_cr = is_cr;

        if (suppress) { continue; }

        if (is_cr || is_lf) {
            line_buf[pos] = '\0';

            if (line_too_long) {
                con_send("ERROR line too long");
                line_too_long = false;
                pos = 0;
            } else {
                /* Strip trailing whitespace. */
                while (pos > 0 &&
                       (line_buf[pos - 1] == ' ' ||
                        line_buf[pos - 1] == '\t')) {
                    pos--;
                }
                line_buf[pos] = '\0';
                if (pos > 0) {
                    process_cmd(line_buf);
                }
                pos = 0;
            }

            con_prompt();
            continue;
        }

        /* Regular character — accumulate. */
        if (pos >= CONFIG_SCANNER_CONSOLE_LINE_MAX - 1) {
            line_too_long = true;
            continue;
        }
        line_buf[pos++] = (char)ch;
    }

    vTaskDelete(NULL);
}

/* ======================================================================
 * Public API
 * ====================================================================== */

esp_err_t scanner_console_init(void)
{
    /* UART driver install — RX ring buffer only; no TX buffer (direct write). */
    esp_err_t ret = uart_driver_install(CONSOLE_UART_NUM,
                                        CONSOLE_UART_RX_BUF, 0, 0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    uart_config_t cfg = {
        .baud_rate  = CONFIG_SCANNER_CMD_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ret = uart_param_config(CONSOLE_UART_NUM, &cfg);
    if (ret != ESP_OK) {
        uart_driver_delete(CONSOLE_UART_NUM);
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    ret = uart_set_pin(CONSOLE_UART_NUM,
                       CONFIG_SCANNER_CMD_UART_TX_PIN,
                       CONFIG_SCANNER_CMD_UART_RX_PIN,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        uart_driver_delete(CONSOLE_UART_NUM);
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    /* Register with watchdog BEFORE task creation. */
    watchdog_register_task("scanner_console", CONSOLE_WDT_MS);

    if (xTaskCreate(scanner_console_task, "scanner_console",
                    CONSOLE_TASK_STACK, NULL,
                    CONSOLE_TASK_PRIO, NULL) != pdPASS) {
        uart_driver_delete(CONSOLE_UART_NUM);
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Console ready: UART%d RX=GPIO%d TX=GPIO%d %d baud",
             CONSOLE_UART_NUM,
             CONFIG_SCANNER_CMD_UART_RX_PIN,
             CONFIG_SCANNER_CMD_UART_TX_PIN,
             CONFIG_SCANNER_CMD_UART_BAUD);
    return ESP_OK;
}
