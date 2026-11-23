/**
 * @file esp_wifi_manager_ble_nimble.c
 * @brief BLE backend using the NimBLE host stack
 */

#include "sdkconfig.h"

#if defined(CONFIG_WIFI_MGR_ENABLE_BLE) && defined(CONFIG_BT_NIMBLE_ENABLED)

#include "esp_wifi_manager_ble_int.h"
#include "esp_log.h"
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_bt.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "wifi_mgr_ble_nb";

/** Largest command accepted on the Command characteristic. */
#define BLE_CMD_MAX_LEN  512

// =============================================================================
// UUIDs
// =============================================================================

static const ble_uuid16_t s_svc_uuid =
    BLE_UUID16_INIT(WIFI_BLE_SVC_UUID);
static const ble_uuid16_t s_char_status_uuid =
    BLE_UUID16_INIT(WIFI_BLE_CHAR_STATUS_UUID);
static const ble_uuid16_t s_char_command_uuid =
    BLE_UUID16_INIT(WIFI_BLE_CHAR_COMMAND_UUID);
static const ble_uuid16_t s_char_response_uuid =
    BLE_UUID16_INIT(WIFI_BLE_CHAR_RESPONSE_UUID);

// =============================================================================
// State
// =============================================================================

static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_response_val_handle;
static char s_device_name[32];

/** Application host callbacks replaced at init, restored at deinit (service-only mode). */
static ble_hs_sync_fn *s_prev_sync_cb;
static ble_hs_reset_fn *s_prev_reset_cb;

/** true if we initialized the NimBLE stack ourselves; false if it was already running. */
static bool s_ble_stack_owned = false;

// Forward declarations
static void nimble_host_task(void *param);
static void ble_on_sync(void);
static void ble_on_reset(int reason);
static int gap_event_handler(struct ble_gap_event *event, void *arg);
static void start_advertising(void);

// =============================================================================
// GATT Access Callbacks
// =============================================================================

static int gatt_status_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    // Status characteristic: read returns empty for now (notifications push data)
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int gatt_command_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        // Flatten the mbuf chain; the shared layer copies it and processes the
        // command on its own task (the nimble_host stack is too small for that).
        uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
        if (om_len == 0 || om_len > BLE_CMD_MAX_LEN) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        uint8_t *buf = malloc(om_len);
        if (!buf) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        uint16_t len = om_len;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, len, &len);
        if (rc == 0) {
            wifi_mgr_ble_on_command(buf, len);
        }
        free(buf);
        return rc == 0 ? 0 : BLE_ATT_ERR_UNLIKELY;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int gatt_response_access(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

// =============================================================================
// GATT Service Definition
// =============================================================================

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // Status (read + notify)
                .uuid = &s_char_status_uuid.u,
                .access_cb = gatt_status_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                // Command (write)
                .uuid = &s_char_command_uuid.u,
                .access_cb = gatt_command_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                // Response (read + notify)
                .uuid = &s_char_response_uuid.u,
                .access_cb = gatt_response_access,
                .val_handle = &s_response_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }, // Terminator
        },
    },
    { 0 }, // Terminator
};

// =============================================================================
// GAP Event Handler
// =============================================================================

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                s_conn_handle = event->connect.conn_handle;
                ESP_LOGI(TAG, "BLE client connected, conn_handle %d", s_conn_handle);
                wifi_mgr_ble_on_connect();
            } else {
                ESP_LOGE(TAG, "Connection failed, status %d", event->connect.status);
                start_advertising();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ESP_LOGI(TAG, "BLE client disconnected, reason %d", event->disconnect.reason);
            wifi_mgr_ble_on_disconnect();
            start_advertising();
            break;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            ESP_LOGD(TAG, "Advertising complete");
            start_advertising();
            break;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == s_response_val_handle) {
                bool enabled = event->subscribe.cur_notify;
                wifi_mgr_ble_set_response_notify(enabled);
                ESP_LOGI(TAG, "Response notify %s", enabled ? "enabled" : "disabled");
            }
            break;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "MTU changed to %d", event->mtu.value);
            break;

        default:
            break;
    }
    return 0;
}

// =============================================================================
// Advertising
// =============================================================================

static void start_advertising(void)
{
    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = 0x20;
    adv_params.itvl_max = 0x40;

    static const ble_uuid16_t adv_service_uuid = BLE_UUID16_INIT(WIFI_BLE_SVC_UUID);

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_device_name;
    fields.name_len = strlen(s_device_name);
    fields.name_is_complete = 1;
    fields.uuids16 = &adv_service_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set adv fields, rc=%d", rc);
        return;
    }

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                            &adv_params, gap_event_handler, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "Failed to start advertising, rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "BLE advertising started: %s", s_device_name);
    }
}

