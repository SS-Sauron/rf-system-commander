/**
 * @file command_parser.c
 * @brief Serial command parser implementation.
 *
 * COMPONENT LOCATION: common_components/command_parser/
 *
 * ---- Console vs. JSON output channel -----------------------------------
 * This component writes plain text to UART2 (the human console).
 * Machine-readable JSON still flows through output_write() on UART0.
 * These two channels are completely independent.
 *
 * ---- Memory model -------------------------------------------------------
 * line_buf[CONFIG_CMD_LINE_MAX]  stack-allocated in task (256 bytes).
 * s_resp_buf[CMD_RESP_BUF_SIZE]  static module-level for long responses
 *                                 (RULE LIST, STATUS, RULE TEST).
 *                                 Single-task use; no mutex needed.
 *
 * ---- Error handling style -----------------------------------------------
 * No ESP_ERROR_CHECK. Explicit if(ret != ESP_OK) checks throughout.
 * command_parser_init() returns ESP_FAIL on failure; main.c restarts.
 * The task itself never calls esp_restart(); only the watchdog does.
 *
 * ---- Lock ordering ------------------------------------------------------
 * When STATUS or RULE LIST calls rule_engine_get_count() or
 * rule_engine_get_all_rules(), those functions acquire rule_table_mutex
 * then may call snprintf (no output_write). This is safe because:
 *   command_parser_task → rule_table_mutex (brief, no output_mutex inside)
 * The rule engine task acquires rule_table_mutex → output_mutex.
 * Ordering is consistent; no deadlock is possible.
 */

#include "command_parser.h"

#include "rule_engine.h"
#include "action_registry.h"
#include "health.h"
#include "watchdog.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

#include <string.h>
#include <stdlib.h> /* strtol */
#include <ctype.h>  /* toupper */
#include <stdio.h>  /* snprintf */
#include <stdbool.h>

static const char *TAG = "cmd_parser";

/* ---- Compile-time defaults ------------------------------------------- */

#ifndef CONFIG_CMD_LINE_MAX
#define CONFIG_CMD_LINE_MAX 256
#endif
#ifndef CONFIG_CMD_UART_RX_PIN
#define CONFIG_CMD_UART_RX_PIN 18
#endif
#ifndef CONFIG_CMD_UART_TX_PIN
#define CONFIG_CMD_UART_TX_PIN 17
#endif
#ifndef CONFIG_CMD_UART_BAUD
#define CONFIG_CMD_UART_BAUD 115200
#endif
#ifndef CONFIG_CMD_RESP_BUF_SIZE
#define CONFIG_CMD_RESP_BUF_SIZE 2048
#endif
#ifndef CONFIG_MAX_RULES
#define CONFIG_MAX_RULES 16
#endif
#ifndef CONFIG_MAX_ACTION_MODULES
#define CONFIG_MAX_ACTION_MODULES 8
#endif

#define CMD_UART_NUM UART_NUM_2
#define CMD_UART_RX_BUF 512 /* UART driver RX ring buffer */
#define CMD_TASK_STACK 4096
#define CMD_TASK_PRIORITY 2      /* below scan_parser(3), rule_engine(4) */
#define CMD_WDT_INTERVAL_MS 1000 /* fed at loop top every 100 ms timeout */
#define CMD_READ_TIMEOUT_MS 100  /* uart_read_bytes block time */

/* Static response buffer for long outputs (RULE LIST, STATUS, RULE TEST).
 * Single-task use — only command_parser_task writes here. */
static char s_resp_buf[CONFIG_CMD_RESP_BUF_SIZE];

/* ======================================================================
 * Low-level UART write helpers
 * ====================================================================== */

/** Write a null-terminated string to the command console. */
static void cmd_send(const char *str)
{
    if (str == NULL)
        return;
    uart_write_bytes(CMD_UART_NUM, str, strlen(str));
}

/** Write the command prompt. Called after every response. */
static void cmd_send_prompt(void)
{
    uart_write_bytes(CMD_UART_NUM, "\r\n> ", 4);
}

/* ======================================================================
 * Token extraction helpers
 * ====================================================================== */

/**
 * @brief Skip leading whitespace and return pointer to first non-ws char.
 */
static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t')
    {
        p++;
    }
    return p;
}

