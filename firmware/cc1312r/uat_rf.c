#include "uat_rf.h"

#include <limits.h>
#include <string.h>

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/rf_data_entry.h)
#include DeviceFamily_constructPath(driverlib/rf_common_cmd.h)
#include DeviceFamily_constructPath(driverlib/rf_mailbox.h)
#include DeviceFamily_constructPath(driverlib/rf_prop_cmd.h)
#include DeviceFamily_constructPath(driverlib/rf_prop_mailbox.h)

#include <ti/drivers/rf/RF.h>
#include <ti/drivers/dpl/ClockP.h>

#include "uat_radio_config.h"

#define UAT_ENTRY_COUNT        2u
#define UAT_APPEND_BYTES       6u /* RSSI + RAT timestamp + PROP status */
#define UAT_HEADER_BYTES       1u /* stored 4-bit sync tail */
#define UAT_LENGTH_BYTES       2u
#define UAT_ENTRY_HEADROOM     16u
#define UAT_PAYLOAD_CAPACITY   (RP_CC13XX_UAT_MAX_LEN + UAT_ENTRY_HEADROOM)
#define UAT_ENTRY_RAW_SIZE     (sizeof(rfc_dataEntryPartial_t) + \
                                UAT_LENGTH_BYTES + UAT_HEADER_BYTES + \
                                UAT_PAYLOAD_CAPACITY + UAT_APPEND_BYTES)
#define UAT_ENTRY_STRIDE       ((UAT_ENTRY_RAW_SIZE + 3u) & ~3u)
#define UAT_RF_PROBE_INTERVAL_SECONDS 1u
#define UAT_RF_PROBE_FAULT_STATUS     0xFFF0u

static RF_Object g_rf_object;
static RF_Handle g_rf_handle;
static RF_CmdHandle g_rx_handle = RF_ALLOC_ERROR;
static dataQueue_t g_queue;
static rfc_propRxOutput_t g_rx_output;
static uint8_t g_entry_memory[UAT_ENTRY_COUNT * UAT_ENTRY_STRIDE]
    __attribute__((aligned(4)));
static rfc_dataEntryPartial_t *g_entries[UAT_ENTRY_COUNT];
static rfc_dataEntryPartial_t *g_read_entry;
static volatile int16_t g_requested_len = -1;
static volatile bool g_set_len_ok;
static uat_rf_frame_cb_t g_callback;
static void *g_callback_user;
static uat_phy_health_t g_health;
static uat_phy_recovery_t g_recovery;
static volatile bool g_chain_terminated;
static uint32_t g_last_probe_tick;
static uint32_t g_health_generation;
static int8_t g_idle_rssi_dbm;
static bool g_idle_rssi_valid;
static bool g_last_probe_ok;
static bool g_reset_required;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void queue_init(void)
{
    memset(g_entry_memory, 0, sizeof(g_entry_memory));
    memset(&g_rx_output, 0, sizeof(g_rx_output));
    g_requested_len = -1;
    g_set_len_ok = false;
    for (unsigned i = 0; i < UAT_ENTRY_COUNT; ++i) {
        g_entries[i] = (rfc_dataEntryPartial_t *)
            &g_entry_memory[i * UAT_ENTRY_STRIDE];
    }
    for (unsigned i = 0; i < UAT_ENTRY_COUNT; ++i) {
        rfc_dataEntryPartial_t *e = g_entries[i];
        e->pNextEntry = (uint8_t *)g_entries[(i + 1u) % UAT_ENTRY_COUNT];
        e->status = DATA_ENTRY_PENDING;
        e->config.type = DATA_ENTRY_TYPE_PARTIAL;
        e->config.lenSz = UAT_LENGTH_BYTES;
        e->config.irqIntv = 0; /* RF_EventNDataWritten every 16 bytes */
        e->length = (uint16_t)(sizeof(e->pktStatus) + sizeof(e->nextIndex) +
                               UAT_LENGTH_BYTES + UAT_HEADER_BYTES +
                               UAT_PAYLOAD_CAPACITY + UAT_APPEND_BYTES);
    }
    g_queue.pCurrEntry = (uint8_t *)g_entries[0];
    g_queue.pLastEntry = NULL;
    g_read_entry = g_entries[0];
}

