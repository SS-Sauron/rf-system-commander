/* common_components/bt_media_action/bt_media_action.c
 *
 * Bluetooth AVRCP Controller + A2DP Source action module.
 * Corrected against official Espressif ESP-IDF documentation.
 *
 * ---- Key corrections from documentation review ---------------------------
 *
 * 1. esp_bt_controller_mem_release(ESP_BT_MODE_BLE) called before controller
 *    init to free BLE memory when only Classic BT is used.
 *
 * 2. esp_bluedroid_init_with_cfg() + BT_BLUEDROID_INIT_CONFIG_DEFAULT() used
 *    instead of bare esp_bluedroid_init().
 *
 * 3. AVRCP init (esp_avrc_ct_init) MUST precede A2DP init (esp_a2d_source_init).
 *    Deinit is the reverse: A2DP deinit before AVRCP deinit.
 *    Source: ESP-IDF AVRCP API — "AVRC should be initialized before A2DP."
 *
 * 4. ESP_AVRC_BIT_MASK_OP_TEST is the correct enum value for capability
 *    testing (GET does not exist; TEST, SET, CLEAR are the three members).
 *
 * 5. esp_avrc_rn_evt_bit_mask_operation() takes a pointer to the struct
 *    (&param->get_rn_caps_rsp.evt_set), not the .bits field directly.
 *
 * 6. s_abs_vol_supported is reset to false on AVRCP disconnect, not only
 *    on A2DP connect.
 *
 * 7. A2DP source registers a silent PCM data callback and starts the media
 *    stream after connection so speakers do not drop an idle A2DP link.
 *
 * ---- Architecture overview -----------------------------------------------
 *
 * execute() parses the JSON command and posts a bt_media_cmd_t to the
 * internal queue. bt_media_task dequeues and processes commands sequentially.
 * All long-running BT operations run inside that task with watchdog feeding.
 *
 * ---- A2DP + AVRCP relationship -------------------------------------------
 *
 * Most speakers only accept AVRCP after an A2DP connection. Bluedroid
 * automatically connects AVRCP alongside A2DP when both profiles are
 * initialised — no explicit AVRCP connect call is required. The application
 * only calls esp_a2d_source_connect(); the AVRCP connection fires the
 * ESP_AVRC_CT_CONNECTION_STATE_EVT callback independently.
 *
 * CONFIG_BT_ACL_CONNECTIONS must be 2 (not 1) because A2DP and AVRCP each
 * consume one ACL slot even when connecting to the same device.
 *
 * ---- Volume control -------------------------------------------------------
 *
 * On AVRCP connect, query capabilities. If ESP_AVRC_RN_VOLUME_CHANGE is
 * supported, use esp_avrc_ct_send_set_absolute_volume_cmd (0–127).
 * Otherwise update s_volume locally and log only (passthrough fallback).
 *
 * ---- Thread safety --------------------------------------------------------
 *
 * Shared state written by Bluedroid callbacks and read by bt_media_task is
 * declared volatile. No further barrier is needed for single-word primitives
 * on Xtensa at C6. The available flag is set once by the registry at init
 * and never changed at runtime per the C6 design decision.
 */

#include "bt_media_action.h"
#include "action_registry.h"
#include "watchdog.h"
#include "json_utils.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"    /* esp_bluedroid_init_with_cfg / enable / etc. */
#include "esp_a2dp_api.h"   /* A2DP source API                             */
#include "esp_avrc_api.h"   /* AVRCP controller API                        */
#include "esp_gap_bt_api.h" /* esp_bt_gap_set_device_name, set_scan_mode   */

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

static const char *TAG = "bt_media";

/* ======================================================================
 * Compile-time configuration defaults
 * ====================================================================== */

#ifndef CONFIG_BT_MEDIA_TASK_STACK_SIZE
#define CONFIG_BT_MEDIA_TASK_STACK_SIZE 6144
#endif
#ifndef CONFIG_BT_MEDIA_QUEUE_DEPTH
#define CONFIG_BT_MEDIA_QUEUE_DEPTH 4
#endif
#ifndef CONFIG_BT_MEDIA_WDT_INTERVAL_MS
#define CONFIG_BT_MEDIA_WDT_INTERVAL_MS 5000
#endif