/**
 * @brief Extract and uppercase the next whitespace-delimited token from *pp.
 *
 * Advances *pp past the token and any trailing whitespace.
 * Writes at most out_size-1 uppercase chars into out (null-terminated).
 * Returns the number of characters copied (0 if no token found).
 */
static size_t next_token_upper(const char **pp, char *out, size_t out_size)
{
    const char *p = skip_ws(*pp);
    if (*p == '\0')
    {
        out[0] = '\0';
        *pp = p;
        return 0;
    }

    size_t len = 0;
    while (*p && *p != ' ' && *p != '\t' && len < out_size - 1)
    {
        out[len++] = (char)toupper((unsigned char)*p);
        p++;
    }
    out[len] = '\0';
    *pp = skip_ws(p); /* advance past token + trailing ws */
    return len;
}

/* ======================================================================
 * Command handlers
 * ====================================================================== */

/* ---------- HELP ------------------------------------------------------- */

static void handle_help(void)
{
    /* Static string literal lives in flash — zero DRAM cost. */
    cmd_send(
        "\r\nCommander Command Reference\r\n"
        "------------------------------\r\n"
        "RULE ADD <json>        Add rule. Schema:\r\n"
        "                        {\"name\":\"<n>\",\"on\":1,\r\n"
        "                         \"cm\":\"<module>\",\"cf\":\"<field>\",\r\n"
        "                         \"co\":\"<op>\",\"cv\":\"<val>\",\r\n"
        "                         \"am\":\"<action_mod>\",\r\n"
        "                         \"ac\":{...},\"fm\":0}\r\n"
        "                       Operators: eq ne contains gt lt\r\n"
        "RULE DELETE <n>        Delete rule at index n\r\n"
        "RULE ENABLE <n>        Enable rule at index n\r\n"
        "RULE DISABLE <n>       Disable rule at index n\r\n"
        "RULE LIST              List all rules with index and status\r\n"
        "RULE TEST <json>       Dry-run; shows which rules would fire\r\n"
        "                        Event JSON: {\"module\":\"<m>\",\"data\":[...]}\r\n"
        "STATUS                 Heap, uptime, task watermarks, rule/module counts\r\n"
        "GET_STATUS <module>    Module-specific status from get_status()\r\n"
        "HELP                   This help text\r\n");
}

/* ---------- RULE subcommand dispatcher --------------------------------- */

static void handle_rule_add(const char *json_arg)
{
    if (*json_arg == '\0')
    {
        cmd_send("ERROR missing JSON argument");
        return;
    }
    int idx = -1;
    esp_err_t ret = rule_engine_add_rule(json_arg, &idx);
    if (ret == ESP_OK)
    {
        char resp[32];
        snprintf(resp, sizeof(resp), "OK index=%d", idx);
        cmd_send(resp);
    }
    else
    {
        char resp[80];
        snprintf(resp, sizeof(resp), "ERROR %s", esp_err_to_name(ret));
        cmd_send(resp);
    }
}

static void handle_rule_delete(const char *idx_arg)
{
    if (*idx_arg == '\0')
    {
        cmd_send("ERROR missing index");
        return;
    }
    int idx = (int)strtol(idx_arg, NULL, 10);
    esp_err_t ret = rule_engine_delete_rule(idx);
    cmd_send(ret == ESP_OK ? "OK" : "ERROR rule not found or invalid index");
}

static void handle_rule_enable(const char *idx_arg, bool enable)
{
    if (*idx_arg == '\0')
    {
        cmd_send("ERROR missing index");
        return;
    }
    int idx = (int)strtol(idx_arg, NULL, 10);
    esp_err_t ret = rule_engine_set_enabled(idx, enable);
    if (ret == ESP_OK)
    {
        cmd_send("OK");
    }
    else
    {
        char resp[80];
        snprintf(resp, sizeof(resp), "ERROR %s", esp_err_to_name(ret));
        cmd_send(resp);
    }
}

static void handle_rule_list(void)
{
    esp_err_t ret = rule_engine_get_all_rules(s_resp_buf, sizeof(s_resp_buf));
    if (ret == ESP_OK)
    {
        cmd_send(s_resp_buf);
    }
    else
    {
        char resp[80];
        snprintf(resp, sizeof(resp), "ERROR %s", esp_err_to_name(ret));
        cmd_send(resp);
    }
}

