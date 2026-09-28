#include "uat_phy.h"

#include <string.h>

/* SmartRF Studio 2.32.0 factual export parameters for CC1312R Rev E,
 * independently represented here (no generated TX command is carried into
 * this RX-only firmware). Requested 1.041667 Mbaud quantizes to
 * 1.042179870 Mbaud: +512.87 baud / +492 ppm. Deviation is exact 312.5 kHz;
 * selected RX filter bandwidth is 2.1851 MHz. */
static const uat_phy_profile_t k_profile = {
    .frequency_hz = 978000000u,
    .sync_adsb32 = 0xEACDDA4Eu,
    .sync_uplink32 = 0x153225B1u,
    .deviation_hz = 312500u,
    .symbol_rate_mbaud = 1042179870u,
    .rx_bw_hz = 2185100u,
    .rate_word = 0x0A6BFBu,
    .deviation_word = 0x04E2u,
    .rate_pre_scale = 0x0Fu,
    .rx_bw_code = 0x64u,
};

const uat_phy_profile_t *uat_phy_profile(void)
{
    return &k_profile;
}

bool uat_phy_classify(uint8_t sync_id, uint8_t sync_tail,
                      uint8_t mdb_type, uat_phy_class_t *out)
{
    uat_phy_class_t v;
    if (sync_id == 0) {
        if ((sync_tail & 0x0Fu) != 0x02u) return false;
        if ((mdb_type & 0x1Fu) == 0) {
            v.kind = RP_CC13XX_UAT_DOWNLINK_SHORT;
            v.frame_len = RP_CC13XX_UAT_SHORT_LEN;
        } else {
            v.kind = RP_CC13XX_UAT_DOWNLINK_LONG;
            v.frame_len = RP_CC13XX_UAT_LONG_LEN;
        }
    } else if (sync_id == 1) {
        if ((sync_tail & 0x0Fu) != 0x0Du) return false;
        v.kind = RP_CC13XX_UAT_UPLINK;
        v.frame_len = RP_CC13XX_UAT_UPLINK_LEN;
    } else {
        return false;
    }
    if (out) *out = v;
    return true;
}

bool uat_phy_validate_packet(uint8_t sync_id, uint8_t sync_tail,
                             const uint8_t *frame, uint16_t frame_len,
                             int8_t rssi_dbm, uint8_t rf_result,
                             uint32_t cc_time, bool set_len_ok,
                             uat_phy_packet_t *out)
{
    if (!frame || !out || rf_result != 0 || !set_len_ok) return false;
    uat_phy_class_t c;
    const uint8_t mdb = (uint8_t)(frame[0] >> 3);
    if (!uat_phy_classify(sync_id, sync_tail, mdb, &c) ||
        c.frame_len != frame_len) return false;
    uat_phy_packet_t v = {
        .frame = frame,
        .frame_len = frame_len,
        .rssi_dbm = rssi_dbm,
        .cc_time = cc_time,
        .kind = c.kind,
        .flags = RP_CC13XX_DESC_F_SYNC36_OK |
                 RP_CC13XX_DESC_F_RF_STATUS_OK |
                 RP_CC13XX_DESC_F_SET_LEN_OK,
    };
    *out = v;
    return true;
}

void uat_phy_health_init(uat_phy_health_t *h)
{
    memset(h, 0, sizeof(*h));
}

uat_phy_action_t uat_phy_on_command_status(uat_phy_health_t *h,
                                           uat_phy_cmd_status_t status,
                                           uint32_t now_ticks)
{
    if (!h) return UAT_PHY_ACTION_REINIT_RF;
    (void)now_ticks;
    switch (status) {
    case UAT_PHY_CMD_ACTIVE:
        return UAT_PHY_ACTION_NONE;
    case UAT_PHY_CMD_DONE_OK:
        h->command_restarts++;
        return UAT_PHY_ACTION_RESTART_RX;
    default:
        h->last_error = (uint16_t)status;
        h->rf_reinits++;
        return UAT_PHY_ACTION_REINIT_RF;
    }
}

