/**
 * @file esp_wifi_manager_ble.c
 * @brief BLE shared layer — JSON command routing and protocol handling
 *
 * Stack-specific transport (Bluedroid or NimBLE) is in the backend files.
 */

#include "esp_wifi_manager_priv.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#ifdef CONFIG_WIFI_MGR_ENABLE_BLE

#include "esp_wifi_manager_ble_int.h"

static const char *TAG = "wifi_mgr_ble";

// =============================================================================
// Connection State (shared)
// =============================================================================

static volatile bool s_connected = false;
static volatile bool s_response_notify_enabled = false;
static bool s_initialized = false;

// =============================================================================
// Command Queue
// =============================================================================
//
// Backends call wifi_mgr_ble_on_command() from the BLE host task (NimBLE host
// or Bluedroid BTC task). Commands such as "scan" block for seconds and build
// large JSON documents, so they are processed on a dedicated task instead.

#define BLE_CMD_MAX_LEN          512
#define BLE_CMD_QUEUE_DEPTH      2
#define BLE_CMD_TASK_STACK_SIZE  4096

typedef struct {
    char *data;     // NUL-terminated, heap-allocated; NULL = stop request
    bool is_reply;  // data is a ready-made response to send, not a command
} ble_cmd_msg_t;

static QueueHandle_t s_cmd_queue;
static TaskHandle_t s_cmd_task;
static SemaphoreHandle_t s_cmd_task_done;
// Guards s_cmd_accepting + queue sends from BLE host callbacks against teardown
static SemaphoreHandle_t s_cmd_lock;
static bool s_cmd_accepting;

static void handle_command(const char *json_str);
static void send_response(const char *json_str);

// =============================================================================
// Command Handlers
// =============================================================================

static cJSON *handle_get_status(void)
{
    cJSON *data = cJSON_CreateObject();
    wifi_status_t status;

    if (wifi_manager_get_status(&status) == ESP_OK) {
        cJSON_AddStringToObject(data, "state",
            status.state == WIFI_STATE_CONNECTED ? "connected" :
            status.state == WIFI_STATE_CONNECTING ? "connecting" : "disconnected");
        cJSON_AddStringToObject(data, "ssid", status.ssid);
        cJSON_AddNumberToObject(data, "rssi", status.rssi);
        cJSON_AddNumberToObject(data, "quality", status.quality);
        cJSON_AddStringToObject(data, "ip", status.ip);
        cJSON_AddNumberToObject(data, "channel", status.channel);
        cJSON_AddStringToObject(data, "netmask", status.netmask);
        cJSON_AddStringToObject(data, "gateway", status.gateway);
        cJSON_AddStringToObject(data, "dns", status.dns);
        cJSON_AddStringToObject(data, "mac", status.mac);
        cJSON_AddStringToObject(data, "hostname", status.hostname);
        cJSON_AddNumberToObject(data, "uptime_ms", status.uptime_ms);
        cJSON_AddBoolToObject(data, "ap_active", status.ap_active);
    }

    return data;
}

static cJSON *handle_scan(void)
{
    // Heap-allocate results to keep the BLE command task stack small
    wifi_scan_result_t *results = calloc(WIFI_MGR_MAX_SCAN_RESULTS, sizeof(wifi_scan_result_t));
    if (!results) {
        return NULL;
    }

    size_t count = 0;
    esp_err_t ret = wifi_manager_scan(results, WIFI_MGR_MAX_SCAN_RESULTS, &count);
    if (ret != ESP_OK) {
        free(results);
        return NULL;
    }

    cJSON *data = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(data, "networks");

    for (size_t i = 0; i < count; i++) {
        cJSON *net = cJSON_CreateObject();
        cJSON_AddStringToObject(net, "ssid", results[i].ssid);
        cJSON_AddNumberToObject(net, "rssi", results[i].rssi);

        const char *auth_str = "UNKNOWN";
        switch (results[i].auth) {
            case WIFI_AUTH_OPEN: auth_str = "OPEN"; break;
            case WIFI_AUTH_WEP: auth_str = "WEP"; break;
            case WIFI_AUTH_WPA_PSK: auth_str = "WPA"; break;
            case WIFI_AUTH_WPA2_PSK: auth_str = "WPA2"; break;
            case WIFI_AUTH_WPA_WPA2_PSK: auth_str = "WPA/WPA2"; break;
            case WIFI_AUTH_WPA3_PSK: auth_str = "WPA3"; break;
            default: break;
        }
        cJSON_AddStringToObject(net, "auth", auth_str);
        cJSON_AddItemToArray(arr, net);
    }

    free(results);
    return data;
}