static void handle_rule_test(const char *event_json)
{
    if (*event_json == '\0')
    {
        cmd_send("ERROR missing event JSON");
        return;
    }
    esp_err_t ret = rule_engine_test_event(event_json, s_resp_buf, sizeof(s_resp_buf));
    if (ret == ESP_OK)
    {
        cmd_send(s_resp_buf);
    }
    else
    {
        char resp[80];
        snprintf(resp, sizeof(resp), "ERROR %s", esp_err_to_name(ret));
        cmd_send(resp);
    }
}

static void handle_rule(const char *args)
{
    char sub[16];
    const char *rest = args;
    size_t sub_len = next_token_upper(&rest, sub, sizeof(sub));

    if (sub_len == 0)
    {
        cmd_send("ERROR missing RULE subcommand");
        return;
    }

    if (strcmp(sub, "ADD") == 0)
    {
        handle_rule_add(rest);
    }
    else if (strcmp(sub, "DELETE") == 0)
    {
        handle_rule_delete(rest);
    }
    else if (strcmp(sub, "ENABLE") == 0)
    {
        handle_rule_enable(rest, true);
    }
    else if (strcmp(sub, "DISABLE") == 0)
    {
        handle_rule_enable(rest, false);
    }
    else if (strcmp(sub, "LIST") == 0)
    {
        handle_rule_list();
    }
    else if (strcmp(sub, "TEST") == 0)
    {
        handle_rule_test(rest);
    }
    else
    {
        char resp[48];
        snprintf(resp, sizeof(resp), "ERROR unknown RULE subcommand '%s'", sub);
        cmd_send(resp);
    }
}

/* ---------- STATUS ----------------------------------------------------- */

static void handle_status(void)
{
    size_t pos = 0;
    int w;

    /* Health snapshot fills the beginning of s_resp_buf. */
    esp_err_t ret = health_get_snapshot(s_resp_buf, sizeof(s_resp_buf));
    if (ret == ESP_OK)
    {
        pos = strlen(s_resp_buf);
    }
    else
    {
        w = snprintf(s_resp_buf, sizeof(s_resp_buf),
                     "(health snapshot unavailable: %s)\r\n",
                     esp_err_to_name(ret));
        pos = (w > 0) ? (size_t)w : 0;
    }

    /* Append rule and module counts. */
    int rule_count = rule_engine_get_count();
    w = snprintf(s_resp_buf + pos, sizeof(s_resp_buf) - pos,
                 "Rules: %d/%d\r\n"
                 "Action modules: %d/%d\r\n",
                 rule_count, CONFIG_MAX_RULES,
                 action_registry_get_module_count(), CONFIG_MAX_ACTION_MODULES);
    if (w > 0 && pos + (size_t)w < sizeof(s_resp_buf))
    {
        pos += (size_t)w;
    }

    cmd_send(s_resp_buf);
}

/* ---------- GET_STATUS ------------------------------------------------- */

static void handle_get_status(const char *module_name)
{
    if (*module_name == '\0')
    {
        cmd_send("ERROR missing module name");
        return;
    }

    const action_module_t *mod = action_registry_get_module(module_name);
    if (mod == NULL)
    {
        cmd_send("ERROR module not registered");
        return;
    }
    if (mod->get_status == NULL)
    {
        cmd_send("ERROR module does not implement get_status");
        return;
    }

    size_t len = sizeof(s_resp_buf);
    esp_err_t ret = mod->get_status(s_resp_buf, &len);
    if (ret != ESP_OK)
    {
        char resp[80];
        snprintf(resp, sizeof(resp), "ERROR get_status failed: %s",
                 esp_err_to_name(ret));
        cmd_send(resp);
        return;
    }
    cmd_send(s_resp_buf);
}

/* ======================================================================
 * Main command dispatcher
 * ====================================================================== */

static void process_command(char *line)
{
    /* Skip leading whitespace. */
    const char *p = (const char *)line;

    char cmd[16];
    size_t cmd_len = next_token_upper(&p, cmd, sizeof(cmd));

    if (cmd_len == 0)
        return; /* blank line */

    if (strcmp(cmd, "RULE") == 0)
    {
        handle_rule(p);
    }
    else if (strcmp(cmd, "STATUS") == 0)
    {
        handle_status();
    }
    else if (strcmp(cmd, "GET_STATUS") == 0)
    {
        handle_get_status(p);
    }
    else if (strcmp(cmd, "HELP") == 0)
    {
        handle_help();
    }
    else
    {
        char resp[64];
        snprintf(resp, sizeof(resp), "ERROR unknown command '%s'", cmd);
        cmd_send(resp);
    }
}