/* ======================================================================
 * Module constants
 * ====================================================================== */

#define BT_VOLUME_MIN 0
#define BT_VOLUME_MAX 127
#define BT_VOLUME_DEFAULT 64
#define BT_VOLUME_STEP 8

#define BT_CONNECT_TIMEOUT_MS 12000
#define BT_DISCONNECT_TIMEOUT_MS 5000
#define BT_POLL_INTERVAL_MS 200
#define BT_QUEUE_WAIT_MS 1000

#define BT_TL_NEXT(tl) (((tl) + 1u) & 0x0Fu)
#define BD_ADDR_STR_LEN 18 /* "AA:BB:CC:DD:EE:FF\0" */
#define BT_DEVICE_NAME "RF-Commander"

/* ======================================================================
 * Internal types
 * ====================================================================== */

typedef enum
{
    BT_MEDIA_STATE_DISCONNECTED = 0,
    BT_MEDIA_STATE_CONNECTING = 1,
    BT_MEDIA_STATE_CONNECTED = 2,
    BT_MEDIA_STATE_COMMAND_PENDING = 3,
} bt_media_state_t;

typedef enum
{
    BT_CMD_PLAY = 0,
    BT_CMD_PAUSE = 1,
    BT_CMD_VOL_UP = 2,
    BT_CMD_VOL_DOWN = 3,
    BT_CMD_CONNECT = 4,
    BT_CMD_DISCONNECT = 5,
} bt_cmd_type_t;

typedef struct
{
    bt_cmd_type_t type;
    esp_bd_addr_t target;
    bool has_target;
} bt_media_cmd_t;

/* ======================================================================
 * Module state
 * ====================================================================== */

static volatile bt_media_state_t s_state = BT_MEDIA_STATE_DISCONNECTED;
static volatile bool s_abs_vol_supported = false;
static esp_bd_addr_t s_connected_addr;
static uint8_t s_volume = BT_VOLUME_DEFAULT;
static uint8_t s_tl = 0;

/* Connection handle and MTU captured from ESP_A2D_CONNECTION_STATE_EVT. */
static esp_a2d_conn_hdl_t s_conn_hdl;
static uint16_t s_audio_mtu = 0;

static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_task_handle = NULL;

/* ======================================================================
 * Helpers
 * ====================================================================== */

static void format_bd_addr(const esp_bd_addr_t addr, char *buf, size_t buf_size)
{
    snprintf(buf, buf_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

static bool parse_bd_addr(const char *str, esp_bd_addr_t addr)
{
    unsigned int a[6];
    if (sscanf(str, "%02X:%02X:%02X:%02X:%02X:%02X",
               &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6)
    {
        return false;
    }
    for (int i = 0; i < 6; i++)
    {
        addr[i] = (uint8_t)a[i];
    }
    return true;
}

static int32_t bt_a2d_data_cb(uint8_t *data, int32_t len)
{
    if (data == NULL || len <= 0)
    {
        return 0;
    }
    memset(data, 0, (size_t)len);
    return len;
}

static bool wait_for_state(bt_media_state_t target, uint32_t timeout_ms)
{
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms)
    {
        watchdog_feed_task("bt_media");
        vTaskDelay(pdMS_TO_TICKS(BT_POLL_INTERVAL_MS));
        elapsed += BT_POLL_INTERVAL_MS;
        if (s_state == target)
        {
            return true;
        }
        if (target == BT_MEDIA_STATE_CONNECTED &&
            s_state == BT_MEDIA_STATE_DISCONNECTED)
        {
            return false;
        }
    }
    return (s_state == target);
}

static void send_passthrough(uint8_t key_code, const char *name)
{
    s_tl = (uint8_t)BT_TL_NEXT(s_tl);
    esp_err_t ret = esp_avrc_ct_send_passthrough_cmd(
        s_tl, key_code, ESP_AVRC_PT_CMD_STATE_PRESSED);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "AVRCP passthrough PRESSED (%s) failed: %s",
                 name, esp_err_to_name(ret));
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(100));

    s_tl = (uint8_t)BT_TL_NEXT(s_tl);
    ret = esp_avrc_ct_send_passthrough_cmd(
        s_tl, key_code, ESP_AVRC_PT_CMD_STATE_RELEASED);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "AVRCP passthrough RELEASED (%s) failed: %s",
                 name, esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "AVRCP %s sent", name);
}

