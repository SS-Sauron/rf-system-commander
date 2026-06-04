/* common_components/bt_media_action/bt_media_action.c
 *
 * C6 L2CAP refactor: Bluetooth media control over raw Classic L2CAP.
 *
 * This module keeps the Commander action API and JSON commands unchanged,
 * but sends AVRCP passthrough commands directly over AVCTP on L2CAP PSM 23.
 * It does not create an A2DP stream and does not use the high-level AVRCP
 * controller APIs.
 */

#include "bt_media_action.h"
#include "action_registry.h"
#include "watchdog.h"
#include "json_utils.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_l2cap_bt_api.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

#define BT_AVRCP_L2CAP_PSM 23

#define BT_CONNECT_TIMEOUT_MS 12000
#define BT_DISCONNECT_TIMEOUT_MS 5000
#define BT_L2CAP_INIT_TIMEOUT_MS 5000
#define BT_POLL_INTERVAL_MS 200
#define BT_QUEUE_WAIT_MS 1000
#define BT_PASSTHROUGH_HOLD_MS 100

#define BT_TL_NEXT(tl) (((tl) + 1u) & 0x0Fu)
#define BD_ADDR_STR_LEN 18 /* "AA:BB:CC:DD:EE:FF\0" */
#define BT_DEVICE_NAME "RF-Commander"

#define AVCTP_OP_VOL_UP 0x41
#define AVCTP_OP_VOL_DOWN 0x42
#define AVCTP_OP_PLAY 0x44
#define AVCTP_OP_PAUSE 0x46

#define AVCTP_STATE_PRESS 0x00
#define AVCTP_STATE_RELEASE 0x80

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
static esp_bd_addr_t s_connected_addr;
static uint8_t s_tl = 0;

/* C6 L2CAP refactor: raw L2CAP synchronization/state. */
static SemaphoreHandle_t g_l2cap_sem = NULL;
static SemaphoreHandle_t g_acl_disc_sem = NULL;
static int g_l2cap_fd = -1;
static volatile bool s_l2cap_inited = false;
static volatile bool s_l2cap_vfs_registered = false;
static volatile bool s_l2cap_open_failed = false;
static volatile esp_bt_l2cap_status_t s_l2cap_last_status = ESP_BT_L2CAP_SUCCESS;
static volatile bool s_watchdog_registered = false;

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

static const char *state_name(bt_media_state_t state)
{
    switch (state)
    {
    case BT_MEDIA_STATE_DISCONNECTED:
        return "disconnected";
    case BT_MEDIA_STATE_CONNECTING:
        return "connecting";
    case BT_MEDIA_STATE_CONNECTED:
        return "connected";
    case BT_MEDIA_STATE_COMMAND_PENDING:
        return "command_pending";
    default:
        return "unknown";
    }
}

static void maybe_feed_watchdog(void)
{
    if (s_watchdog_registered)
    {
        (void)watchdog_feed_task("bt_media");
    }
}

static void drain_semaphore(SemaphoreHandle_t sem)
{
    if (sem == NULL)
    {
        return;
    }
    while (xSemaphoreTake(sem, 0) == pdTRUE)
    {
    }
}

static bool wait_for_flag(volatile bool *flag, bool target, uint32_t timeout_ms)
{
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms)
    {
        if (*flag == target)
        {
            return true;
        }

        maybe_feed_watchdog();
        (void)xSemaphoreTake(g_l2cap_sem, pdMS_TO_TICKS(BT_POLL_INTERVAL_MS));
        elapsed += BT_POLL_INTERVAL_MS;
    }
    return (*flag == target);
}

static bool wait_for_l2cap_open(uint32_t timeout_ms)
{
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms)
    {
        if (g_l2cap_fd >= 0)
        {
            return true;
        }
        if (s_l2cap_open_failed)
        {
            return false;
        }

        maybe_feed_watchdog();
        (void)xSemaphoreTake(g_l2cap_sem, pdMS_TO_TICKS(BT_POLL_INTERVAL_MS));
        elapsed += BT_POLL_INTERVAL_MS;
    }
    return (g_l2cap_fd >= 0);
}

static bool wait_for_acl_disconnect(uint32_t timeout_ms)
{
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms)
    {
        maybe_feed_watchdog();
        if (xSemaphoreTake(g_acl_disc_sem, pdMS_TO_TICKS(BT_POLL_INTERVAL_MS)) == pdTRUE)
        {
            return true;
        }
        elapsed += BT_POLL_INTERVAL_MS;
    }
    return false;
}