static void queue_advance(void)
{
    g_read_entry->status = DATA_ENTRY_PENDING;
    g_read_entry = (rfc_dataEntryPartial_t *)g_read_entry->pNextEntry;
    g_queue.pCurrEntry = (uint8_t *)g_read_entry;
}

static void reject_packet(void)
{
    g_health.packets_rejected++;
}

static void deliver_finished_entry(rfc_dataEntryPartial_t *entry)
{
    uint8_t *raw = &entry->rxData;
    const uint16_t stored = (uint16_t)raw[0] | ((uint16_t)raw[1] << 8);
    if (stored < UAT_HEADER_BYTES + UAT_APPEND_BYTES) {
        reject_packet();
        return;
    }
    const uint16_t frame_len =
        (uint16_t)(stored - UAT_HEADER_BYTES - UAT_APPEND_BYTES);
    if (frame_len > RP_CC13XX_UAT_MAX_LEN) {
        reject_packet();
        return;
    }

    const uint8_t sync_tail = (uint8_t)(raw[UAT_LENGTH_BYTES] & 0x0Fu);
    const uint8_t *frame = raw + UAT_LENGTH_BYTES + UAT_HEADER_BYTES;
    const uint8_t *appended = frame + frame_len;
    const int8_t rssi_dbm = (int8_t)appended[0];
    const uint32_t cc_time = le32(appended + 1);
    const rfc_propRxStatus_t *status =
        (const rfc_propRxStatus_t *)(appended + 5);
    uat_phy_packet_t packet;
    if ((g_requested_len < 0) ||
        (uint16_t)g_requested_len != frame_len ||
        !uat_phy_validate_packet(status->status.syncWordId, sync_tail,
                                 frame, frame_len, rssi_dbm,
                                 status->status.result, cc_time,
                                 g_set_len_ok, &packet)) {
        reject_packet();
        return;
    }
    g_health.packets_ok++;
    if (g_callback) g_callback(&packet, g_callback_user);
}

static void choose_packet_length(RF_Handle handle)
{
    if (g_requested_len >= 0) return;
    const uint8_t *raw = &g_read_entry->rxData;
    const uint8_t sync_tail = (uint8_t)(raw[UAT_LENGTH_BYTES] & 0x0Fu);
    const uint8_t mdb_type =
        (uint8_t)(raw[UAT_LENGTH_BYTES + UAT_HEADER_BYTES] >> 3);
    uat_phy_class_t packet_class;
    rfc_CMD_PROP_SET_LEN_t set_len = {.commandNo = CMD_PROP_SET_LEN};
    /* The primary/alternate discriminator is only appended at completion.
     * The mutually-exclusive 36-bit tail (2/D) identifies it at this point;
     * the appended syncWordId is independently checked before delivery. */
    const uint8_t inferred_sync = sync_tail == 0x0Du ? 1u : 0u;
    if (!uat_phy_classify(inferred_sync, sync_tail, mdb_type,
                          &packet_class)) {
        g_requested_len = 0;
        g_set_len_ok = false;
        reject_packet();
        return;
    }
    set_len.rxLen = packet_class.frame_len;
    g_requested_len = (int16_t)packet_class.frame_len;
    g_set_len_ok =
        RF_runImmediateCmd(handle, (uint32_t *)&set_len) ==
        RF_StatCmdDoneSuccess;
    if (!g_set_len_ok) g_health.set_len_errors++;
}