static void apply_volume_change(int delta)
{
    int new_vol = (int)s_volume + delta;
    if (new_vol < BT_VOLUME_MIN)
    {
        new_vol = BT_VOLUME_MIN;
    }
    if (new_vol > BT_VOLUME_MAX)
    {
        new_vol = BT_VOLUME_MAX;
    }
    s_volume = (uint8_t)new_vol;

    ESP_LOGD(TAG, "Volume %s → %d/127", delta > 0 ? "up" : "down", s_volume);

    if (s_abs_vol_supported && s_state == BT_MEDIA_STATE_CONNECTED)
    {
        s_tl = (uint8_t)BT_TL_NEXT(s_tl);
        esp_err_t ret = esp_avrc_ct_send_set_absolute_volume_cmd(s_tl, s_volume);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "Absolute volume cmd failed (%s); local-only",
                     esp_err_to_name(ret));
        }
        else
        {
            ESP_LOGI(TAG, "Volume %s: absolute volume → %d",
                     delta > 0 ? "up" : "down", s_volume);
        }
    }
    else
    {
        ESP_LOGI(TAG, "Volume %s: local tracking only (abs_vol=%s, state=%d)",
                 delta > 0 ? "up" : "down",
                 s_abs_vol_supported ? "yes" : "no", (int)s_state);
    }
}

/* ======================================================================
 * A2DP event callback
 * ====================================================================== */

static void bt_a2d_callback(esp_a2d_cb_event_t event,
                            esp_a2d_cb_param_t *param)
{
    switch (event)
    {
    case ESP_A2D_CONNECTION_STATE_EVT:
    {
        char addr[BD_ADDR_STR_LEN];
        format_bd_addr(param->conn_stat.remote_bda, addr, sizeof(addr));

        switch (param->conn_stat.state)
        {
        case ESP_A2D_CONNECTION_STATE_CONNECTING:
            s_state = BT_MEDIA_STATE_CONNECTING;
            ESP_LOGI(TAG, "A2DP connecting to %s", addr);
            break;

        case ESP_A2D_CONNECTION_STATE_CONNECTED:
            memcpy(s_connected_addr, param->conn_stat.remote_bda,
                   sizeof(esp_bd_addr_t));

            s_conn_hdl = param->conn_stat.conn_hdl;
            s_audio_mtu = param->conn_stat.audio_mtu;

            s_state = BT_MEDIA_STATE_CONNECTED;
            s_volume = BT_VOLUME_DEFAULT;
            s_abs_vol_supported = false;
            ESP_LOGI(TAG, "A2DP connected to %s (conn_hdl=%u, mtu=%u)",
                     addr, (unsigned)s_conn_hdl, (unsigned)s_audio_mtu);

            esp_err_t ret = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
            if (ret != ESP_OK)
            {
                ESP_LOGW(TAG, "A2DP media start failed: %s",
                         esp_err_to_name(ret));
            }
            break;

        case ESP_A2D_CONNECTION_STATE_DISCONNECTING:
            ESP_LOGI(TAG, "A2DP disconnecting from %s", addr);
            break;

        case ESP_A2D_CONNECTION_STATE_DISCONNECTED:
            s_state = BT_MEDIA_STATE_DISCONNECTED;
            memset(s_connected_addr, 0, sizeof(s_connected_addr));
            s_conn_hdl = 0;
            s_audio_mtu = 0;
            ESP_LOGI(TAG, "A2DP disconnected from %s", addr);
            break;

        default:
            break;
        }
        break;
    }

    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "A2DP audio state: %d", param->audio_stat.state);
        break;

    case ESP_A2D_AUDIO_CFG_EVT:
        ESP_LOGI(TAG, "A2DP audio cfg (codec type %d)",
                 param->audio_cfg.mcc.type);
        break;

    default:
        break;
    }
}

/* ======================================================================
 * AVRCP Controller event callback
 * ====================================================================== */

