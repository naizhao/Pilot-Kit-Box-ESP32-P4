/*
 * test_uat_phy.c — CC1312R 978 MHz RX-only PHY 纯逻辑合同。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/cc1312r -I firmware/components/rp_cc13xx_codec \
 *      -I firmware/components/adsb_link_codec \
 *      -o /tmp/test_uat_phy firmware/test/test_uat_phy.c \
 *      firmware/cc1312r/uat_phy.c \
 *      firmware/components/rp_cc13xx_codec/rp_cc13xx_codec.c \
 *      firmware/components/adsb_link_codec/adsb_link_codec.c \
 *   && /tmp/test_uat_phy
 */
#include "uat_phy.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); \
    printf(" @%s:%d\n", __FILE__, __LINE__); fails++; } } while (0)

static void test_profile(void)
{
    const uat_phy_profile_t *p = uat_phy_profile();
    CHECK(p->frequency_hz == 978000000u, "frequency=%u", p->frequency_hz);
    CHECK(p->sync_adsb32 == 0xEACDDA4Eu &&
          p->sync_uplink32 == 0x153225B1u,
          "RF Core high-32 sync words");
    CHECK((((uint64_t)p->sync_adsb32 << 4) | 0x02u) ==
              UINT64_C(0xEACDDA4E2) &&
          (((uint64_t)p->sync_uplink32 << 4) | 0x0Du) ==
              UINT64_C(0x153225B1D),
          "32-bit correlator + 4-bit header did not rebuild 36-bit sync");
    CHECK(p->deviation_hz == 312500u, "deviation=%u", p->deviation_hz);
    CHECK(p->symbol_rate_mbaud == 1042179870u,
          "symbol rate milli-baud=%u", p->symbol_rate_mbaud);
    CHECK(p->rx_bw_hz == 2185100u, "bandwidth=%u", p->rx_bw_hz);
    CHECK(p->rate_pre_scale == 0x0f && p->rate_word == 0x0A6BFBu &&
          p->deviation_word == 0x04E2u && p->rx_bw_code == 0x64,
          "SmartRF quantized words changed");
}

static void test_sync_and_length(void)
{
    uat_phy_class_t c;
    CHECK(uat_phy_classify(0, 0x02, 0x00, &c) &&
          c.kind == RP_CC13XX_UAT_DOWNLINK_SHORT && c.frame_len == 30,
          "ADSB short classify");
    CHECK(uat_phy_classify(0, 0x02, 0x01, &c) &&
          c.kind == RP_CC13XX_UAT_DOWNLINK_LONG && c.frame_len == 48,
          "ADSB long classify");
    CHECK(uat_phy_classify(0, 0x02, 0x1f, &c) && c.frame_len == 48,
          "ADSB type31 long classify");
    CHECK(uat_phy_classify(1, 0x0d, 0x00, &c) &&
          c.kind == RP_CC13XX_UAT_UPLINK && c.frame_len == 552,
          "uplink classify");
    CHECK(!uat_phy_classify(0, 0x03, 0, &c), "bad ADSB sync tail accepted");
    CHECK(!uat_phy_classify(1, 0x0c, 0, &c), "bad uplink sync tail accepted");
    CHECK(!uat_phy_classify(2, 0x02, 0, &c), "unknown sync id accepted");
}

static void test_completed_packet_gate(void)
{
    uint8_t f[552];
    for (size_t i = 0; i < sizeof f; i++) f[i] = (uint8_t)(i * 5u + 1u);
    uat_phy_packet_t out;
    CHECK(uat_phy_validate_packet(0, 0x02, f, 30, -92, 0, 0xFFFFFFFEu,
                                  true, &out), "valid short rejected");
    CHECK(out.kind == RP_CC13XX_UAT_DOWNLINK_SHORT && out.frame == f &&
          out.frame_len == 30 && out.rssi_dbm == -92 &&
          out.cc_time == 0xFFFFFFFEu, "short metadata");
    CHECK(!uat_phy_validate_packet(0, 0x02, f, 48, -80, 0, 1, true, &out),
          "sync/MDB short presented as 48 accepted");
    f[0] = 0x08; /* mdb_type=1 */
    CHECK(uat_phy_validate_packet(0, 0x02, f, 48, -80, 0, 1, true, &out) &&
          out.kind == RP_CC13XX_UAT_DOWNLINK_LONG, "valid long rejected");
    CHECK(!uat_phy_validate_packet(0, 0x02, f, 48, -80, 3, 1, true, &out),
          "aborted RF status accepted");
    CHECK(!uat_phy_validate_packet(0, 0x02, f, 48, -80, 0, 1, false, &out),
          "SET_LEN failure accepted");
    CHECK(!uat_phy_validate_packet(1, 0x0d, f, 551, -70, 0, 2, true, &out),
          "truncated uplink accepted");
}