/* ======================================================================
 * Command parser task
 * ====================================================================== */

static void command_parser_task(void *arg)
{
    (void)arg;

    /* Stack-allocated line buffer per spec. */
    char line_buf[CONFIG_CMD_LINE_MAX];
    int pos = 0;
    bool line_too_long = false;

    /* Welcome banner and initial prompt. */
    cmd_send("\r\nCommander ready. Type HELP for commands.\r\n> ");
    ESP_LOGI(TAG, "Welcome banner sent");

    bool last_was_cr = false;

    while (1)
    {
        /* Feed watchdog BEFORE blocking — ensures heartbeat even with no input. */
        watchdog_feed_task("command_parser");

        /* Read one byte at a time. 100 ms timeout keeps the watchdog feed
         * cadence well within the 1000 ms watchdog interval. */
        uint8_t ch = 0;
        int n = uart_read_bytes(CMD_UART_NUM, &ch, 1,
                                pdMS_TO_TICKS(CMD_READ_TIMEOUT_MS));
        if (n <= 0)
        {
            continue; /* timeout — loop back and feed watchdog */
        }

        /* Accept CR, LF, and CRLF as line terminators. For CRLF, process
         * the command on CR and ignore the immediately following LF. */
        if (ch == '\n' && last_was_cr)
        {
            last_was_cr = false;
            continue;
        }

        if (ch == '\r' || ch == '\n')
        {
            last_was_cr = (ch == '\r');

            if (line_too_long)
            {
                /* Discard overlong line, report error. */
                cmd_send("ERROR line too long");
                cmd_send_prompt();
                line_too_long = false;
                pos = 0;
                continue;
            }

            /* Strip trailing whitespace. */
            while (pos > 0 && (line_buf[pos - 1] == ' ' ||
                               line_buf[pos - 1] == '\t'))
            {
                pos--;
            }
            line_buf[pos] = '\0';

            if (pos > 0)
            {
                process_command(line_buf);
            }

            cmd_send_prompt();
            pos = 0;
            continue;
        }

        /* Accumulate character. */
        if (pos >= CONFIG_CMD_LINE_MAX - 1)
        {
            /* Buffer full: set flag and keep consuming until LF. */
            line_too_long = true;
            continue;
        }
        line_buf[pos++] = (char)ch;
        last_was_cr = false;
    }

    /* Unreachable. */
    vTaskDelete(NULL);
}

/* ======================================================================
 * Public API
 * ====================================================================== */

esp_err_t command_parser_init(void)
{
    /* ---- UART driver install ---- */
    /* RX ring buffer 512 bytes; no TX ring buffer (direct write mode);
     * no event queue needed for simple line-based reading. */
    esp_err_t ret = uart_driver_install(CMD_UART_NUM,
                                        CMD_UART_RX_BUF, 0, 0, NULL, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    /* ---- UART peripheral config ---- */
    uart_config_t cfg = {
        .baud_rate = CONFIG_CMD_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ret = uart_param_config(CMD_UART_NUM, &cfg);
    if (ret != ESP_OK)
    {
        uart_driver_delete(CMD_UART_NUM);
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    /* ---- Pin assignment ---- */
    ret = uart_set_pin(CMD_UART_NUM,
                       CONFIG_CMD_UART_TX_PIN,
                       CONFIG_CMD_UART_RX_PIN,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (ret != ESP_OK)
    {
        uart_driver_delete(CMD_UART_NUM);
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    /* ---- Watchdog registration BEFORE task creation ---- */
    watchdog_register_task("command_parser", CMD_WDT_INTERVAL_MS);

    /* ---- Task creation ---- */
    if (xTaskCreate(command_parser_task, "cmd_parser",
                    CMD_TASK_STACK, NULL,
                    CMD_TASK_PRIORITY, NULL) != pdPASS)
    {
        uart_driver_delete(CMD_UART_NUM);
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Command parser ready: UART%d RX=GPIO%d TX=GPIO%d %d baud",
             CMD_UART_NUM,
             CONFIG_CMD_UART_RX_PIN, CONFIG_CMD_UART_TX_PIN,
             CONFIG_CMD_UART_BAUD);
    return ESP_OK;
}