static void bt_avrc_ct_callback(esp_avrc_ct_cb_event_t event,
                                esp_avrc_ct_cb_param_t *param)
{
    switch (event)
    {

    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        if (param->conn_stat.connected)
        {
            char addr[BD_ADDR_STR_LEN];
            format_bd_addr(param->conn_stat.remote_bda, addr, sizeof(addr));
            ESP_LOGI(TAG, "AVRCP connected to %s", addr);
            /* Query remote RN capabilities to detect absolute volume support. */
            s_tl = (uint8_t)BT_TL_NEXT(s_tl);
            esp_avrc_ct_send_get_rn_capabilities_cmd(s_tl);
        }
        else
        {
            /* Reset capability flags on disconnect so they are not
             * accidentally used for a future connection to a different device.
             * Confirmed correct behaviour from ESP-IDF example code. */
            s_abs_vol_supported = false;
            ESP_LOGI(TAG, "AVRCP disconnected");
        }
        break;

    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        /* Use ESP_AVRC_BIT_MASK_OP_TEST (confirmed: GET does not exist;
         * the three valid values are TEST, SET, CLEAR).
         * Pass pointer to the struct, not the .bits field directly.
         * Correct field path: param->get_rn_caps_rsp.evt_set (struct),
         * accessed as &param->get_rn_caps_rsp.evt_set for the pointer. */
        s_abs_vol_supported = esp_avrc_rn_evt_bit_mask_operation(
            ESP_AVRC_BIT_MASK_OP_TEST,
            &param->get_rn_caps_rsp.evt_set,
            ESP_AVRC_RN_VOLUME_CHANGE);

        ESP_LOGI(TAG, "AVRCP capabilities (bitmask=0x%04X): abs vol %s",
                 (unsigned)param->get_rn_caps_rsp.evt_set.bits,
                 s_abs_vol_supported ? "SUPPORTED" : "not supported");
        break;

    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
        if (param->psth_rsp.key_state == ESP_AVRC_PT_CMD_STATE_RELEASED)
        {
            ESP_LOGI(TAG, "AVRCP passthrough rsp: key=0x%02X",
                     param->psth_rsp.key_code);
        }
        break;

    case ESP_AVRC_CT_SET_ABSOLUTE_VOLUME_RSP_EVT:
        ESP_LOGD(TAG, "Absolute volume confirmed by remote: %d",
                 param->set_volume_rsp.volume);
        break;

    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        /* event_parameter is a union; check event_id before reading volume. */
        if (param->change_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE)
        {
            s_volume = param->change_ntf.event_parameter.volume;
            ESP_LOGD(TAG, "Remote volume change: %d", s_volume);
        }
        break;

    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        ESP_LOGI(TAG, "AVRCP remote features: 0x%04X",
                 param->rmt_feats.feat_mask);
        break;

    default:
        break;
    }
}

/* ======================================================================
 * Internal command processor
 * ====================================================================== */

static bool do_connect(const esp_bd_addr_t target)
{
    char addr[BD_ADDR_STR_LEN];
    format_bd_addr(target, addr, sizeof(addr));
    ESP_LOGI(TAG, "Connecting to %s", addr);

    s_state = BT_MEDIA_STATE_CONNECTING;
    esp_err_t ret = esp_a2d_source_connect((uint8_t *)target);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_a2d_source_connect failed: %s",
                 esp_err_to_name(ret));
        s_state = BT_MEDIA_STATE_DISCONNECTED;
        return false;
    }

    if (!wait_for_state(BT_MEDIA_STATE_CONNECTED, BT_CONNECT_TIMEOUT_MS))
    {
        ESP_LOGE(TAG, "Connection to %s timed out", addr);
        s_state = BT_MEDIA_STATE_DISCONNECTED;
        return false;
    }

    ESP_LOGI(TAG, "Connected to %s", addr);
    return true;
}