static void rf_callback(RF_Handle handle, RF_CmdHandle command,
                        RF_EventMask events)
{
    (void)command;
    if (events & RF_EventMdmSoft) {
        g_requested_len = -1;
        g_set_len_ok = false;
    }
    if (events & RF_EventNDataWritten) choose_packet_length(handle);
    if (events & RF_EventRxEntryDone) {
        if (g_read_entry->status == DATA_ENTRY_FINISHED) {
            deliver_finished_entry(g_read_entry);
            queue_advance();
        } else {
            reject_packet();
        }
        g_requested_len = -1;
        g_set_len_ok = false;
    }
    if (events & (RF_EventRxBufFull | RF_EventInternalError |
                  RF_EventError)) {
        g_health.last_error = (uint16_t)events;
    }
    if (events & (RF_EventLastCmdDone | RF_EventCmdCancelled |
                  RF_EventCmdAborted | RF_EventCmdStopped)) {
        g_chain_terminated = true;
    }
}

static bool post_rx(void)
{
    g_uat_rx.status = IDLE;
    g_rx_handle = RF_postCmd(g_rf_handle, (RF_Op *)&g_uat_rx,
                             RF_PriorityNormal, rf_callback,
                             RF_EventMdmSoft | RF_EventNDataWritten |
                             RF_EventRxEntryDone | RF_EventRxOk |
                             RF_EventRxNOk | RF_EventRxBufFull |
                             RF_EventLastCmdDone | RF_EventCmdCancelled |
                             RF_EventCmdAborted | RF_EventCmdStopped |
                             RF_EventInternalError | RF_EventError);
    return g_rx_handle >= 0;
}

static void cancel_and_close(void)
{
    if (!g_rf_handle) return;
    /* RF_close explicitly waits for every queued command.  RX_ADV has a
     * TRIG_NEVER end condition, so closing it directly from an error path
     * blocks the main loop until the hardware watchdog resets the MCU. */
    if (g_rx_handle >= 0) {
        (void)RF_cancelCmd(g_rf_handle, g_rx_handle, 0);
    }
    RF_close(g_rf_handle);
    g_rf_handle = NULL;
    g_rx_handle = RF_ALLOC_ERROR;
}

static bool rf_open_and_tune(void)
{
    RF_Params params;
    RF_Params_init(&params);
    params.nInactivityTimeout = 0;
    params.pErrCb = rf_callback;
    g_uat_radio_setup.status = IDLE;
    g_uat_fs.status = IDLE;
    g_rf_handle = RF_open(&g_rf_object, &g_uat_rf_mode,
                          (RF_RadioSetup *)&g_uat_radio_setup, &params);
    if (!g_rf_handle) return false;
    /* Submit FS and RX as one RF Core chain.  A synchronous RF_runCmd(FS)
     * blocked the bare-metal SSI loop indefinitely on real CC1312R hardware;
     * a separately posted FS followed by main-loop polling could likewise
     * starve the pipelined SPI HELLO.  Chaining is the RF Driver's native
     * ordering primitive: RX cannot start before FS has completed, while the
     * MCU immediately returns to service the RP2040 link. */
    g_uat_fs.status = IDLE;
    g_uat_rx.status = IDLE;
    g_uat_fs.pNextOp = (rfc_radioOp_t *)&g_uat_rx;
    g_rx_handle = RF_postCmd(g_rf_handle, (RF_Op *)&g_uat_fs,
                             RF_PriorityNormal, rf_callback,
                             RF_EventMdmSoft | RF_EventNDataWritten |
                             RF_EventRxEntryDone | RF_EventRxOk |
                             RF_EventRxNOk | RF_EventRxBufFull |
                             RF_EventLastCmdDone | RF_EventCmdCancelled |
                             RF_EventCmdAborted | RF_EventCmdStopped |
                             RF_EventInternalError | RF_EventError);
    if (g_rx_handle < 0) {
        RF_close(g_rf_handle);
        g_rf_handle = NULL;
        return false;
    }
    const uint32_t now = ClockP_getSystemTicks();
    g_health.last_progress = now;
    g_last_probe_tick = now;
    g_chain_terminated = false;
    return true;
}