static void test_restart_policy(void)
{
    uat_phy_health_t h;
    uat_phy_health_init(&h);
    CHECK(uat_phy_on_command_status(&h, UAT_PHY_CMD_ACTIVE, 1000) ==
          UAT_PHY_ACTION_NONE, "active restarted");
    CHECK(uat_phy_on_command_status(&h, UAT_PHY_CMD_DONE_OK, 2000) ==
          UAT_PHY_ACTION_RESTART_RX, "done did not restart");
    CHECK(uat_phy_on_command_status(&h, UAT_PHY_CMD_ERROR_RXFULL, 3000) ==
          UAT_PHY_ACTION_REINIT_RF, "RXFULL did not reinit");
    CHECK(h.command_restarts == 1 && h.rf_reinits == 1 && h.last_error ==
          UAT_PHY_CMD_ERROR_RXFULL, "health counters");
    /* ACTIVE status alone is not liveness: a wedged RF Core can leave the
     * operation status looking active forever.  A read-only RSSI command is
     * the target-side probe.  Quiet spectrum is healthy when that command
     * succeeds, irrespective of whether any UAT packet arrived. */
    h.last_progress = 3000;
    CHECK(uat_phy_on_liveness_probe(&h, true, 4000, 1000) ==
          UAT_PHY_ACTION_NONE, "successful quiet-spectrum probe restarted");
    CHECK(h.last_progress == 4000 && h.rssi_probe_errors == 0,
          "successful probe did not publish progress");
    CHECK(uat_phy_on_command_status(&h, UAT_PHY_CMD_ACTIVE, 8500) ==
          UAT_PHY_ACTION_NONE && h.last_progress == 4000,
          "ACTIVE status incorrectly refreshed RF liveness");
    CHECK(uat_phy_on_liveness_probe(&h, false, 8999, 1000) ==
          UAT_PHY_ACTION_NONE, "early failed probes restarted");
    CHECK(uat_phy_on_liveness_probe(&h, false, 9000, 1000) ==
          UAT_PHY_ACTION_REINIT_RF, "five-second probe stall not reinitialized");
    CHECK(h.rssi_probe_errors == 2 && h.watchdog_reinits == 1,
          "probe/watchdog counters errors=%u reinits=%u",
          h.rssi_probe_errors, h.watchdog_reinits);
}

static void test_nonblocking_recovery_sequence(void)
{
    uat_phy_recovery_t recovery;
    uat_phy_recovery_init(&recovery);

    CHECK(uat_phy_recovery_fault(&recovery, 0x3802, 1000) ==
          UAT_PHY_RECOVERY_CANCEL, "terminal fault did not request cancel");
    CHECK(recovery.last_raw_status == 0x3802 && recovery.report_pending,
          "terminal status was not retained for one diagnostic report");
    uat_phy_recovery_cancel_result(&recovery, UAT_PHY_CANCEL_ACCEPTED);
    CHECK(uat_phy_recovery_step(&recovery, false, 2999, 1000) ==
          UAT_PHY_RECOVERY_WAIT, "close attempted before termination callback");
    CHECK(uat_phy_recovery_step(&recovery, true, 3000, 1000) ==
          UAT_PHY_RECOVERY_CLOSE_REOPEN,
          "terminated command did not advance to close/reopen");
    uat_phy_recovery_reopened(&recovery, true);
    CHECK(uat_phy_recovery_step(&recovery, false, 3000, 1000) ==
          UAT_PHY_RECOVERY_NONE &&
          recovery.state == UAT_PHY_RECOVERY_RUNNING,
          "successful reopen did not return to continuous RX");

    /* A second fault must be observable and recoverable as well; a permanent
     * one-shot latch would leave report_pending false or state non-running. */
    CHECK(uat_phy_recovery_fault(&recovery, 0x3803, 4000) ==
          UAT_PHY_RECOVERY_CANCEL && recovery.report_pending,
          "second terminal fault was permanently latched");
}

static void test_cancel_busy_is_bounded(void)
{
    uat_phy_recovery_t recovery;
    uat_phy_recovery_init(&recovery);
    CHECK(uat_phy_recovery_fault(&recovery, 0x3804, 100) ==
          UAT_PHY_RECOVERY_CANCEL, "fault did not start first cancel");
    CHECK(uat_phy_recovery_cancel_result(&recovery,
          UAT_PHY_CANCEL_RETRYABLE) == UAT_PHY_RECOVERY_WAIT,
          "first busy result was not deferred");
    CHECK(uat_phy_recovery_step(&recovery, false, 101, 1000) ==
          UAT_PHY_RECOVERY_CANCEL, "first bounded retry not requested");
    CHECK(uat_phy_recovery_cancel_result(&recovery,
          UAT_PHY_CANCEL_RETRYABLE) == UAT_PHY_RECOVERY_WAIT,
          "second busy result was not deferred");
    CHECK(uat_phy_recovery_step(&recovery, false, 102, 1000) ==
          UAT_PHY_RECOVERY_CANCEL, "second bounded retry not requested");
    CHECK(uat_phy_recovery_cancel_result(&recovery,
          UAT_PHY_CANCEL_RETRYABLE) == UAT_PHY_RECOVERY_HARD_RESET,
          "third busy result did not leave recovery via hard reset");
}

static void test_missing_termination_callback_times_out(void)
{
    uat_phy_recovery_t recovery;
    uat_phy_recovery_init(&recovery);
    CHECK(uat_phy_recovery_fault(&recovery, 0x3805, 5000) ==
          UAT_PHY_RECOVERY_CANCEL, "fault did not request cancel");
    CHECK(uat_phy_recovery_cancel_result(&recovery,
          UAT_PHY_CANCEL_ACCEPTED) == UAT_PHY_RECOVERY_WAIT,
          "accepted cancel did not await callback");
    CHECK(uat_phy_recovery_step(&recovery, false, 7999, 1000) ==
          UAT_PHY_RECOVERY_WAIT, "callback timeout fired early");
    CHECK(uat_phy_recovery_step(&recovery, false, 8000, 1000) ==
          UAT_PHY_RECOVERY_HARD_RESET,
          "missing callback left receiver permanently stuck");
}

int main(void)
{
    test_profile();
    test_sync_and_length();
    test_completed_packet_gate();
    test_restart_policy();
    test_nonblocking_recovery_sequence();
    test_cancel_busy_is_bounded();
    test_missing_termination_callback_times_out();
    if (fails) return 1;
    puts("PASS: UAT PHY profile/classifier/recovery");
    return 0;
}
