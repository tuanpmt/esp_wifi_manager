/**
 * @file esp_wifi_manager_sm.c
 * @brief Pure decision helpers for the WiFi Manager state machine.
 */
#include "esp_wifi_manager_sm.h"
#include <string.h>

wifi_mgr_disc_action_t wifi_mgr_sm_on_disconnect(const wifi_mgr_disc_input_t *in)
{
    // Disconnects raised while the connect sequence runs are attempt failures;
    // the sequence handles retry/fallback itself. Processing them later (the
    // manager task is blocked during the sequence) would tear down whatever
    // network the sequence ended up connecting to (issue #14).
    if (in->during_connect) {
        return WIFI_MGR_DISC_STALE;
    }
    // Associated again by the time the event is processed: the event is old.
    if (in->associated) {
        return WIFI_MGR_DISC_STALE;
    }
    if (in->auto_reconnect && !in->ap_active) {
        return WIFI_MGR_DISC_LOST_RECONNECT;
    }
    return WIFI_MGR_DISC_LOST;
}

uint32_t wifi_mgr_sm_backoff_ms(uint32_t base_ms, uint32_t max_ms, unsigned retry)
{
    if (retry >= 31) {
        return max_ms;
    }
    uint64_t delay = (uint64_t)base_ms << retry;
    return delay > max_ms ? max_ms : (uint32_t)delay;
}

void wifi_mgr_sm_copy_str(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    // Bounded scan: src may be a fixed-size field without a terminator.
    size_t len = 0;
    while (len < dst_size - 1 && src[len] != '\0') {
        len++;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}