uat_phy_action_t uat_phy_on_liveness_probe(uat_phy_health_t *h,
                                           bool probe_ok,
                                           uint32_t now_ticks,
                                           uint32_t ticks_per_second)
{
    if (!h) return UAT_PHY_ACTION_REINIT_RF;
    if (probe_ok) {
        h->last_progress = now_ticks;
        h->rssi_probe_successes++;
        return UAT_PHY_ACTION_NONE;
    }
    h->rssi_probe_errors++;
    if (!ticks_per_second ||
        ticks_per_second > UINT32_MAX / UAT_PHY_STALL_SECONDS) {
        h->last_error = UAT_PHY_CMD_ERROR_OTHER;
        h->rf_reinits++;
        h->watchdog_reinits++;
        return UAT_PHY_ACTION_REINIT_RF;
    }
    const uint32_t stall_ticks =
        UAT_PHY_STALL_SECONDS * ticks_per_second;
    if ((uint32_t)(now_ticks - h->last_progress) < stall_ticks)
        return UAT_PHY_ACTION_NONE;
    h->last_progress = now_ticks;
    h->rf_reinits++;
    h->watchdog_reinits++;
    return UAT_PHY_ACTION_REINIT_RF;
}

void uat_phy_recovery_init(uat_phy_recovery_t *r)
{
    if (r) memset(r, 0, sizeof(*r));
}

uat_phy_recovery_action_t uat_phy_recovery_fault(uat_phy_recovery_t *r,
                                                 uint16_t raw_status,
                                                 uint32_t now_ticks)
{
    if (!r || r->state != UAT_PHY_RECOVERY_RUNNING)
        return UAT_PHY_RECOVERY_WAIT;
    r->last_raw_status = raw_status;
    r->started_ticks = now_ticks;
    r->cancel_attempts = 1;
    r->cancel_retry_pending = false;
    r->report_pending = true;
    r->state = UAT_PHY_RECOVERY_WAIT_TERMINATION;
    return UAT_PHY_RECOVERY_CANCEL;
}

uat_phy_recovery_action_t uat_phy_recovery_cancel_result(
    uat_phy_recovery_t *r, uat_phy_cancel_result_t result)
{
    if (!r || r->state != UAT_PHY_RECOVERY_WAIT_TERMINATION)
        return UAT_PHY_RECOVERY_WAIT;
    switch (result) {
    case UAT_PHY_CANCEL_ACCEPTED:
        return UAT_PHY_RECOVERY_WAIT;
    case UAT_PHY_CANCEL_ALREADY_ENDED:
        r->state = UAT_PHY_RECOVERY_REOPEN;
        return UAT_PHY_RECOVERY_CLOSE_REOPEN;
    case UAT_PHY_CANCEL_RETRYABLE:
        if (r->cancel_attempts >= UAT_PHY_CANCEL_MAX_ATTEMPTS) {
            r->state = UAT_PHY_RECOVERY_FAILED;
            return UAT_PHY_RECOVERY_HARD_RESET;
        }
        r->cancel_attempts++;
        r->cancel_retry_pending = true;
        return UAT_PHY_RECOVERY_WAIT;
    default:
        r->state = UAT_PHY_RECOVERY_FAILED;
        return UAT_PHY_RECOVERY_HARD_RESET;
    }
}

uat_phy_recovery_action_t uat_phy_recovery_step(uat_phy_recovery_t *r,
                                                bool command_terminated,
                                                uint32_t now_ticks,
                                                uint32_t ticks_per_second)
{
    if (!r) return UAT_PHY_RECOVERY_WAIT;
    if (r->state == UAT_PHY_RECOVERY_WAIT_TERMINATION) {
        if (command_terminated) {
            r->state = UAT_PHY_RECOVERY_REOPEN;
            return UAT_PHY_RECOVERY_CLOSE_REOPEN;
        }
        if (!ticks_per_second ||
            ticks_per_second > UINT32_MAX / UAT_PHY_RECOVERY_TIMEOUT_SECONDS ||
            (uint32_t)(now_ticks - r->started_ticks) >=
                UAT_PHY_RECOVERY_TIMEOUT_SECONDS * ticks_per_second) {
            r->state = UAT_PHY_RECOVERY_FAILED;
            return UAT_PHY_RECOVERY_HARD_RESET;
        }
        if (r->cancel_retry_pending) {
            r->cancel_retry_pending = false;
            return UAT_PHY_RECOVERY_CANCEL;
        }
        return UAT_PHY_RECOVERY_WAIT;
    }
    return UAT_PHY_RECOVERY_NONE;
}

void uat_phy_recovery_reopened(uat_phy_recovery_t *r, bool success)
{
    if (!r) return;
    if (success) r->state = UAT_PHY_RECOVERY_RUNNING;
}
