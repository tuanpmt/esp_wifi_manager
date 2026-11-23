/**
 * @file esp_wifi_manager_ble_bluedroid.c
 * @brief BLE backend using the Bluedroid host stack
 */

#include "sdkconfig.h"

#if defined(CONFIG_WIFI_MGR_ENABLE_BLE) && defined(CONFIG_BT_BLUEDROID_ENABLED)

#include "esp_wifi_manager_ble_int.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"

static const char *TAG = "wifi_mgr_ble_bd";

// =============================================================================
// GATT Table
// =============================================================================

#define WIFI_SVC_INST_ID        0
#define PROFILE_APP_IDX         0

enum {
    IDX_SVC,
    IDX_CHAR_STATUS,
    IDX_CHAR_STATUS_VAL,
    IDX_CHAR_STATUS_CCC,
    IDX_CHAR_COMMAND,
    IDX_CHAR_COMMAND_VAL,
    IDX_CHAR_RESPONSE,
    IDX_CHAR_RESPONSE_VAL,
    IDX_CHAR_RESPONSE_CCC,
    WIFI_IDX_NB,
};

static uint16_t wifi_handle_table[WIFI_IDX_NB];

static const uint16_t primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t character_declaration_uuid = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t character_client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

static const uint8_t char_prop_read_notify = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t char_prop_write = ESP_GATT_CHAR_PROP_BIT_WRITE;

static const uint16_t wifi_svc_uuid = WIFI_BLE_SVC_UUID;
static const uint16_t char_status_uuid = WIFI_BLE_CHAR_STATUS_UUID;
static const uint16_t char_command_uuid = WIFI_BLE_CHAR_COMMAND_UUID;
static const uint16_t char_response_uuid = WIFI_BLE_CHAR_RESPONSE_UUID;

static uint8_t status_ccc[2] = {0x00, 0x00};
static uint8_t response_ccc[2] = {0x00, 0x00};

static const esp_gatts_attr_db_t wifi_gatt_db[WIFI_IDX_NB] = {
    // Service Declaration
    [IDX_SVC] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid, ESP_GATT_PERM_READ,
         sizeof(uint16_t), sizeof(wifi_svc_uuid), (uint8_t *)&wifi_svc_uuid}
    },

    // Status Characteristic Declaration
    [IDX_CHAR_STATUS] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
         1, 1, (uint8_t *)&char_prop_read_notify}
    },
    // Status Characteristic Value
    [IDX_CHAR_STATUS_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&char_status_uuid, ESP_GATT_PERM_READ,
         512, 0, NULL}
    },
    // Status CCCD
    [IDX_CHAR_STATUS_CCC] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid,
         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
         sizeof(status_ccc), sizeof(status_ccc), status_ccc}
    },

    // Command Characteristic Declaration
    [IDX_CHAR_COMMAND] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
         1, 1, (uint8_t *)&char_prop_write}
    },
    // Command Characteristic Value
    [IDX_CHAR_COMMAND_VAL] = {
        {ESP_GATT_RSP_BY_APP},
        {ESP_UUID_LEN_16, (uint8_t *)&char_command_uuid, ESP_GATT_PERM_WRITE,
         512, 0, NULL}
    },

    // Response Characteristic Declaration
    [IDX_CHAR_RESPONSE] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
         1, 1, (uint8_t *)&char_prop_read_notify}
    },
    // Response Characteristic Value
    [IDX_CHAR_RESPONSE_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&char_response_uuid, ESP_GATT_PERM_READ,
         512, 0, NULL}
    },
    // Response CCCD
    [IDX_CHAR_RESPONSE_CCC] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid,
         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
         sizeof(response_ccc), sizeof(response_ccc), response_ccc}
    },
};

// =============================================================================
// Profile State
// =============================================================================

static struct {
    esp_gatt_if_t gatts_if;
    uint16_t conn_id;
    uint16_t mtu;
    bool connected;
    esp_bd_addr_t remote_bda;
} s_profile = {
    .gatts_if = ESP_GATT_IF_NONE,
    .mtu = 23,
    .connected = false,
};

static char s_device_name[32];

/** true if we initialized the BLE stack ourselves; false if it was already running. */
static bool s_ble_stack_owned = false;

// =============================================================================
// GAP Event Handler
// =============================================================================

static uint8_t adv_service_uuid[] = {
    0xE0, 0xFF,  // WIFI_BLE_SVC_UUID (0xFFE0) in little-endian
};

static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = false,
    .min_interval = 0x0006,
    .max_interval = 0x0010,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(adv_service_uuid),
    .p_service_uuid = adv_service_uuid,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            esp_ble_gap_start_advertising(&adv_params);
            break;

        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "Advertising start failed");
            } else {
                ESP_LOGI(TAG, "BLE advertising started: %s", s_device_name);
            }
            break;

        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "Advertising stop failed");
            }
            break;

        default:
            break;
    }
}