bool uat_rf_start(uat_rf_frame_cb_t callback, void *user)
{
    g_callback = callback;
    g_callback_user = user;
    uat_phy_health_init(&g_health);
    uat_phy_recovery_init(&g_recovery);
    g_chain_terminated = false;
    g_last_probe_tick = 0;
    g_health_generation = 1;
    g_idle_rssi_dbm = RF_GET_RSSI_ERROR_VAL;
    g_idle_rssi_valid = false;
    g_last_probe_ok = false;
    g_reset_required = false;
    queue_init();
    g_uat_rx.pQueue = &g_queue;
    g_uat_rx.pOutput = (uint8_t *)&g_rx_output;
    return rf_open_and_tune();
}

static uat_phy_cancel_result_t map_cancel_result(RF_Stat status)
{
    switch (status) {
    case RF_StatSuccess:
    case RF_StatCmdDoneSuccess:
    case RF_StatCmdSch:
        return UAT_PHY_CANCEL_ACCEPTED;
    case RF_StatCmdEnded:
        return UAT_PHY_CANCEL_ALREADY_ENDED;
    case RF_StatBusyError:
        return UAT_PHY_CANCEL_RETRYABLE;
    case RF_StatRadioInactiveError:
    case RF_StatCmdDoneError:
    case RF_StatInvalidParamsError:
    case RF_StatError:
    default:
        return UAT_PHY_CANCEL_FATAL;
    }
}

static uat_phy_recovery_action_t cancel_rx_once(void)
{
    const RF_Stat status = RF_cancelCmd(g_rf_handle, g_rx_handle, 0);
    return uat_phy_recovery_cancel_result(&g_recovery,
                                          map_cancel_result(status));
}

static uint32_t ticks_per_second(void)
{
    const uint32_t period_us = ClockP_getSystemTickPeriod();
    return period_us ? 1000000u / period_us : 0u;
}

static uat_phy_cmd_status_t map_status(uint16_t status)
{
    if (status == ACTIVE || status == PENDING || status == IDLE)
        return UAT_PHY_CMD_ACTIVE;
    if ((status & 0xFF00u) == 0x3400u) return UAT_PHY_CMD_DONE_OK;
    if (status == PROP_ERROR_RXFULL) return UAT_PHY_CMD_ERROR_RXFULL;
    if (status == PROP_ERROR_RXBUF) return UAT_PHY_CMD_ERROR_RXBUF;
    if (status == PROP_ERROR_RXOVF) return UAT_PHY_CMD_ERROR_RXOVF;
    return UAT_PHY_CMD_ERROR_OTHER;
}