static void reset_connection_state(void)
{
    g_l2cap_fd = -1;
    s_state = BT_MEDIA_STATE_DISCONNECTED;
    memset(s_connected_addr, 0, sizeof(s_connected_addr));
}

/* ======================================================================
 * GAP and L2CAP callbacks
 * ====================================================================== */

static void bt_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (event == ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT)
    {
        char addr[BD_ADDR_STR_LEN];
        format_bd_addr(param->acl_disconn_cmpl_stat.bda, addr, sizeof(addr));
        ESP_LOGI(TAG, "ACL disconnected from %s (reason=0x%02X)",
                 addr, (unsigned)param->acl_disconn_cmpl_stat.reason);
        reset_connection_state();
        if (g_acl_disc_sem)
        {
            (void)xSemaphoreGive(g_acl_disc_sem);
        }
    }
}

static void bt_l2cap_callback(esp_bt_l2cap_cb_event_t event,
                              esp_bt_l2cap_cb_param_t *param)
{
    char addr[BD_ADDR_STR_LEN];

    switch (event)
    {
    case ESP_BT_L2CAP_INIT_EVT:
        s_l2cap_last_status = param->init.status;
        s_l2cap_inited = (param->init.status == ESP_BT_L2CAP_SUCCESS);
        ESP_LOGI(TAG, "L2CAP init status=%d", param->init.status);
        if (g_l2cap_sem)
        {
            (void)xSemaphoreGive(g_l2cap_sem);
        }
        break;

    case ESP_BT_L2CAP_VFS_REGISTER_EVT:
        s_l2cap_last_status = param->vfs_register.status;
        s_l2cap_vfs_registered =
            (param->vfs_register.status == ESP_BT_L2CAP_SUCCESS);
        ESP_LOGI(TAG, "L2CAP VFS register status=%d", param->vfs_register.status);
        if (g_l2cap_sem)
        {
            (void)xSemaphoreGive(g_l2cap_sem);
        }
        break;

    case ESP_BT_L2CAP_OPEN_EVT:
        s_l2cap_last_status = param->open.status;
        if (param->open.status == ESP_BT_L2CAP_SUCCESS)
        {
            format_bd_addr(param->open.rem_bda, addr, sizeof(addr));
            g_l2cap_fd = param->open.fd;
            s_l2cap_open_failed = false;
            memcpy(s_connected_addr, param->open.rem_bda, sizeof(esp_bd_addr_t));
            s_state = BT_MEDIA_STATE_CONNECTED;
            ESP_LOGI(TAG, "L2CAP open: fd=%d tx_mtu=%ld remote=%s",
                     param->open.fd, (long)param->open.tx_mtu, addr);
        }
        else
        {
            s_l2cap_open_failed = true;
            ESP_LOGE(TAG, "L2CAP open failed: status=%d", param->open.status);
        }
        if (g_l2cap_sem)
        {
            (void)xSemaphoreGive(g_l2cap_sem);
        }
        break;

    case ESP_BT_L2CAP_CLOSE_EVT:
        s_l2cap_last_status = param->close.status;
        ESP_LOGI(TAG, "L2CAP close status=%d async=%d",
                 param->close.status, param->close.async ? 1 : 0);
        reset_connection_state();
        if (g_l2cap_sem)
        {
            (void)xSemaphoreGive(g_l2cap_sem);
        }
        break;

    case ESP_BT_L2CAP_UNINIT_EVT:
        s_l2cap_last_status = param->uninit.status;
        s_l2cap_inited = false;
        s_l2cap_vfs_registered = false;
        s_l2cap_open_failed = false;
        reset_connection_state();
        ESP_LOGI(TAG, "L2CAP uninit status=%d", param->uninit.status);
        if (g_l2cap_sem)
        {
            (void)xSemaphoreGive(g_l2cap_sem);
        }
        break;

    case ESP_BT_L2CAP_CL_INIT_EVT:
        s_l2cap_last_status = param->cl_init.status;
        ESP_LOGI(TAG, "L2CAP client init status=%d", param->cl_init.status);
        if (param->cl_init.status != ESP_BT_L2CAP_SUCCESS)
        {
            s_l2cap_open_failed = true;
            if (g_l2cap_sem)
            {
                (void)xSemaphoreGive(g_l2cap_sem);
            }
        }
        break;

    default:
        break;
    }
}

