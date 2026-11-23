/**
 * @file test_sm.c
 * @brief Host-side unit tests for the pure state machine helpers.
 *
 * Build & run: make -C test/host
 */

#include "esp_wifi_manager_sm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define EXPECT_EQ(actual, expected) do { \
    long long _a = (long long)(actual), _e = (long long)(expected); \
    if (_a != _e) { \
        printf("  FAIL %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #actual, _a, _e); \
        failures++; \
    } \
} while (0)

#define RUN(test) do { printf("%s\n", #test); test(); } while (0)

// --- Issue #14: stale disconnect events ------------------------------------

static void test_disconnect_during_connect_sequence_is_stale(void)
{
    // Primary network failed while the connect sequence owned the radio;
    // the event is processed later, after the fallback network connected.
    wifi_mgr_disc_input_t in = {
        .during_connect = true, .associated = true,
        .auto_reconnect = true, .ap_active = false,
    };
    EXPECT_EQ(wifi_mgr_sm_on_disconnect(&in), WIFI_MGR_DISC_STALE);
}

static void test_disconnect_while_still_associated_is_stale(void)
{
    // Event raised outside the sequence, but by the time it is processed the
    // station is associated again: it cannot describe the current link.
    wifi_mgr_disc_input_t in = {
        .during_connect = false, .associated = true,
        .auto_reconnect = true, .ap_active = false,
    };
    EXPECT_EQ(wifi_mgr_sm_on_disconnect(&in), WIFI_MGR_DISC_STALE);
}

static void test_failed_attempt_with_no_link_is_stale_for_sequence(void)
{
    // Failure during the sequence and still not associated: the sequence
    // itself handles retries/fallback, so the task must not start another.
    wifi_mgr_disc_input_t in = {
        .during_connect = true, .associated = false,
        .auto_reconnect = true, .ap_active = false,
    };
    EXPECT_EQ(wifi_mgr_sm_on_disconnect(&in), WIFI_MGR_DISC_STALE);
}

static void test_real_link_loss_reconnects(void)
{
    wifi_mgr_disc_input_t in = {
        .during_connect = false, .associated = false,
        .auto_reconnect = true, .ap_active = false,
    };
    EXPECT_EQ(wifi_mgr_sm_on_disconnect(&in), WIFI_MGR_DISC_LOST_RECONNECT);
}

static void test_real_link_loss_without_auto_reconnect(void)
{
    wifi_mgr_disc_input_t in = {
        .during_connect = false, .associated = false,
        .auto_reconnect = false, .ap_active = false,
    };
    EXPECT_EQ(wifi_mgr_sm_on_disconnect(&in), WIFI_MGR_DISC_LOST);
}

static void test_real_link_loss_with_ap_active_does_not_reconnect(void)
{
    wifi_mgr_disc_input_t in = {
        .during_connect = false, .associated = false,
        .auto_reconnect = true, .ap_active = true,
    };
    EXPECT_EQ(wifi_mgr_sm_on_disconnect(&in), WIFI_MGR_DISC_LOST);
}

// --- Backoff -----------------------------------------------------------------

static void test_backoff_doubles_from_base(void)
{
    EXPECT_EQ(wifi_mgr_sm_backoff_ms(1000, 60000, 0), 1000);
    EXPECT_EQ(wifi_mgr_sm_backoff_ms(1000, 60000, 1), 2000);
    EXPECT_EQ(wifi_mgr_sm_backoff_ms(1000, 60000, 3), 8000);
}

static void test_backoff_is_capped(void)
{
    EXPECT_EQ(wifi_mgr_sm_backoff_ms(5000, 60000, 4), 60000);
}

static void test_backoff_large_retry_does_not_overflow(void)
{
    // retry >= 32 is undefined behavior for a plain shift; must clamp.
    EXPECT_EQ(wifi_mgr_sm_backoff_ms(5000, 60000, 40), 60000);
    EXPECT_EQ(wifi_mgr_sm_backoff_ms(5000, 60000, 31), 60000);
}

// --- SSID copy (issue #8) ---------------------------------------------------

static void test_ssid_copy_32_bytes_is_terminated(void)
{
    // Scan record SSIDs are 32 bytes + NUL; "DIRECT-B9-HP OfficeJet Pro 6970" is 31,
    // use an exactly-32-byte name.
    const char src[33] = "0123456789abcdef0123456789ABCDEF";
    char dst[33];
    memset(dst, 'X', sizeof(dst));
    wifi_mgr_sm_copy_str(dst, sizeof(dst), src);
    EXPECT_EQ(strlen(dst), 32);
    EXPECT_EQ(strcmp(dst, src), 0);
}

static void test_ssid_copy_truncates_and_terminates(void)
{
    char dst[8];
    memset(dst, 'X', sizeof(dst));
    wifi_mgr_sm_copy_str(dst, sizeof(dst), "0123456789");
    EXPECT_EQ(strcmp(dst, "0123456"), 0);
}

static void test_ssid_copy_null_src_gives_empty(void)
{
    char dst[4] = "abc";
    wifi_mgr_sm_copy_str(dst, sizeof(dst), NULL);
    EXPECT_EQ(dst[0], '\0');
}

// --- NVS migration for ssid[32] -> ssid[33] (v1.x blobs) ---------------------

typedef struct { char ssid[32]; char password[64]; uint8_t priority; } legacy_net_t;
typedef struct { char ssid[33]; char password[64]; uint8_t priority; } new_net_t;

static void test_migrate_legacy_network_blob(void)
{
    legacy_net_t old;
    memset(&old, 0, sizeof(old));
    strcpy(old.ssid, "HomeWiFi");
    strcpy(old.password, "secret123");
    old.priority = 42;

    new_net_t out;
    memset(&out, 0xAA, sizeof(out));
    EXPECT_EQ(wifi_mgr_sm_migrate_ssid32_blob(&old, sizeof(old), &out, sizeof(out)), 1);
    EXPECT_EQ(strcmp(out.ssid, "HomeWiFi"), 0);
    EXPECT_EQ(strcmp(out.password, "secret123"), 0);
    EXPECT_EQ(out.priority, 42);
}

static void test_migrate_legacy_blob_with_full_31_char_ssid(void)
{
    legacy_net_t old;
    memset(&old, 0, sizeof(old));
    memcpy(old.ssid, "0123456789012345678901234567890", 31);  // max legacy length
    strcpy(old.password, "pw");

    new_net_t out;
    EXPECT_EQ(wifi_mgr_sm_migrate_ssid32_blob(&old, sizeof(old), &out, sizeof(out)), 1);
    EXPECT_EQ(strlen(out.ssid), 31);
    EXPECT_EQ(strcmp(out.password, "pw"), 0);
}

static void test_migrate_rejects_wrong_size(void)
{
    uint8_t old[10] = {0};
    new_net_t out;
    EXPECT_EQ(wifi_mgr_sm_migrate_ssid32_blob(old, sizeof(old), &out, sizeof(out)), 0);
}

// --- URI path segment decoding (REST /networks/:ssid, /vars/:key) ----------

static void test_uri_segment_decodes_percent_escapes(void)
{
    char out[33];
    EXPECT_EQ(wifi_mgr_sm_uri_last_segment(out, sizeof(out), "/api/wifi/networks/My%20WiFi"), 1);
    EXPECT_EQ(strcmp(out, "My WiFi"), 0);
}

static void test_uri_segment_decodes_utf8(void)
{
    char out[33];
    // "Nhà" = N h 0xC3 0xA0
    EXPECT_EQ(wifi_mgr_sm_uri_last_segment(out, sizeof(out), "/api/wifi/networks/Nh%C3%A0"), 1);
    EXPECT_EQ(strcmp(out, "Nh\xC3\xA0"), 0);
}

static void test_uri_segment_strips_query(void)
{
    char out[33];
    EXPECT_EQ(wifi_mgr_sm_uri_last_segment(out, sizeof(out), "/api/wifi/vars/name?x=1"), 1);
    EXPECT_EQ(strcmp(out, "name"), 0);
}

static void test_uri_segment_keeps_plus_literal(void)
{
    // '+' is only a space in form encoding, not in paths
    char out[33];
    EXPECT_EQ(wifi_mgr_sm_uri_last_segment(out, sizeof(out), "/n/a+b"), 1);
    EXPECT_EQ(strcmp(out, "a+b"), 0);
}

static void test_uri_segment_rejects_overflow(void)
{
    char out[4];
    EXPECT_EQ(wifi_mgr_sm_uri_last_segment(out, sizeof(out), "/n/abcd"), 0);
}

static void test_uri_segment_rejects_bad_escape_and_empty(void)
{
    char out[16];
    EXPECT_EQ(wifi_mgr_sm_uri_last_segment(out, sizeof(out), "/n/a%2"), 0);
    EXPECT_EQ(wifi_mgr_sm_uri_last_segment(out, sizeof(out), "/n/a%zz"), 0);
    EXPECT_EQ(wifi_mgr_sm_uri_last_segment(out, sizeof(out), "/n/"), 0);
    EXPECT_EQ(wifi_mgr_sm_uri_last_segment(out, sizeof(out), "/n/%00x"), 0);
}

// --- Static file path sanitizing (Web UI wildcard handler) -----------------

static void test_path_accepts_normal_file(void)
{
    char out[64];
    EXPECT_EQ(wifi_mgr_sm_sanitize_path(out, sizeof(out), "/assets/logo.png?v=2"), 1);
    EXPECT_EQ(strcmp(out, "/assets/logo.png"), 0);
}

static void test_path_rejects_traversal(void)
{
    char out[64];
    EXPECT_EQ(wifi_mgr_sm_sanitize_path(out, sizeof(out), "/../secret.txt"), 0);
    EXPECT_EQ(wifi_mgr_sm_sanitize_path(out, sizeof(out), "/assets/../../x"), 0);
    EXPECT_EQ(wifi_mgr_sm_sanitize_path(out, sizeof(out), "/assets/%2e%2e/x"), 0);
    EXPECT_EQ(wifi_mgr_sm_sanitize_path(out, sizeof(out), "/a\\..\\b"), 0);
    EXPECT_EQ(wifi_mgr_sm_sanitize_path(out, sizeof(out), "assets/x"), 0);
}

static void test_path_allows_dots_inside_names(void)
{
    char out[64];
    EXPECT_EQ(wifi_mgr_sm_sanitize_path(out, sizeof(out), "/app..min.js"), 1);
    EXPECT_EQ(wifi_mgr_sm_sanitize_path(out, sizeof(out), "/.well-known/x"), 1);
}

static void test_path_rejects_overflow(void)
{
    char out[8];
    EXPECT_EQ(wifi_mgr_sm_sanitize_path(out, sizeof(out), "/abcdefgh"), 0);
}

// --- MIME types --------------------------------------------------------------

static void test_mime_matches_suffix_not_substring(void)
{
    EXPECT_EQ(strcmp(wifi_mgr_sm_mime_type("/data.json"), "application/json"), 0);
    EXPECT_EQ(strcmp(wifi_mgr_sm_mime_type("/app.js"), "application/javascript"), 0);
    EXPECT_EQ(strcmp(wifi_mgr_sm_mime_type("/my.json.backup"), "application/octet-stream"), 0);
    EXPECT_EQ(strcmp(wifi_mgr_sm_mime_type("/INDEX.HTML"), "text/html"), 0);
    EXPECT_EQ(strcmp(wifi_mgr_sm_mime_type("/style.css"), "text/css"), 0);
    EXPECT_EQ(strcmp(wifi_mgr_sm_mime_type("/noext"), "application/octet-stream"), 0);
    EXPECT_EQ(strcmp(wifi_mgr_sm_mime_type("/app.js.gz"), "application/gzip"), 0);
}

int main(void)
{
    RUN(test_disconnect_during_connect_sequence_is_stale);
    RUN(test_disconnect_while_still_associated_is_stale);
    RUN(test_failed_attempt_with_no_link_is_stale_for_sequence);
    RUN(test_real_link_loss_reconnects);
    RUN(test_real_link_loss_without_auto_reconnect);
    RUN(test_real_link_loss_with_ap_active_does_not_reconnect);
    RUN(test_backoff_doubles_from_base);
    RUN(test_backoff_is_capped);
    RUN(test_backoff_large_retry_does_not_overflow);
    RUN(test_ssid_copy_32_bytes_is_terminated);
    RUN(test_ssid_copy_truncates_and_terminates);
    RUN(test_ssid_copy_null_src_gives_empty);
    RUN(test_migrate_legacy_network_blob);
    RUN(test_migrate_legacy_blob_with_full_31_char_ssid);
    RUN(test_migrate_rejects_wrong_size);
    RUN(test_uri_segment_decodes_percent_escapes);
    RUN(test_uri_segment_decodes_utf8);
    RUN(test_uri_segment_strips_query);
    RUN(test_uri_segment_keeps_plus_literal);
    RUN(test_uri_segment_rejects_overflow);
    RUN(test_uri_segment_rejects_bad_escape_and_empty);
    RUN(test_path_accepts_normal_file);
    RUN(test_path_rejects_traversal);
    RUN(test_path_allows_dots_inside_names);
    RUN(test_path_rejects_overflow);
    RUN(test_mime_matches_suffix_not_substring);

    if (failures) {
        printf("\n%d assertion(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("\nAll tests passed\n");
    return EXIT_SUCCESS;
}
