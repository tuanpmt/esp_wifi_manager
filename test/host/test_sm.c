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

    if (failures) {
        printf("\n%d assertion(s) failed\n", failures);
        return EXIT_FAILURE;
    }
    printf("\nAll tests passed\n");
    return EXIT_SUCCESS;
}