/* ======================================================================
 * Raw L2CAP helpers
 * ====================================================================== */

static bool ensure_l2cap_ready(void)
{
    esp_err_t ret;

    if (!s_l2cap_inited)
    {
        s_l2cap_last_status = ESP_BT_L2CAP_SUCCESS;
        s_l2cap_open_failed = false;
        drain_semaphore(g_l2cap_sem);

        ret = esp_bt_l2cap_init();
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_bt_l2cap_init failed: %s", esp_err_to_name(ret));
            return false;
        }

        if (!wait_for_flag(&s_l2cap_inited, true, BT_L2CAP_INIT_TIMEOUT_MS))
        {
            ESP_LOGE(TAG, "L2CAP init timed out/status=%d", s_l2cap_last_status);
            return false;
        }
    }

    if (!s_l2cap_vfs_registered)
    {
        s_l2cap_last_status = ESP_BT_L2CAP_SUCCESS;
        drain_semaphore(g_l2cap_sem);

        ret = esp_bt_l2cap_vfs_register();
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_bt_l2cap_vfs_register failed: %s",
                     esp_err_to_name(ret));
            return false;
        }

        if (!wait_for_flag(&s_l2cap_vfs_registered, true, BT_L2CAP_INIT_TIMEOUT_MS))
        {
            ESP_LOGE(TAG, "L2CAP VFS register timed out/status=%d",
                     s_l2cap_last_status);
            return false;
        }
    }

    return true;
}

static bool write_avctp_frame(uint8_t tl, uint8_t op_data, uint8_t state)
{
    uint8_t pkt[8];

    pkt[0] = (uint8_t)((tl << 4) & 0xF0); /* transaction label */
    pkt[1] = 0x11;                        /* AVCTP header: command type, 1 packet */
    pkt[2] = 0x0E;                        /* AVRCP opcode: Passthrough */
    pkt[3] = 0x00;                        /* subunit type and id */
    pkt[4] = 0x48;                        /* operation id */
    pkt[5] = 0x7C;                        /* operation data length: 1 byte */
    pkt[6] = op_data;                     /* key code */
    pkt[7] = state;                       /* 0x00 press, 0x80 release */

    if (g_l2cap_fd < 0)
    {
        ESP_LOGW(TAG, "L2CAP fd is not open");
        return false;
    }

    ssize_t written = write(g_l2cap_fd, pkt, sizeof(pkt));
    if (written != (ssize_t)sizeof(pkt))
    {
        ESP_LOGE(TAG, "L2CAP write failed: wrote=%ld errno=%d",
                 (long)written, errno);
        return false;
    }

    return true;
}

static bool send_passthrough(uint8_t op_data, const char *name)
{
    if (s_state != BT_MEDIA_STATE_CONNECTED || g_l2cap_fd < 0)
    {
        ESP_LOGW(TAG, "Not connected; ignoring %s", name);
        return false;
    }

    uint8_t press_tl = s_tl;
    if (!write_avctp_frame(press_tl, op_data, AVCTP_STATE_PRESS))
    {
        return false;
    }

    ESP_LOGI(TAG, "AVCTP %s press sent (tl=%u op=0x%02X)",
             name, (unsigned)press_tl, (unsigned)op_data);

    vTaskDelay(pdMS_TO_TICKS(BT_PASSTHROUGH_HOLD_MS));

    uint8_t release_tl = (uint8_t)BT_TL_NEXT(press_tl);
    if (!write_avctp_frame(release_tl, op_data, AVCTP_STATE_RELEASE))
    {
        s_tl = (uint8_t)BT_TL_NEXT(release_tl);
        return false;
    }

    ESP_LOGI(TAG, "AVCTP %s release sent (tl=%u op=0x%02X)",
             name, (unsigned)release_tl, (unsigned)op_data);

    s_tl = (uint8_t)BT_TL_NEXT(release_tl);
    return true;
}

