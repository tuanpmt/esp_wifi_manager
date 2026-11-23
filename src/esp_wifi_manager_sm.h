/**
 * @file esp_wifi_manager_sm.h
 * @brief Pure decision helpers for the WiFi Manager state machine.
 *
 * No ESP-IDF dependencies so the logic can be unit-tested on the host
 * (see test/host).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MGR_DISC_STALE = 0,        ///< Ignore: does not describe the current link
    WIFI_MGR_DISC_LOST,             ///< Link lost, do not reconnect automatically
    WIFI_MGR_DISC_LOST_RECONNECT,   ///< Link lost, schedule auto-reconnect
} wifi_mgr_disc_action_t;

typedef struct {
    bool during_connect;    ///< Event was raised while the connect sequence was running
    bool associated;        ///< Station is associated at the time the event is processed
    bool auto_reconnect;    ///< config.auto_reconnect
    bool ap_active;         ///< SoftAP is running
} wifi_mgr_disc_input_t;

wifi_mgr_disc_action_t wifi_mgr_sm_on_disconnect(const wifi_mgr_disc_input_t *in);
uint32_t wifi_mgr_sm_backoff_ms(uint32_t base_ms, uint32_t max_ms, unsigned retry);
void wifi_mgr_sm_copy_str(char *dst, size_t dst_size, const char *src);

/**
 * @brief Convert a v1.x NVS blob (ssid[32] first) to the v2 layout (ssid[33] first)
 *
 * Valid only for structs whose first member is the SSID and whose members are
 * all byte-aligned, so every later field is shifted by exactly one byte.
 *
 * @return true on success, false if new_len != old_len + 1
 */
bool wifi_mgr_sm_migrate_ssid32_blob(const void *old_blob, size_t old_len,
                                     void *new_blob, size_t new_len);

/**
 * @brief Extract and percent-decode the last path segment of a URI
 *
 * "/api/wifi/networks/My%20WiFi?x=1" -> "My WiFi". Rejects empty segments,
 * malformed escapes, embedded NULs and results that do not fit in dst.
 */
bool wifi_mgr_sm_uri_last_segment(char *dst, size_t dst_size, const char *uri);

#ifdef __cplusplus
}
#endif