static cJSON *handle_list_networks(void)
{
    wifi_network_t networks[WIFI_MGR_MAX_NETWORKS];
    size_t count = 0;

    wifi_manager_list_networks(networks, WIFI_MGR_MAX_NETWORKS, &count);

    cJSON *data = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(data, "networks");

    for (size_t i = 0; i < count; i++) {
        cJSON *net = cJSON_CreateObject();
        cJSON_AddStringToObject(net, "ssid", networks[i].ssid);
        cJSON_AddNumberToObject(net, "priority", networks[i].priority);
        cJSON_AddItemToArray(arr, net);
    }

    return data;
}

static cJSON *handle_add_network(cJSON *params)
{
    cJSON *ssid = cJSON_GetObjectItem(params, "ssid");
    cJSON *password = cJSON_GetObjectItem(params, "password");
    cJSON *priority = cJSON_GetObjectItem(params, "priority");

    if (!cJSON_IsString(ssid)) {
        return NULL;
    }

    // Upsert: fields not present in the request keep their saved values
    esp_err_t ret = wifi_mgr_network_upsert(ssid->valuestring,
                                            cJSON_IsString(password) ? password->valuestring : NULL,
                                            cJSON_IsNumber(priority) ? priority->valueint : -1,
                                            true);
    if (ret != ESP_OK) {
        return NULL;
    }

    return cJSON_CreateObject();
}

static cJSON *handle_del_network(cJSON *params)
{
    cJSON *ssid = cJSON_GetObjectItem(params, "ssid");
    if (!cJSON_IsString(ssid)) {
        return NULL;
    }

    esp_err_t ret = wifi_manager_remove_network(ssid->valuestring);
    if (ret != ESP_OK) {
        return NULL;
    }

    return cJSON_CreateObject();
}

static cJSON *handle_connect(cJSON *params)
{
    const char *ssid = NULL;
    cJSON *ssid_item = cJSON_GetObjectItem(params, "ssid");
    if (cJSON_IsString(ssid_item)) {
        ssid = ssid_item->valuestring;
    }

    wifi_manager_connect(ssid);
    return cJSON_CreateObject();
}

static cJSON *handle_disconnect(void)
{
    wifi_manager_disconnect();
    return cJSON_CreateObject();
}

static cJSON *handle_ap_status(void)
{
    wifi_ap_status_t status;
    wifi_manager_get_ap_status(&status);

    cJSON *data = cJSON_CreateObject();
    cJSON_AddBoolToObject(data, "active", status.active);
    cJSON_AddStringToObject(data, "ssid", status.ssid);
    cJSON_AddStringToObject(data, "ip", status.ip);
    cJSON_AddNumberToObject(data, "channel", status.channel);
    cJSON_AddNumberToObject(data, "sta_count", status.sta_count);

    return data;
}

static cJSON *handle_start_ap(cJSON *params)
{
    wifi_mgr_ap_config_t *config = NULL;
    wifi_mgr_ap_config_t temp_config = {0};

    if (params) {
        wifi_manager_get_ap_config(&temp_config);

        cJSON *ssid = cJSON_GetObjectItem(params, "ssid");
        cJSON *password = cJSON_GetObjectItem(params, "password");

        if (cJSON_IsString(ssid)) {
            strncpy(temp_config.ssid, ssid->valuestring, sizeof(temp_config.ssid) - 1);
        }
        if (cJSON_IsString(password)) {
            strncpy(temp_config.password, password->valuestring, sizeof(temp_config.password) - 1);
        }
        config = &temp_config;
    }

    wifi_manager_start_ap(config);
    return cJSON_CreateObject();
}

static cJSON *handle_stop_ap(void)
{
    wifi_manager_stop_ap();
    return cJSON_CreateObject();
}

static cJSON *handle_get_var(cJSON *params)
{
    cJSON *key = cJSON_GetObjectItem(params, "key");
    if (!cJSON_IsString(key)) {
        return NULL;
    }

    char value[128];
    esp_err_t ret = wifi_manager_get_var(key->valuestring, value, sizeof(value));
    if (ret != ESP_OK) {
        return NULL;
    }

    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "key", key->valuestring);
    cJSON_AddStringToObject(data, "value", value);
    return data;
}