static bool do_disconnect(void)
{
    bool had_connection = (g_l2cap_fd >= 0 ||
                           s_state == BT_MEDIA_STATE_CONNECTED ||
                           s_state == BT_MEDIA_STATE_CONNECTING);

    if (!s_l2cap_inited)
    {
        reset_connection_state();
        return true;
    }

    drain_semaphore(g_acl_disc_sem);
    drain_semaphore(g_l2cap_sem);

    esp_err_t ret = esp_bt_l2cap_deinit();
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "esp_bt_l2cap_deinit failed: %s", esp_err_to_name(ret));
        reset_connection_state();
        s_l2cap_inited = false;
        s_l2cap_vfs_registered = false;
        return false;
    }

    if (had_connection && !wait_for_acl_disconnect(BT_DISCONNECT_TIMEOUT_MS))
    {
        ESP_LOGW(TAG, "ACL disconnect wait timed out");
    }

    if (!wait_for_flag(&s_l2cap_inited, false, BT_DISCONNECT_TIMEOUT_MS))
    {
        ESP_LOGW(TAG, "L2CAP uninit wait timed out");
    }

    reset_connection_state();
    s_l2cap_inited = false;
    s_l2cap_vfs_registered = false;
    ESP_LOGI(TAG, "Disconnected");
    return true;
}

static bool do_connect(const esp_bd_addr_t target)
{
    char addr[BD_ADDR_STR_LEN];
    format_bd_addr(target, addr, sizeof(addr));
    ESP_LOGI(TAG, "Connecting raw L2CAP AVRCP control to %s", addr);

    if (!ensure_l2cap_ready())
    {
        reset_connection_state();
        return false;
    }

    s_state = BT_MEDIA_STATE_CONNECTING;
    memcpy(s_connected_addr, target, sizeof(esp_bd_addr_t));
    g_l2cap_fd = -1;
    s_l2cap_open_failed = false;
    drain_semaphore(g_l2cap_sem);

    esp_err_t ret = esp_bt_l2cap_connect(
        ESP_BT_L2CAP_SEC_NONE, BT_AVRCP_L2CAP_PSM, (uint8_t *)target);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_bt_l2cap_connect failed: %s", esp_err_to_name(ret));
        reset_connection_state();
        return false;
    }

    if (!wait_for_l2cap_open(BT_CONNECT_TIMEOUT_MS))
    {
        ESP_LOGE(TAG, "L2CAP connection to %s failed/timed out (status=%d)",
                 addr, s_l2cap_last_status);
        reset_connection_state();
        return false;
    }

    s_state = BT_MEDIA_STATE_CONNECTED;
    memcpy(s_connected_addr, target, sizeof(esp_bd_addr_t));
    ESP_LOGI(TAG, "Connected to %s over L2CAP PSM %u (fd=%d)",
             addr, (unsigned)BT_AVRCP_L2CAP_PSM, g_l2cap_fd);
    return true;
}

/* ======================================================================
 * Internal command processor
 * ====================================================================== */

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
        (void)do_connect(cmd->target);
        break;

    case BT_CMD_DISCONNECT:
        if (s_state == BT_MEDIA_STATE_DISCONNECTED && !s_l2cap_inited)
        {
            ESP_LOGI(TAG, "Already disconnected");
            return;
        }
        if (s_state == BT_MEDIA_STATE_CONNECTED ||
            s_state == BT_MEDIA_STATE_CONNECTING)
        {
            format_bd_addr(s_connected_addr, addr, sizeof(addr));
            ESP_LOGI(TAG, "Disconnecting from %s", addr);
        }
        (void)do_disconnect();
        break;

    case BT_CMD_PLAY:
        (void)send_passthrough(AVCTP_OP_PLAY, "PLAY");
        break;

    case BT_CMD_PAUSE:
        (void)send_passthrough(AVCTP_OP_PAUSE, "PAUSE");
        break;

    case BT_CMD_VOL_UP:
        (void)send_passthrough(AVCTP_OP_VOL_UP, "VOL_UP");
        break;

    case BT_CMD_VOL_DOWN:
        (void)send_passthrough(AVCTP_OP_VOL_DOWN, "VOL_DOWN");
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

static void delete_sync_objects(void)
{
    if (g_l2cap_sem)
    {
        vSemaphoreDelete(g_l2cap_sem);
        g_l2cap_sem = NULL;
    }
    if (g_acl_disc_sem)
    {
        vSemaphoreDelete(g_acl_disc_sem);
        g_acl_disc_sem = NULL;
    }
}

static void deinit_bt_stack(void)
{
    if (s_l2cap_inited)
    {
        (void)do_disconnect();
    }
    (void)esp_bluedroid_disable();
    (void)esp_bluedroid_deinit();
    (void)esp_bt_controller_disable();
    (void)esp_bt_controller_deinit();
}