// =============================================================================
// NimBLE Host Callbacks
// =============================================================================

static void ble_on_sync(void)
{
    // Service-only mode: let the application finish its own setup first
    if (s_prev_sync_cb && s_prev_sync_cb != ble_on_sync) {
        s_prev_sync_cb();
    }

    // Use best available address
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to ensure address, rc=%d", rc);
        return;
    }

    start_advertising();
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason=%d", reason);
}

static void nimble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// =============================================================================
// Backend Interface Implementation
// =============================================================================

esp_err_t wifi_mgr_ble_backend_notify_response(const uint8_t *data, size_t length)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, length);
    if (!om) {
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, s_response_val_handle, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "Notify failed, rc=%d", rc);
        return ESP_FAIL;
    }

    return ESP_OK;
}

uint16_t wifi_mgr_ble_backend_get_mtu(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return 0;
    }
    return ble_att_mtu(s_conn_handle);
}

bool wifi_mgr_ble_backend_is_stack_running(void)
{
    // ble_hs_is_enabled() only turns true once the host task has started, so
    // an application that just called nimble_port_init() would be missed and
    // the stack initialized twice. A non-idle controller means someone owns it.
    return ble_hs_is_enabled() ||
           esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE;
}

esp_err_t wifi_mgr_ble_backend_init(const char *device_name)
{
    strncpy(s_device_name, device_name, sizeof(s_device_name) - 1);
    s_device_name[sizeof(s_device_name) - 1] = '\0';

    if (wifi_mgr_ble_backend_is_stack_running()) {
        // Stack already running — service-only mode
        s_ble_stack_owned = false;
        ESP_LOGI(TAG, "NimBLE stack already running, registering service only");
    } else {
        // Full stack init
        s_ble_stack_owned = true;

        esp_err_t ret = nimble_port_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    // Configure host callbacks (remember the application's to restore them)
    s_prev_sync_cb = ble_hs_cfg.sync_cb;
    s_prev_reset_cb = ble_hs_cfg.reset_cb;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    // Set preferred MTU
    int mtu_rc = ble_att_set_preferred_mtu(517);
    if (mtu_rc != 0) {
        ESP_LOGW(TAG, "Failed to set preferred MTU, rc=%d", mtu_rc);
    }

    // Set device name
    ble_svc_gap_device_name_set(s_device_name);

    // Initialize GAP and GATT services
    ble_svc_gap_init();
    ble_svc_gatt_init();

    // Register custom GATT service
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed, rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed, rc=%d", rc);
        goto fail;
    }

    // Start NimBLE host task only if we own the stack
    if (s_ble_stack_owned) {
        nimble_port_freertos_init(nimble_host_task);
    } else {
        // Stack is already running — trigger advertising via on_sync path
        start_advertising();
    }

    return ESP_OK;

fail:
    ble_hs_cfg.sync_cb = s_prev_sync_cb;
    ble_hs_cfg.reset_cb = s_prev_reset_cb;
    if (s_ble_stack_owned) {
        nimble_port_deinit();
        s_ble_stack_owned = false;
    }
    return ESP_FAIL;
}

esp_err_t wifi_mgr_ble_backend_deinit(void)
{
    // Disconnect active client
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        // Wait for async disconnect to complete (gap_event_handler sets
        // s_conn_handle = BLE_HS_CONN_HANDLE_NONE on disconnect event)
        for (int i = 0; i < 50 && s_conn_handle != BLE_HS_CONN_HANDLE_NONE; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // Stop advertising
    ble_gap_adv_stop();

    // Reset GATT services so our service is unregistered.
    // ble_gatts_reset() removes all services; re-register the mandatory ones
    // and commit with ble_gatts_start() so the server is in a stable state.
    int rc = ble_gatts_reset();
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_gatts_reset failed, rc=%d", rc);
    }
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_start();

    // Full stack teardown only if we own it
    if (s_ble_stack_owned) {
        int rc = nimble_port_stop();
        if (rc != 0) {
            ESP_LOGE(TAG, "nimble_port_stop failed, rc=%d", rc);
        }

        rc = nimble_port_deinit();
        if (rc != 0) {
            ESP_LOGE(TAG, "nimble_port_deinit failed, rc=%d", rc);
        }
    }

    // Hand the host callbacks back to the application
    ble_hs_cfg.sync_cb = s_prev_sync_cb;
    ble_hs_cfg.reset_cb = s_prev_reset_cb;

    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_ble_stack_owned = false;

    return ESP_OK;
}

#endif // CONFIG_WIFI_MGR_ENABLE_BLE && CONFIG_BT_NIMBLE_ENABLED