uint16_t uat_rf_poll(void)
{
    const uint32_t now = ClockP_getSystemTicks();
    const uint32_t tick_hz = ticks_per_second();
    if (g_recovery.state == UAT_PHY_RECOVERY_WAIT_TERMINATION) {
        uat_phy_recovery_action_t recovery_action =
            uat_phy_recovery_step(&g_recovery, g_chain_terminated,
                                  now, tick_hz);
        if (recovery_action == UAT_PHY_RECOVERY_CANCEL)
            recovery_action = cancel_rx_once();
        if (recovery_action == UAT_PHY_RECOVERY_HARD_RESET) {
            g_reset_required = true;
            g_health_generation++;
        }
        if (g_recovery.report_pending) {
            g_recovery.report_pending = false;
            return g_recovery.last_raw_status;
        }
        if (recovery_action != UAT_PHY_RECOVERY_CLOSE_REOPEN) return 0;
    }
    if (g_recovery.state == UAT_PHY_RECOVERY_REOPEN) {
        /* RF_close is only legal here after a subscribed termination callback
         * (or RF_StatCmdEnded) proved the infinite RX command left the queue.
         * This keeps close non-blocking in the SSI service loop. */
        if (g_rf_handle) {
            RF_close(g_rf_handle);
            g_rf_handle = NULL;
            g_rx_handle = RF_ALLOC_ERROR;
        }
        queue_init();
        const bool reopened = rf_open_and_tune();
        uat_phy_recovery_reopened(&g_recovery, reopened);
        g_health_generation++;
        return 0;
    }
    if (!g_rf_handle) {
        queue_init();
        if (rf_open_and_tune()) g_health_generation++;
        return 0;
    }
    const uint16_t status = ((volatile RF_Op *)&g_uat_rx)->status;
    uat_phy_action_t action = uat_phy_on_command_status(
        &g_health, map_status(status), now);
    uint16_t fault_status = status;
    if (action == UAT_PHY_ACTION_NONE) {
        const uint32_t probe_interval =
            tick_hz * UAT_RF_PROBE_INTERVAL_SECONDS;
        if (probe_interval &&
            (uint32_t)(now - g_last_probe_tick) >= probe_interval) {
            g_last_probe_tick = now;
            const int8_t rssi = RF_getRssi(g_rf_handle);
            const bool probe_ok = rssi != RF_GET_RSSI_ERROR_VAL;
            g_last_probe_ok = probe_ok;
            g_idle_rssi_valid = probe_ok;
            if (probe_ok) g_idle_rssi_dbm = rssi;
            action = uat_phy_on_liveness_probe(&g_health, probe_ok, now,
                                                tick_hz);
            g_health_generation++;
            if (action == UAT_PHY_ACTION_REINIT_RF) {
                fault_status = UAT_RF_PROBE_FAULT_STATUS;
            }
        }
        if (action == UAT_PHY_ACTION_NONE) return 0;
    }

    if (action == UAT_PHY_ACTION_RESTART_RX) {
        queue_init();
        if (!post_rx()) {
            g_health.last_error = 0xFFFFu;
            g_health.rf_reinits++;
            cancel_and_close();
        }
        return 0;
    }
    const uint16_t raw_status = fault_status;
    if (uat_phy_recovery_fault(&g_recovery, raw_status, now) ==
        UAT_PHY_RECOVERY_CANCEL) {
        g_chain_terminated = false;
        const uat_phy_recovery_action_t cancel_action = cancel_rx_once();
        if (cancel_action == UAT_PHY_RECOVERY_HARD_RESET)
            g_reset_required = true;
        g_health_generation++;
    }
    if (g_recovery.report_pending) {
        g_recovery.report_pending = false;
        return raw_status;
    }
    return 0;
}

bool uat_rf_reset_required(void)
{
    return g_reset_required;
}

const uat_phy_health_t *uat_rf_health(void)
{
    return &g_health;
}

void uat_rf_health_status(rp_cc13xx_rf_health_t *out, uint32_t *generation)
{
    if (out) {
        uint8_t flags = 0;
        if (g_rf_handle && g_recovery.state == UAT_PHY_RECOVERY_RUNNING)
            flags |= RP_CC13XX_RF_HEALTH_ACTIVE;
        if (g_last_probe_ok) flags |= RP_CC13XX_RF_HEALTH_PROBE_OK;
        if (g_idle_rssi_valid) flags |= RP_CC13XX_RF_HEALTH_RSSI_VALID;
        if (g_recovery.state != UAT_PHY_RECOVERY_RUNNING)
            flags |= RP_CC13XX_RF_HEALTH_RECOVERING;
        *out = (rp_cc13xx_rf_health_t) {
            .flags = flags,
            .idle_rssi_dbm = g_idle_rssi_dbm,
            .last_raw_status = g_recovery.last_raw_status,
            .probe_successes = g_health.rssi_probe_successes,
            .probe_errors = g_health.rssi_probe_errors,
            .rf_reinits = g_health.rf_reinits,
        };
    }
    if (generation) *generation = g_health_generation;
}