static esp_err_t bt_media_init(void)
{
    esp_err_t ret;

    g_l2cap_sem = xSemaphoreCreateBinary();
    g_acl_disc_sem = xSemaphoreCreateBinary();
    if (g_l2cap_sem == NULL || g_acl_disc_sem == NULL)
    {
        ESP_LOGE(TAG, "Semaphore creation failed");
        delete_sync_objects();
        return ESP_ERR_NO_MEM;
    }

    /* C6 L2CAP refactor: keep dual-mode controller memory available. */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
        delete_sync_objects();
        return ret;
    }

    ret = esp_bt_controller_enable(bt_cfg.mode);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "BT controller enable failed (mode=%d): %s",
                 bt_cfg.mode, esp_err_to_name(ret));
        esp_bt_controller_deinit();
        delete_sync_objects();
        return ret;
    }

    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ret = esp_bluedroid_init_with_cfg(&bd_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        delete_sync_objects();
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        delete_sync_objects();
        return ret;
    }

    ret = esp_bt_gap_register_callback(bt_gap_callback);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "GAP callback register failed: %s", esp_err_to_name(ret));
        deinit_bt_stack();
        delete_sync_objects();
        return ret;
    }

    esp_bt_gap_set_device_name(BT_DEVICE_NAME);
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

    ret = esp_bt_l2cap_register_callback(bt_l2cap_callback);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "L2CAP callback register failed: %s", esp_err_to_name(ret));
        deinit_bt_stack();
        delete_sync_objects();
        return ret;
    }

    if (!ensure_l2cap_ready())
    {
        ESP_LOGE(TAG, "L2CAP setup failed");
        deinit_bt_stack();
        delete_sync_objects();
        return ESP_FAIL;
    }

    s_cmd_queue = xQueueCreate(CONFIG_BT_MEDIA_QUEUE_DEPTH, sizeof(bt_media_cmd_t));
    if (s_cmd_queue == NULL)
    {
        ESP_LOGE(TAG, "Command queue creation failed");
        deinit_bt_stack();
        delete_sync_objects();
        return ESP_ERR_NO_MEM;
    }

    ret = watchdog_register_task("bt_media", CONFIG_BT_MEDIA_WDT_INTERVAL_MS);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Watchdog registration failed: %s", esp_err_to_name(ret));
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        deinit_bt_stack();
        delete_sync_objects();
        return ret;
    }
    s_watchdog_registered = true;

    if (xTaskCreate(bt_media_task, "bt_media_task",
                    CONFIG_BT_MEDIA_TASK_STACK_SIZE,
                    NULL, 3, &s_task_handle) != pdPASS)
    {
        ESP_LOGE(TAG, "Task creation failed");
        s_watchdog_registered = false;
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        deinit_bt_stack();
        delete_sync_objects();
        return ESP_FAIL;
    }

    reset_connection_state();
    s_tl = 0;

    ESP_LOGI(TAG, "bt_media raw L2CAP ready (stack=%d queue=%d wdt=%d ms)",
             CONFIG_BT_MEDIA_TASK_STACK_SIZE,
             CONFIG_BT_MEDIA_QUEUE_DEPTH,
             CONFIG_BT_MEDIA_WDT_INTERVAL_MS);
    return ESP_OK;
}

static esp_err_t bt_media_deinit(void)
{
    if (s_state == BT_MEDIA_STATE_CONNECTED ||
        s_state == BT_MEDIA_STATE_CONNECTING ||
        s_l2cap_inited)
    {
        (void)do_disconnect();
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

    s_watchdog_registered = false;
    deinit_bt_stack();
    delete_sync_objects();
    reset_connection_state();
    s_tl = 0;

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
        ESP_LOGW(TAG, "execute: queue full - command dropped");
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

    char addr[BD_ADDR_STR_LEN] = "N/A";
    if (s_state == BT_MEDIA_STATE_CONNECTED ||
        s_state == BT_MEDIA_STATE_CONNECTING)
    {
        format_bd_addr(s_connected_addr, addr, sizeof(addr));
    }

    int w = snprintf(buf, *len,
                     "{\"module\":\"bt_media\","
                     "\"available\":%s,"
                     "\"state\":\"%s\","
                     "\"connected_to\":\"%s\"}",
                     bt_media_action_module.available ? "true" : "false",
                     state_name(s_state), addr);

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