static cJSON *handle_set_var(cJSON *params)
{
    cJSON *key = cJSON_GetObjectItem(params, "key");
    cJSON *value = cJSON_GetObjectItem(params, "value");
    if (!cJSON_IsString(key) || !cJSON_IsString(value)) {
        return NULL;
    }

    esp_err_t ret = wifi_manager_set_var(key->valuestring, value->valuestring);
    if (ret != ESP_OK) {
        return NULL;
    }

    return cJSON_CreateObject();
}

static cJSON *handle_update_network(cJSON *params)
{
    cJSON *ssid = cJSON_GetObjectItem(params, "ssid");
    if (!cJSON_IsString(ssid)) {
        return NULL;
    }

    cJSON *password = cJSON_GetObjectItem(params, "password");
    cJSON *priority = cJSON_GetObjectItem(params, "priority");

    // Partial update: omitted fields keep their saved values
    esp_err_t ret = wifi_mgr_network_upsert(ssid->valuestring,
                                            cJSON_IsString(password) ? password->valuestring : NULL,
                                            cJSON_IsNumber(priority) ? priority->valueint : -1,
                                            false);
    if (ret != ESP_OK) {
        return NULL;
    }

    return cJSON_CreateObject();
}

static cJSON *handle_list_vars(void)
{
    wifi_mgr_lock();

    cJSON *data = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(data, "vars");

    for (size_t i = 0; i < g_wifi_mgr->var_count; i++) {
        cJSON *var = cJSON_CreateObject();
        cJSON_AddStringToObject(var, "key", g_wifi_mgr->vars[i].key);
        cJSON_AddStringToObject(var, "value", g_wifi_mgr->vars[i].value);
        cJSON_AddItemToArray(arr, var);
    }

    wifi_mgr_unlock();
    return data;
}

static cJSON *handle_del_var(cJSON *params)
{
    cJSON *key = cJSON_GetObjectItem(params, "key");
    if (!cJSON_IsString(key)) {
        return NULL;
    }

    esp_err_t ret = wifi_manager_del_var(key->valuestring);
    if (ret != ESP_OK) {
        return NULL;
    }

    return cJSON_CreateObject();
}

static cJSON *handle_factory_reset(void)
{
    wifi_manager_factory_reset();
    return cJSON_CreateObject();
}

// =============================================================================
// Response Sending (via backend)
// =============================================================================

/** Minimum payload size if MTU query fails or returns something unusable. */
#define BLE_MIN_CHUNK_SIZE  20

/** Delay between chunks to avoid flooding the BLE controller's TX queue. */
#define BLE_CHUNK_DELAY_MS  20

static void send_response(const char *json_str)
{
    if (!s_connected || !s_response_notify_enabled) {
        return;
    }

    uint16_t mtu = wifi_mgr_ble_backend_get_mtu();
    size_t chunk_size = (mtu >= 3 + BLE_MIN_CHUNK_SIZE) ? (mtu - 3) : BLE_MIN_CHUNK_SIZE;

    const uint8_t *ptr = (const uint8_t *)json_str;
    size_t remaining = strlen(json_str);

    while (remaining > 0) {
        size_t send_len = (remaining > chunk_size) ? chunk_size : remaining;

        esp_err_t err = wifi_mgr_ble_backend_notify_response(ptr, send_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Notify failed during chunked send, %d bytes remaining",
                     (int)remaining);
            return;
        }

        ptr += send_len;
        remaining -= send_len;

        if (remaining > 0) {
            vTaskDelay(pdMS_TO_TICKS(BLE_CHUNK_DELAY_MS));
        }
    }
}

// =============================================================================
// Command Router
// =============================================================================