// =============================================================================
// GATTS Event Handler
// =============================================================================

// =============================================================================
// Prepared (long) writes
// =============================================================================

#define PREP_WRITE_MAX_LEN 512

static uint8_t *s_prep_buf;
static size_t s_prep_len;

static void handle_prepare_write(esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    esp_gatt_status_t status = ESP_GATT_OK;

    if (param->write.handle != wifi_handle_table[IDX_CHAR_COMMAND_VAL]) {
        status = ESP_GATT_REQ_NOT_SUPPORTED;
    } else if (param->write.offset + param->write.len > PREP_WRITE_MAX_LEN) {
        status = ESP_GATT_INVALID_ATTR_LEN;
    } else if (param->write.offset != s_prep_len) {
        status = ESP_GATT_INVALID_OFFSET;
    } else {
        if (!s_prep_buf) {
            s_prep_buf = malloc(PREP_WRITE_MAX_LEN);
            if (!s_prep_buf) status = ESP_GATT_NO_RESOURCES;
        }
        if (status == ESP_GATT_OK) {
            memcpy(s_prep_buf + param->write.offset, param->write.value, param->write.len);
            s_prep_len = param->write.offset + param->write.len;
        }
    }

    if (status != ESP_GATT_OK) {
        free(s_prep_buf);
        s_prep_buf = NULL;
        s_prep_len = 0;
    }

    if (param->write.need_rsp) {
        // Prepare write responses must echo the received value
        esp_gatt_rsp_t *rsp = calloc(1, sizeof(esp_gatt_rsp_t));
        if (rsp) {
            rsp->attr_value.handle = param->write.handle;
            rsp->attr_value.offset = param->write.offset;
            rsp->attr_value.len = param->write.len;
            rsp->attr_value.auth_req = ESP_GATT_AUTH_REQ_NONE;
            memcpy(rsp->attr_value.value, param->write.value, param->write.len);
        }
        esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id,
                                    status, rsp);
        free(rsp);
    }
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                 esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
        case ESP_GATTS_REG_EVT:
            if (param->reg.status == ESP_GATT_OK) {
                s_profile.gatts_if = gatts_if;
                esp_ble_gap_set_device_name(s_device_name);
                esp_ble_gap_config_adv_data(&adv_data);
                esp_ble_gatts_create_attr_tab(wifi_gatt_db, gatts_if, WIFI_IDX_NB, WIFI_SVC_INST_ID);
            } else {
                ESP_LOGE(TAG, "GATT register failed, status %d", param->reg.status);
            }
            break;

        case ESP_GATTS_CREAT_ATTR_TAB_EVT:
            if (param->add_attr_tab.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "Create attr table failed, status %d", param->add_attr_tab.status);
            } else if (param->add_attr_tab.num_handle != WIFI_IDX_NB) {
                ESP_LOGE(TAG, "Create attr table abnormally, num_handle (%d) != WIFI_IDX_NB (%d)",
                         param->add_attr_tab.num_handle, WIFI_IDX_NB);
            } else {
                memcpy(wifi_handle_table, param->add_attr_tab.handles, sizeof(wifi_handle_table));
                esp_ble_gatts_start_service(wifi_handle_table[IDX_SVC]);
                ESP_LOGI(TAG, "GATT service started");
            }
            break;

        case ESP_GATTS_CONNECT_EVT:
            s_profile.conn_id = param->connect.conn_id;
            s_profile.connected = true;
            memcpy(s_profile.remote_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
            ESP_LOGI(TAG, "BLE client connected, conn_id %d", param->connect.conn_id);

            wifi_mgr_ble_on_connect();

            // Update connection params for better throughput
            esp_ble_conn_update_params_t conn_params = {0};
            memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
            conn_params.latency = 0;
            conn_params.max_int = 0x20;
            conn_params.min_int = 0x10;
            conn_params.timeout = 400;
            esp_ble_gap_update_conn_params(&conn_params);
            break;

        case ESP_GATTS_DISCONNECT_EVT:
            s_profile.connected = false;
            s_profile.mtu = 23;
            ESP_LOGI(TAG, "BLE client disconnected");

            wifi_mgr_ble_on_disconnect();
            free(s_prep_buf);
            s_prep_buf = NULL;
            s_prep_len = 0;

            // Restart advertising
            esp_ble_gap_start_advertising(&adv_params);
            break;

        case ESP_GATTS_WRITE_EVT:
            if (!param->write.is_prep) {
                // Handle CCCD writes
                if (param->write.handle == wifi_handle_table[IDX_CHAR_RESPONSE_CCC]) {
                    if (param->write.len == 2) {
                        uint16_t descr_value = param->write.value[1] << 8 | param->write.value[0];
                        bool enabled = (descr_value == 0x0001);
                        wifi_mgr_ble_set_response_notify(enabled);
                        ESP_LOGI(TAG, "Response notify %s", enabled ? "enabled" : "disabled");
                    }
                } else if (param->write.handle == wifi_handle_table[IDX_CHAR_STATUS_CCC]) {
                    if (param->write.len == 2) {
                        uint16_t descr_value = param->write.value[1] << 8 | param->write.value[0];
                        ESP_LOGI(TAG, "Status notify %s",
                                 (descr_value == 0x0001) ? "enabled" : "disabled");
                    }
                }
                // Handle command write
                else if (param->write.handle == wifi_handle_table[IDX_CHAR_COMMAND_VAL]) {
                    wifi_mgr_ble_on_command(param->write.value, param->write.len);
                }

                // Send response if needed
                if (param->write.need_rsp) {
                    esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                                 param->write.trans_id, ESP_GATT_OK, NULL);
                }
            } else {
                // Long write (command longer than MTU-3): buffer prepared chunks
                handle_prepare_write(gatts_if, param);
            }
            break;

        case ESP_GATTS_EXEC_WRITE_EVT:
            if (param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC &&
                s_prep_buf && s_prep_len > 0) {
                wifi_mgr_ble_on_command(s_prep_buf, s_prep_len);
            }
            free(s_prep_buf);
            s_prep_buf = NULL;
            s_prep_len = 0;
            esp_ble_gatts_send_response(gatts_if, param->exec_write.conn_id,
                                        param->exec_write.trans_id, ESP_GATT_OK, NULL);
            break;

        case ESP_GATTS_MTU_EVT:
            s_profile.mtu = param->mtu.mtu;
            ESP_LOGI(TAG, "MTU changed to %d", param->mtu.mtu);
            break;

        default:
            break;
    }
}

