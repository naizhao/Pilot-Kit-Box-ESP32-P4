#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rp_cc13xx_codec.h"

/* RF command liveness is probed independently of packet arrival.  Five
 * seconds without a successful read-only probe is a local RF fault; an empty
 * 978 MHz channel is not.  The caller supplies its actual ClockP tick rate. */
#define UAT_PHY_STALL_SECONDS 5u
#define UAT_PHY_RECOVERY_TIMEOUT_SECONDS 3u
#define UAT_PHY_CANCEL_MAX_ATTEMPTS 3u

typedef struct {
    uint32_t frequency_hz;
    uint32_t sync_adsb32;
    uint32_t sync_uplink32;
    uint32_t deviation_hz;
    uint32_t symbol_rate_mbaud;
    uint32_t rx_bw_hz;
    uint32_t rate_word;
    uint16_t deviation_word;
    uint8_t rate_pre_scale;
    uint8_t rx_bw_code;
} uat_phy_profile_t;

const uat_phy_profile_t *uat_phy_profile(void);

typedef struct {
    uint8_t kind;
    uint16_t frame_len;
} uat_phy_class_t;

/* sync_id: 0=0xEACDDA4E ADS-B, 1=0x153225B1 uplink (the high 32
 * bits of the over-air sync). sync_tail is the following four header bits;
 * 2/D reconstruct the complete 36-bit sync EACDDA4E2/153225B1D. */
bool uat_phy_classify(uint8_t sync_id, uint8_t sync_tail,
                      uint8_t mdb_type, uat_phy_class_t *out);

typedef struct {
    const uint8_t *frame;
    uint16_t frame_len;
    int8_t rssi_dbm;
    uint32_t cc_time;
    uint8_t kind;
    uint8_t flags;
} uat_phy_packet_t;

bool uat_phy_validate_packet(uint8_t sync_id, uint8_t sync_tail,
                             const uint8_t *frame, uint16_t frame_len,
                             int8_t rssi_dbm, uint8_t rf_result,
                             uint32_t cc_time, bool set_len_ok,
                             uat_phy_packet_t *out);

/* 与 TI PROP status 类别解耦的可测试恢复裁决。目标胶水负责映射 RF Core
 * status 并执行 restart/re-open；这里不隐藏任何错误。 */
typedef enum {
    UAT_PHY_CMD_ACTIVE = 0,
    UAT_PHY_CMD_DONE_OK,
    UAT_PHY_CMD_ERROR_RXFULL,
    UAT_PHY_CMD_ERROR_RXBUF,
    UAT_PHY_CMD_ERROR_RXOVF,
    UAT_PHY_CMD_ERROR_OTHER,
} uat_phy_cmd_status_t;

typedef enum {
    UAT_PHY_ACTION_NONE = 0,
    UAT_PHY_ACTION_RESTART_RX,
    UAT_PHY_ACTION_REINIT_RF,
} uat_phy_action_t;

typedef struct {
    uint32_t last_progress;
    uint32_t packets_ok;
    uint32_t packets_rejected;
    uint32_t set_len_errors;
    uint32_t command_restarts;
    uint32_t rf_reinits;
    uint32_t watchdog_reinits;
    uint32_t rssi_probe_errors;
    uint32_t rssi_probe_successes;
    uint16_t last_error;
} uat_phy_health_t;

typedef enum {
    UAT_PHY_RECOVERY_RUNNING = 0,
    UAT_PHY_RECOVERY_WAIT_TERMINATION,
    UAT_PHY_RECOVERY_REOPEN,
    UAT_PHY_RECOVERY_FAILED,
} uat_phy_recovery_state_t;

typedef enum {
    UAT_PHY_RECOVERY_NONE = 0,
    UAT_PHY_RECOVERY_CANCEL,
    UAT_PHY_RECOVERY_WAIT,
    UAT_PHY_RECOVERY_CLOSE_REOPEN,
    UAT_PHY_RECOVERY_HARD_RESET,
} uat_phy_recovery_action_t;

typedef enum {
    UAT_PHY_CANCEL_ACCEPTED = 0,
    UAT_PHY_CANCEL_ALREADY_ENDED,
    UAT_PHY_CANCEL_RETRYABLE,
    UAT_PHY_CANCEL_FATAL,
} uat_phy_cancel_result_t;

typedef struct {
    uat_phy_recovery_state_t state;
    uint16_t last_raw_status;
    uint32_t started_ticks;
    uint8_t cancel_attempts;
    bool cancel_retry_pending;
    bool report_pending;
} uat_phy_recovery_t;

void uat_phy_recovery_init(uat_phy_recovery_t *r);
uat_phy_recovery_action_t uat_phy_recovery_fault(uat_phy_recovery_t *r,
                                                 uint16_t raw_status,
                                                 uint32_t now_ticks);
uat_phy_recovery_action_t uat_phy_recovery_cancel_result(
    uat_phy_recovery_t *r, uat_phy_cancel_result_t result);
uat_phy_recovery_action_t uat_phy_recovery_step(uat_phy_recovery_t *r,
                                                bool command_terminated,
                                                uint32_t now_ticks,
                                                uint32_t ticks_per_second);
void uat_phy_recovery_reopened(uat_phy_recovery_t *r, bool success);

void uat_phy_health_init(uat_phy_health_t *h);
uat_phy_action_t uat_phy_on_command_status(uat_phy_health_t *h,
                                           uat_phy_cmd_status_t status,
                                           uint32_t now_ticks);
uat_phy_action_t uat_phy_on_liveness_probe(uat_phy_health_t *h,
                                           bool probe_ok,
                                           uint32_t now_ticks,
                                           uint32_t ticks_per_second);