static void handle_command(const char *json_str)
{
    ESP_LOGD(TAG, "Command: %s", json_str);

    cJSON *json = cJSON_Parse(json_str);
    if (!json) {
        send_response("{\"status\":\"error\",\"error\":\"Invalid JSON\"}");
        return;
    }

    cJSON *cmd = cJSON_GetObjectItem(json, "cmd");
    cJSON *params = cJSON_GetObjectItem(json, "params");

    if (!cJSON_IsString(cmd)) {
        cJSON_Delete(json);
        send_response("{\"status\":\"error\",\"error\":\"Missing cmd\"}");
        return;
    }

    const char *cmd_str = cmd->valuestring;
    cJSON *result = NULL;

    // Route command
    if (strcmp(cmd_str, "get_status") == 0) {
        result = handle_get_status();
    } else if (strcmp(cmd_str, "scan") == 0) {
        result = handle_scan();
    } else if (strcmp(cmd_str, "list_networks") == 0) {
        result = handle_list_networks();
    } else if (strcmp(cmd_str, "add_network") == 0) {
        result = handle_add_network(params);
    } else if (strcmp(cmd_str, "del_network") == 0) {
        result = handle_del_network(params);
    } else if (strcmp(cmd_str, "connect") == 0) {
        result = handle_connect(params);
    } else if (strcmp(cmd_str, "disconnect") == 0) {
        result = handle_disconnect();
    } else if (strcmp(cmd_str, "get_ap_status") == 0) {
        result = handle_ap_status();
    } else if (strcmp(cmd_str, "start_ap") == 0) {
        result = handle_start_ap(params);
    } else if (strcmp(cmd_str, "stop_ap") == 0) {
        result = handle_stop_ap();
    } else if (strcmp(cmd_str, "get_var") == 0) {
        result = handle_get_var(params);
    } else if (strcmp(cmd_str, "set_var") == 0) {
        result = handle_set_var(params);
    } else if (strcmp(cmd_str, "update_network") == 0) {
        result = handle_update_network(params);
    } else if (strcmp(cmd_str, "list_vars") == 0) {
        result = handle_list_vars();
    } else if (strcmp(cmd_str, "del_var") == 0) {
        result = handle_del_var(params);
    } else if (strcmp(cmd_str, "factory_reset") == 0) {
        result = handle_factory_reset();
    } else {
        cJSON_Delete(json);
        send_response("{\"status\":\"error\",\"error\":\"Unknown command\"}");
        return;
    }

    cJSON_Delete(json);

    // Build response
    cJSON *response = cJSON_CreateObject();
    if (result) {
        cJSON_AddStringToObject(response, "status", "ok");
        cJSON_AddItemToObject(response, "data", result);
    } else {
        cJSON_AddStringToObject(response, "status", "error");
        cJSON_AddStringToObject(response, "error", "Command failed");
    }

    char *response_str = cJSON_PrintUnformatted(response);
    if (response_str) {
        send_response(response_str);
        free(response_str);
    }
    cJSON_Delete(response);
}

// =============================================================================
// Callbacks from stack backend
// =============================================================================

static void ble_cmd_task(void *arg)
{
    ble_cmd_msg_t msg;
    while (xQueueReceive(s_cmd_queue, &msg, portMAX_DELAY) == pdTRUE) {
        if (!msg.data) {
            break;  // stop request
        }
        // All notifications go out from this task, so replies never
        // interleave with the chunks of another response
        if (msg.is_reply) {
            send_response(msg.data);
        } else {
            handle_command(msg.data);
        }
        free(msg.data);
    }
    xSemaphoreGive(s_cmd_task_done);
    vTaskDelete(NULL);
}

void wifi_mgr_ble_on_command(const uint8_t *data, size_t length)
{
    if (!s_cmd_lock || !data || length == 0) {
        return;
    }

    ble_cmd_msg_t msg = {0};
    if (length > BLE_CMD_MAX_LEN) {
        ESP_LOGW(TAG, "Command too long (%u bytes), max %d", (unsigned)length, BLE_CMD_MAX_LEN);
        msg.data = strdup("{\"status\":\"error\",\"error\":\"Command too long\"}");
        msg.is_reply = true;
    } else {
        msg.data = malloc(length + 1);
        if (msg.data) {
            memcpy(msg.data, data, length);
            msg.data[length] = '\0';
        }
    }
    if (!msg.data) {
        return;
    }

    bool queued = false;
    xSemaphoreTake(s_cmd_lock, portMAX_DELAY);
    if (s_cmd_accepting) {
        queued = xQueueSend(s_cmd_queue, &msg, 0) == pdTRUE;
    }
    xSemaphoreGive(s_cmd_lock);

    if (!queued) {
        // Busy with another command (or shutting down). Replying from here
        // could interleave with a response being sent, so only log it.
        ESP_LOGW(TAG, "Command dropped (busy)");
        free(msg.data);
    }
}