// =============================================================================
// Backend Interface Implementation
// =============================================================================

esp_err_t wifi_mgr_ble_backend_notify_response(const uint8_t *data, size_t length)
{
    if (s_profile.gatts_if == ESP_GATT_IF_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_ble_gatts_send_indicate(s_profile.gatts_if, s_profile.conn_id,
                                                wifi_handle_table[IDX_CHAR_RESPONSE_VAL],
                                                length, (uint8_t *)data, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Notify failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

uint16_t wifi_mgr_ble_backend_get_mtu(void)
{
    return s_profile.connected ? s_profile.mtu : 0;
}

bool wifi_mgr_ble_backend_is_stack_running(void)
{
    return esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED;
}

esp_err_t wifi_mgr_ble_backend_init(const char *device_name)
{
    strncpy(s_device_name, device_name, sizeof(s_device_name) - 1);
    s_device_name[sizeof(s_device_name) - 1] = '\0';

    if (wifi_mgr_ble_backend_is_stack_running()) {
        // Stack already running — service-only mode
        s_ble_stack_owned = false;
        ESP_LOGI(TAG, "BLE stack already running, registering service only");
    } else {
        // Full stack init
        s_ble_stack_owned = true;

#ifdef CONFIG_WIFI_MGR_BLE_RELEASE_CLASSIC_BT_MEM
        // Opt-in: permanently gives up Bluetooth Classic to save RAM (#11).
        // Fails harmlessly if already released or the controller is in use.
        esp_err_t rel = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        if (rel != ESP_OK) {
            ESP_LOGW(TAG, "Classic BT mem release skipped: %s", esp_err_to_name(rel));
        }
#endif

        // Initialize BT controller
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        esp_err_t ret = esp_bt_controller_init(&bt_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
            return ret;
        }

        // Initialize Bluedroid
        ret = esp_bluedroid_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
            return ret;
        }

        ret = esp_bluedroid_enable();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    // Register callbacks (needed in both modes)
    esp_err_t ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GATTS register callback failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GAP register callback failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register application profile (triggers GATT table creation + advertising)
    ret = esp_ble_gatts_app_register(PROFILE_APP_IDX);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GATTS app register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Set MTU
    esp_ble_gatt_set_local_mtu(517);

    return ESP_OK;
}

esp_err_t wifi_mgr_ble_backend_deinit(void)
{
    // Disconnect active client
    if (s_profile.connected) {
        esp_ble_gap_disconnect(s_profile.remote_bda);
    }

    // Stop advertising
    esp_ble_gap_stop_advertising();

    // Unregister GATT app (removes our service)
    if (s_profile.gatts_if != ESP_GATT_IF_NONE) {
        esp_ble_gatts_app_unregister(s_profile.gatts_if);
    }

    // Full stack teardown only if we own it
    if (s_ble_stack_owned) {
        esp_bluedroid_disable();
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
    }

    s_profile.gatts_if = ESP_GATT_IF_NONE;
    s_profile.connected = false;
    s_profile.mtu = 23;
    s_ble_stack_owned = false;

    return ESP_OK;
}

#endif // CONFIG_WIFI_MGR_ENABLE_BLE && CONFIG_BT_BLUEDROID_ENABLED