static void process_command(const bt_media_cmd_t *cmd)
{
    char addr[BD_ADDR_STR_LEN] = "N/A";

    if (cmd->has_target &&
        cmd->type != BT_CMD_CONNECT &&
        cmd->type != BT_CMD_DISCONNECT)
    {
        if (s_state == BT_MEDIA_STATE_DISCONNECTED)
        {
            if (!do_connect(cmd->target))
            {
                ESP_LOGE(TAG, "Pre-command connect failed; dropping command");
                return;
            }
        }
    }

    switch (cmd->type)
    {
    case BT_CMD_CONNECT:
        if (s_state == BT_MEDIA_STATE_CONNECTED)
        {
            format_bd_addr(s_connected_addr, addr, sizeof(addr));
            ESP_LOGI(TAG, "Already connected to %s", addr);
            return;
        }
        do_connect(cmd->target);
        break;

    case BT_CMD_DISCONNECT:
        if (s_state == BT_MEDIA_STATE_DISCONNECTED)
        {
            ESP_LOGI(TAG, "Already disconnected");
            return;
        }
        format_bd_addr(s_connected_addr, addr, sizeof(addr));
        ESP_LOGI(TAG, "Disconnecting from %s", addr);
        esp_err_t ret = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "A2DP media suspend before disconnect failed: %s",
                     esp_err_to_name(ret));
        }
        esp_a2d_source_disconnect(s_connected_addr);
        if (!wait_for_state(BT_MEDIA_STATE_DISCONNECTED, BT_DISCONNECT_TIMEOUT_MS))
        {
            ESP_LOGW(TAG, "Disconnect timed out; forcing DISCONNECTED");
            s_state = BT_MEDIA_STATE_DISCONNECTED;
        }
        break;

    case BT_CMD_PLAY:
        if (s_state != BT_MEDIA_STATE_CONNECTED)
        {
            ESP_LOGW(TAG, "Not connected; ignoring PLAY");
            return;
        }
        send_passthrough(ESP_AVRC_PT_CMD_PLAY, "PLAY");
        break;

    case BT_CMD_PAUSE:
        if (s_state != BT_MEDIA_STATE_CONNECTED)
        {
            ESP_LOGW(TAG, "Not connected; ignoring PAUSE");
            return;
        }
        send_passthrough(ESP_AVRC_PT_CMD_PAUSE, "PAUSE");
        break;

    case BT_CMD_VOL_UP:
        apply_volume_change(+BT_VOLUME_STEP);
        break;

    case BT_CMD_VOL_DOWN:
        apply_volume_change(-BT_VOLUME_STEP);
        break;
    }
}

/* ======================================================================
 * Internal Bluetooth task
 * ====================================================================== */

static void bt_media_task(void *arg)
{
    (void)arg;
    bt_media_cmd_t cmd;

    while (1)
    {
        watchdog_feed_task("bt_media");

        BaseType_t got = xQueueReceive(s_cmd_queue, &cmd,
                                       pdMS_TO_TICKS(BT_QUEUE_WAIT_MS));
        if (got != pdTRUE)
        {
            continue;
        }

        process_command(&cmd);
    }

    vTaskDelete(NULL);
}

/* ======================================================================
 * action_module_t interface
 * ====================================================================== */