static esp_err_t cmd_task_start(void)
{
    if (!s_cmd_lock) {
        // Created once and kept: host callbacks may race with deinit
        s_cmd_lock = xSemaphoreCreateMutex();
        if (!s_cmd_lock) return ESP_ERR_NO_MEM;
    }
    s_cmd_queue = xQueueCreate(BLE_CMD_QUEUE_DEPTH, sizeof(ble_cmd_msg_t));
    s_cmd_task_done = xSemaphoreCreateBinary();
    if (!s_cmd_queue || !s_cmd_task_done ||
        xTaskCreate(ble_cmd_task, "wifi_ble_cmd", BLE_CMD_TASK_STACK_SIZE, NULL,
                    WIFI_MGR_TASK_PRIORITY, &s_cmd_task) != pdPASS) {
        if (s_cmd_queue) vQueueDelete(s_cmd_queue);
        if (s_cmd_task_done) vSemaphoreDelete(s_cmd_task_done);
        s_cmd_queue = NULL;
        s_cmd_task_done = NULL;
        s_cmd_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(s_cmd_lock, portMAX_DELAY);
    s_cmd_accepting = true;
    xSemaphoreGive(s_cmd_lock);
    return ESP_OK;
}

/**
 * @brief Stop the command task after it finishes the command in progress
 */
static void cmd_task_stop(void)
{
    if (!s_cmd_task) {
        return;
    }
    // Stop accepting first: after this no host callback touches the queue
    xSemaphoreTake(s_cmd_lock, portMAX_DELAY);
    s_cmd_accepting = false;
    xSemaphoreGive(s_cmd_lock);

    // Drop queued commands, then ask the task to exit and wait for it
    ble_cmd_msg_t msg;
    while (xQueueReceive(s_cmd_queue, &msg, 0) == pdTRUE) {
        free(msg.data);
    }
    msg.data = NULL;
    msg.is_reply = false;
    xQueueSend(s_cmd_queue, &msg, portMAX_DELAY);
    if (xSemaphoreTake(s_cmd_task_done, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGE(TAG, "BLE command task did not stop in time");
        return;  // leak rather than free resources still in use
    }
    s_cmd_task = NULL;
    while (xQueueReceive(s_cmd_queue, &msg, 0) == pdTRUE) {
        free(msg.data);
    }
    vQueueDelete(s_cmd_queue);
    vSemaphoreDelete(s_cmd_task_done);
    s_cmd_queue = NULL;
    s_cmd_task_done = NULL;
}

void wifi_mgr_ble_on_connect(void)
{
    s_connected = true;
    s_response_notify_enabled = false;
}

void wifi_mgr_ble_on_disconnect(void)
{
    s_connected = false;
    s_response_notify_enabled = false;
}

void wifi_mgr_ble_set_response_notify(bool enabled)
{
    s_response_notify_enabled = enabled;
}

// =============================================================================
// Public API
// =============================================================================

esp_err_t wifi_mgr_ble_init(void)
{
    if (!g_wifi_mgr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing BLE interface");

    // Expand device name template
    const char *name_template = g_wifi_mgr->config.ble.device_name;
    if (!name_template || !name_template[0]) {
        name_template = CONFIG_WIFI_MGR_BLE_DEVICE_NAME;
    }

    char device_name[32];
    wifi_mgr_expand_template(name_template, device_name, sizeof(device_name));

    esp_err_t ret = cmd_task_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start BLE command task");
        return ret;
    }

    ret = wifi_mgr_ble_backend_init(device_name);
    if (ret != ESP_OK) {
        cmd_task_stop();
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "BLE interface initialized");
    return ESP_OK;
}

esp_err_t wifi_mgr_ble_deinit(void)
{
    // Never touch the BLE stack if we did not start the interface: the
    // application may own it (ble.enable=false or init failed).
    if (!s_initialized) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Deinitializing BLE interface");

    // Suppress notifications from the command in progress, stop command
    // processing, then tear down the transport
    s_connected = false;
    s_response_notify_enabled = false;
    cmd_task_stop();
    esp_err_t ret = wifi_mgr_ble_backend_deinit();
    s_initialized = false;
    return ret;
}

#else // CONFIG_WIFI_MGR_ENABLE_BLE

esp_err_t wifi_mgr_ble_init(void)
{
    return ESP_OK;
}

esp_err_t wifi_mgr_ble_deinit(void)
{
    return ESP_OK;
}

#endif // CONFIG_WIFI_MGR_ENABLE_BLE
