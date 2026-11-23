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

bool wifi_mgr_sm_migrate_ssid32_blob(const void *old_blob, size_t old_len,
                                     void *new_blob, size_t new_len)
{
    const size_t legacy_ssid_len = 32;
    if (!old_blob || !new_blob || old_len <= legacy_ssid_len || new_len != old_len + 1) {
        return false;
    }
    const uint8_t *src = (const uint8_t *)old_blob;
    uint8_t *dst = (uint8_t *)new_blob;
    memcpy(dst, src, legacy_ssid_len);
    dst[legacy_ssid_len] = '\0';
    memcpy(dst + legacy_ssid_len + 1, src + legacy_ssid_len, old_len - legacy_ssid_len);
    return true;
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

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool wifi_mgr_sm_uri_last_segment(char *dst, size_t dst_size, const char *uri)
{
    if (!dst || dst_size == 0 || !uri) {
        return false;
    }
    dst[0] = '\0';

    size_t uri_len = strcspn(uri, "?#");
    const char *seg = uri;
    for (size_t i = 0; i < uri_len; i++) {
        if (uri[i] == '/') seg = uri + i + 1;
    }
    const char *end = uri + uri_len;
    if (seg >= end) {
        return false;
    }

    size_t n = 0;
    for (const char *p = seg; p < end; p++) {
        char c = *p;
        if (c == '%') {
            if (end - p < 3) return false;
            int hi = hex_value(p[1]);
            int lo = hex_value(p[2]);
            if (hi < 0 || lo < 0) return false;
            c = (char)((hi << 4) | lo);
            if (c == '\0') return false;
            p += 2;
        }
        if (n + 1 >= dst_size) {
            dst[0] = '\0';
            return false;
        }
        dst[n++] = c;
    }
    dst[n] = '\0';
    return true;
}

bool wifi_mgr_sm_sanitize_path(char *dst, size_t dst_size, const char *uri)
{
    if (!dst || dst_size == 0 || !uri || uri[0] != '/') {
        return false;
    }
    dst[0] = '\0';

    size_t n = 0;
    size_t uri_len = strcspn(uri, "?#");
    for (size_t i = 0; i < uri_len; i++) {
        char c = uri[i];
        if (c == '%') {
            if (i + 2 >= uri_len) return false;
            int hi = hex_value(uri[i + 1]);
            int lo = hex_value(uri[i + 2]);
            if (hi < 0 || lo < 0) return false;
            c = (char)((hi << 4) | lo);
            i += 2;
        }
        if (c == '\0' || c == '\\') return false;
        if (n + 1 >= dst_size) {
            dst[0] = '\0';
            return false;
        }
        dst[n++] = c;
    }
    dst[n] = '\0';

    // Reject any ".." path segment (after decoding)
    for (const char *seg = dst; *seg; ) {
        seg++;  // skip '/'
        size_t len = strcspn(seg, "/");
        if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            dst[0] = '\0';
            return false;
        }
        seg += len;
    }
    return true;
}

static bool ends_with_ci(const char *s, size_t len, const char *suffix)
{
    size_t sl = strlen(suffix);
    if (len < sl) return false;
    const char *p = s + len - sl;
    for (size_t i = 0; i < sl; i++) {
        char a = p[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != suffix[i]) return false;
    }
    return true;
}

const char *wifi_mgr_sm_mime_type(const char *path)
{
    static const struct { const char *ext; const char *type; } types[] = {
        { ".html",  "text/html" },
        { ".htm",   "text/html" },
        { ".js",    "application/javascript" },
        { ".mjs",   "application/javascript" },
        { ".css",   "text/css" },
        { ".json",  "application/json" },
        { ".png",   "image/png" },
        { ".jpg",   "image/jpeg" },
        { ".jpeg",  "image/jpeg" },
        { ".gif",   "image/gif" },
        { ".webp",  "image/webp" },
        { ".svg",   "image/svg+xml" },
        { ".ico",   "image/x-icon" },
        { ".woff2", "font/woff2" },
        { ".woff",  "font/woff" },
        { ".ttf",   "font/ttf" },
        { ".txt",   "text/plain" },
    };
    if (!path) return "application/octet-stream";
    size_t len = strlen(path);
    // "foo.js.gz" is served as foo.js with Content-Encoding: gzip
    if (ends_with_ci(path, len, ".gz")) len -= 3;
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        if (ends_with_ci(path, len, types[i].ext)) return types[i].type;
    }
    return "application/octet-stream";
}