static esp_err_t bt_media_init(void)
{
    esp_err_t ret;

    /* ---- 1. BLE memory release -------------------------------------------------
     * REMOVED: To keep the option of dynamic BLE/Classic switching later,
     * we do NOT release BLE memory now.  The controller will be initialized
     * in BTDM mode, enabling both radios at the hardware level.
     * Uncomment the mem_release call only if you decide to go permanently
     * Classic‑only and need to save ~60 KB of DRAM. */

    /* ---- 2. BT Controller ---------------------------------------------------- */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Use the exact mode that the controller expects (derived from sdkconfig).
     * Overriding or guessing the mode can cause ESP_ERR_INVALID_ARG. */
    ret = esp_bt_controller_enable(bt_cfg.mode);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "BT controller enable failed (mode=%d): %s",
                 bt_cfg.mode, esp_err_to_name(ret));
        esp_bt_controller_deinit();
        return ret;
    }
    /* ---- 3. Bluedroid stack -----------------------------------------------
     * Use esp_bluedroid_init_with_cfg() + BT_BLUEDROID_INIT_CONFIG_DEFAULT()
     * instead of bare esp_bluedroid_init() per current ESP-IDF API. */
    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ret = esp_bluedroid_init_with_cfg(&bd_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return ret;
    }

    /* All profile initializations must follow esp_bluedroid_enable(). */

    /* ---- 4. GAP setup ----------------------------------------------------- */
    esp_bt_gap_set_device_name(BT_DEVICE_NAME);
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    /* ---- 5. AVRCP Controller — MUST precede A2DP init --------------------
     * ESP-IDF API doc: "AVRC cannot work independently; AVRC should be
     * initialized before A2DP."                                             */
    ret = esp_avrc_ct_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "AVRCP CT init failed: %s", esp_err_to_name(ret));
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return ret;
    }

    ret = esp_avrc_ct_register_callback(bt_avrc_ct_callback);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "AVRCP CT callback register failed: %s",
                 esp_err_to_name(ret));
        /* Non-fatal; continue. */
    }

    /* ---- 6. A2DP silent source data callback ---------------------------- */
    ret = esp_a2d_source_register_data_callback(bt_a2d_data_cb);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "A2DP data callback registration failed (non-fatal): %s",
                 esp_err_to_name(ret));
    }

    /* ---- 7. A2DP Source — initialized AFTER AVRCP ----------------------- */
    ret = esp_a2d_source_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "A2DP source init failed: %s", esp_err_to_name(ret));
        esp_avrc_ct_deinit();
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return ret;
    }

    ret = esp_a2d_register_callback(bt_a2d_callback);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "A2DP callback register failed: %s",
                 esp_err_to_name(ret));
        /* Non-fatal; continue. */
    }

    /* ---- 8. Internal command queue --------------------------------------- */
    s_cmd_queue = xQueueCreate(CONFIG_BT_MEDIA_QUEUE_DEPTH,
                               sizeof(bt_media_cmd_t));
    if (s_cmd_queue == NULL)
    {
        ESP_LOGE(TAG, "Command queue creation failed");
        esp_a2d_source_deinit();
        esp_avrc_ct_deinit();
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return ESP_FAIL;
    }

    /* ---- 9. Internal task ------------------------------------------------ */
    watchdog_register_task("bt_media", CONFIG_BT_MEDIA_WDT_INTERVAL_MS);

    if (xTaskCreate(bt_media_task, "bt_media_task",
                    CONFIG_BT_MEDIA_TASK_STACK_SIZE,
                    NULL, 3, &s_task_handle) != pdPASS)
    {
        ESP_LOGE(TAG, "Task creation failed");
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        esp_a2d_source_deinit();
        esp_avrc_ct_deinit();
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return ESP_FAIL;
    }

    s_state = BT_MEDIA_STATE_DISCONNECTED;
    s_abs_vol_supported = false;
    s_volume = BT_VOLUME_DEFAULT;
    s_conn_hdl = 0;
    s_audio_mtu = 0;
    memset(s_connected_addr, 0, sizeof(s_connected_addr));

    ESP_LOGI(TAG, "bt_media ready (stack=%d queue=%d wdt=%d ms)",
             CONFIG_BT_MEDIA_TASK_STACK_SIZE,
             CONFIG_BT_MEDIA_QUEUE_DEPTH,
             CONFIG_BT_MEDIA_WDT_INTERVAL_MS);
    return ESP_OK;
}

static esp_err_t bt_media_deinit(void)
{
    if (s_state == BT_MEDIA_STATE_CONNECTED ||
        s_state == BT_MEDIA_STATE_CONNECTING)
    {
        char addr[BD_ADDR_STR_LEN];
        format_bd_addr(s_connected_addr, addr, sizeof(addr));
        ESP_LOGI(TAG, "Disconnecting from %s before deinit", addr);
        esp_err_t ret = esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "A2DP media suspend before deinit failed: %s",
                     esp_err_to_name(ret));
        }
        esp_a2d_source_disconnect(s_connected_addr);
        wait_for_state(BT_MEDIA_STATE_DISCONNECTED, BT_DISCONNECT_TIMEOUT_MS);
    }

    if (s_task_handle)
    {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    }
    if (s_cmd_queue)
    {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
    }

    /* Deinit in reverse init order: A2DP before AVRCP. */
    esp_a2d_source_deinit();
    esp_avrc_ct_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    s_state = BT_MEDIA_STATE_DISCONNECTED;
    s_abs_vol_supported = false;
    s_volume = BT_VOLUME_DEFAULT;
    s_conn_hdl = 0;
    s_audio_mtu = 0;

    ESP_LOGI(TAG, "bt_media deinit complete");
    return ESP_OK;
}

static esp_err_t bt_media_execute(const char *json_command)
{
    if (json_command == NULL || json_command[0] != '{')
    {
        return ESP_ERR_INVALID_ARG;
    }

    size_t json_len = strlen(json_command);

    char action[32] = {0};
    if (!json_get_string_field(json_command, json_len, "action",
                               action, sizeof(action)))
    {
        ESP_LOGE(TAG, "execute: missing 'action' field");
        return ESP_ERR_INVALID_ARG;
    }

    char target_str[BD_ADDR_STR_LEN] = {0};
    bool has_target = json_get_string_field(json_command, json_len, "target",
                                            target_str, sizeof(target_str));

    bt_media_cmd_t cmd = {0};
    cmd.has_target = has_target;

    if (has_target)
    {
        if (!parse_bd_addr(target_str, cmd.target))
        {
            ESP_LOGE(TAG, "execute: invalid BD address '%s'", target_str);
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (strcmp(action, "play") == 0)
    {
        cmd.type = BT_CMD_PLAY;
    }
    else if (strcmp(action, "pause") == 0)
    {
        cmd.type = BT_CMD_PAUSE;
    }
    else if (strcmp(action, "vol_up") == 0)
    {
        cmd.type = BT_CMD_VOL_UP;
    }
    else if (strcmp(action, "vol_down") == 0)
    {
        cmd.type = BT_CMD_VOL_DOWN;
    }
    else if (strcmp(action, "connect") == 0)
    {
        if (!has_target)
        {
            ESP_LOGE(TAG, "execute: 'connect' requires 'target'");
            return ESP_ERR_INVALID_ARG;
        }
        cmd.type = BT_CMD_CONNECT;
    }
    else if (strcmp(action, "disconnect") == 0)
    {
        cmd.type = BT_CMD_DISCONNECT;
    }
    else
    {
        ESP_LOGE(TAG, "execute: unknown action '%s'", action);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Queuing: action='%s'%s%s",
             action, has_target ? " target=" : "", has_target ? target_str : "");

    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "execute: queue full — command dropped");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t bt_media_get_status(char *buf, size_t *len)
{
    if (buf == NULL || len == NULL || *len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    static const char *const state_names[] = {
        "DISCONNECTED", "CONNECTING", "CONNECTED", "COMMAND_PENDING"};

    char addr[BD_ADDR_STR_LEN] = "N/A";
    if (s_state == BT_MEDIA_STATE_CONNECTED ||
        s_state == BT_MEDIA_STATE_CONNECTING)
    {
        format_bd_addr(s_connected_addr, addr, sizeof(addr));
    }

    const char *state_str = ((unsigned)s_state < 4u)
                                ? state_names[s_state]
                                : "UNKNOWN";

    int w = snprintf(buf, *len,
                     "{\"module\":\"bt_media\","
                     "\"available\":%s,"
                     "\"state\":\"%s\","
                     "\"connected_to\":\"%s\","
                     "\"volume\":%d,"
                     "\"abs_vol_supported\":%s,"
                     "\"audio_mtu\":%u}",
                     bt_media_action_module.available ? "true" : "false",
                     state_str, addr, (int)s_volume,
                     s_abs_vol_supported ? "true" : "false",
                     (unsigned)s_audio_mtu);

    if (w < 0 || w >= (int)*len)
    {
        return ESP_ERR_INVALID_SIZE;
    }
    *len = (size_t)w;
    return ESP_OK;
}

/* ======================================================================
 * Module struct definition
 * ====================================================================== */

action_module_t bt_media_action_module = {
    .api_version = ACTION_MODULE_API_VERSION,
    .name = "bt_media",
    .available = false,
    .init = bt_media_init,
    .deinit = bt_media_deinit,
    .execute = bt_media_execute,
    .get_status = bt_media_get_status,
};
