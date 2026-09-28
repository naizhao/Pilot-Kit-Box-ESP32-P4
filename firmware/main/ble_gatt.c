/*
 * ble_gatt.c — NimBLE host + Pilot Kit GATT service + GDL90 emitter.
 *
 * Three pieces live in this file:
 *
 *   1. NimBLE bring-up — nimble_port_init() over ESP-Hosted's VHCI,
 *      callbacks for GAP advertising / connect / subscribe, and one service
 *      carrying the existing GDL90/time contract plus AIRBAND audio/control.
 *
 *   2. Subscription bookkeeping — each notify characteristic remembers
 *      whether the current peer has CCCD-enabled notifications, so the
 *      1 Hz emitter doesn't waste cycles encoding frames for unwilling
 *      clients.
 *
 *   3. GDL90 emitter task — once per second, snapshots aircraft_state
 *      and notifies Heartbeat + one Traffic Report per tracked plane.
 *      A separate FreeRTOS queue holds raw ts-line strings produced by
 *      record_sink_ble for delivery on the Raw characteristic.
 *
 *   4. AIRBAND sender — capture only copies 16 kHz program PCM into a bounded
 *      zero-wait queue. A lower-priority task encodes/fragments/notifies while
 *      the GDL90 scheduler has explicit priority.
 */

#include "ble_gatt.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_att.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "esp_hosted.h"               /* esp_hosted_connect_to_slave */
#include "esp_hosted_misc.h"          /* esp_hosted_bt_controller_init/enable */

#include "aircraft_state.h"
#include "airband_speaker.h"
#include "ble_owner_policy.h"
#include "config_devname.h"           /* 用户自定义的广播名前半段 */
#include "config_demo.h"   /* pk_demo_enabled —— 演示模式下停发 GDL90 目标 */
#include "gdl90.h"
#include "gps.h"
#include "own_ship.h"
#include "pk_clock.h"
#include "uat_ingest.h"

/* NimBLE's generated public headers do not declare the default key-store
 * initializer.  ESP-IDF's own NimBLE Security / bleprph examples declare it
 * here before installing the store callbacks. */
void ble_store_config_init(void);

static const char *TAG = "ble_gatt";

#define BLE_DEVICE_NAME_PREFIX  "Pilot Kit Box"
/* "Pilot Kit Box-AABBCC\0" is 21 bytes. Sized to fit any future
 * tweak of the prefix up to ~26 chars (the adv-packet hard limit). */
#define BLE_DEVICE_NAME_MAX     32
static char s_device_name[BLE_DEVICE_NAME_MAX] = BLE_DEVICE_NAME_PREFIX;
/* MAC 后三字节，on_sync() 拿到地址后填。空串 = 控制器还没同步过。
 * 单独存一份是为了 pk_ble_device_name_apply() 能在任何时候重拼名字——用户改
 * 名字发生在开机很久之后，那时 on_sync() 早就返回了，地址不会再送来一次。 */
static char s_addr_suffix[8];
#define BLE_RAW_QUEUE_DEPTH  64
#define BLE_RAW_LINE_MAX     80
#define BLE_EMIT_PERIOD_MS   1000
#define BLE_EMIT_STACK       6144
#define BLE_AUDIO_RAW_SAMPLES 64u
#define BLE_AUDIO_RAW_DEPTH   32u
#define BLE_AUDIO_STACK       4096
#define BLE_UAT_QUEUE_DEPTH    4

typedef struct {
    uint16_t count;
    uint32_t epoch;
    int64_t captured_us;
    int16_t pcm[BLE_AUDIO_RAW_SAMPLES];
} ble_audio_raw_t;

/* --- 128-bit UUIDs ---------------------------------------------------- *
 *
 * NimBLE stores 128-bit UUIDs in little-endian byte order: the first
 * array element is the LSB of the printable form. The four UUIDs below
 * derive from a single base whose lower 32 bits select the resource:
 *   1090AD5B-0000-1000-8000-1090AD5B<00..>
 */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0x5b, 0xad, 0x90, 0x10, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x5b, 0xad, 0x90, 0x10);

static const ble_uuid128_t s_chr_traffic_uuid = BLE_UUID128_INIT(
    0x01, 0x00, 0x5b, 0xad, 0x90, 0x10, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x5b, 0xad, 0x90, 0x10);

static const ble_uuid128_t s_chr_hb_uuid = BLE_UUID128_INIT(
    0x02, 0x00, 0x5b, 0xad, 0x90, 0x10, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x5b, 0xad, 0x90, 0x10);

static const ble_uuid128_t s_chr_raw_uuid = BLE_UUID128_INIT(
    0x03, 0x00, 0x5b, 0xad, 0x90, 0x10, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x5b, 0xad, 0x90, 0x10);

/* Time Sync characteristic: peer writes 8-byte little-endian Unix epoch
 * milliseconds, firmware calls settimeofday(). READ returns the current
 * firmware-side epoch_ms so clients can verify the write took effect. */
static const ble_uuid128_t s_chr_time_uuid = BLE_UUID128_INIT(
    0x04, 0x00, 0x5b, 0xad, 0x90, 0x10, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x5b, 0xad, 0x90, 0x10);

/* Versioned AIRBAND program audio + route/status control. Existing UUIDs
 * 01..04 remain byte-for-byte unchanged for deployed GDL90 clients. */
static const ble_uuid128_t s_chr_audio_uuid = BLE_UUID128_INIT(
    0x05, 0x00, 0x5b, 0xad, 0x90, 0x10, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x5b, 0xad, 0x90, 0x10);
static const ble_uuid128_t s_chr_audio_ctl_uuid = BLE_UUID128_INIT(
    0x06, 0x00, 0x5b, 0xad, 0x90, 0x10, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x5b, 0xad, 0x90, 0x10);

/* Standard Bluetooth SIG Current Time Service (CTS, UUID 0x1805) — iOS
 * exposes this as a GATT server on every connection. We act as a GATT
 * client toward it on connect to grab the phone's UTC clock, matching
 * what Apple's Accessory Design Guidelines recommend. Android phones
 * do not host CTS by default, in which case the discovery just times
 * out and the firmware falls back to waiting for an app-side write to
 * the custom Time Sync characteristic above. */
static const ble_uuid16_t s_cts_svc_uuid = BLE_UUID16_INIT(0x1805);
static const ble_uuid16_t s_cts_chr_uuid = BLE_UUID16_INIT(0x2A2B);

/* --- Runtime state ---------------------------------------------------- */

static uint16_t s_chr_traffic_handle;
static uint16_t s_chr_hb_handle;
static uint16_t s_chr_raw_handle;
static uint16_t s_chr_time_handle;
static uint16_t s_chr_audio_handle;
static uint16_t s_chr_audio_ctl_handle;

static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool     s_sub_traffic;
static bool     s_sub_hb;
static bool     s_sub_raw;
static atomic_bool s_sub_audio;
static atomic_bool s_audio_connected;
static atomic_bool s_audio_task_ready;
static atomic_ushort s_audio_mtu = 23;
static atomic_uchar s_audio_route = PK_AIRBAND_BLE_ROUTE_DEVICE;
static atomic_uchar s_audio_last_error;
static atomic_uint s_audio_epoch = 1;
static atomic_uint s_audio_queue_drops;
static portMUX_TYPE s_tx_sched_mux = portMUX_INITIALIZER_UNLOCKED;
static pk_ble_tx_scheduler_t s_tx_sched;
static portMUX_TYPE s_audio_diag_mux = portMUX_INITIALIZER_UNLOCKED;
static pk_ble_airband_status_t s_audio_diag;
static volatile bool s_advertising;   /* true while ble_gap_adv_start has been called
                                        * and no connect/stop has occurred yet */

static uint8_t       s_own_addr_type;
static QueueHandle_t s_raw_queue;   /* char[BLE_RAW_LINE_MAX] entries */
static QueueHandle_t s_audio_raw_queue;
static QueueHandle_t s_uat_queue;   /* uint8_t[432], GDL90 ID 7 pass-through */

#define BLE_OWNER_NVS_NS "pk_ble"
#define BLE_OWNER_NVS_KEY "air_owner"
#define BLE_OWNER_WINDOW_US 60000000LL
static portMUX_TYPE s_owner_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_owner_set;
static pk_ble_peer_id_t s_owner;
static int64_t s_owner_window_until_us;

static void owner_load(void)
{
    nvs_handle_t h;
    pk_ble_peer_id_t owner = {0};
    size_t len = sizeof(owner);
    bool valid = nvs_open(BLE_OWNER_NVS_NS, NVS_READONLY, &h) == ESP_OK;
    if (valid) {
        valid = nvs_get_blob(h, BLE_OWNER_NVS_KEY, &owner, &len) == ESP_OK &&
                len == sizeof(owner);
        nvs_close(h);
    }
    portENTER_CRITICAL(&s_owner_mux);
    s_owner_set = valid;
    if (valid) s_owner = owner;
    s_owner_window_until_us = 0;
    portEXIT_CRITICAL(&s_owner_mux);
    ESP_LOGI(TAG, "AIRBAND BLE owner: %s", valid ? "loaded" : "not enrolled");
}

static bool owner_save(const pk_ble_peer_id_t *owner)
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(BLE_OWNER_NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) err = nvs_set_blob(h, BLE_OWNER_NVS_KEY, owner, sizeof(*owner));
    if (err == ESP_OK) err = nvs_commit(h);
    if (h != 0) nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "AIRBAND BLE owner save failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

static bool owner_authorize_control(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) != 0) return false;
    const pk_ble_peer_id_t peer = {
        .type = desc.peer_id_addr.type,
        .value = {desc.peer_id_addr.val[0], desc.peer_id_addr.val[1],
                  desc.peer_id_addr.val[2], desc.peer_id_addr.val[3],
                  desc.peer_id_addr.val[4], desc.peer_id_addr.val[5]},
    };
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_owner_mux);
    const bool owner_set = s_owner_set;
    const pk_ble_peer_id_t owner = s_owner;
    const bool window = now < s_owner_window_until_us;
    portEXIT_CRITICAL(&s_owner_mux);
    const pk_ble_owner_decision_t decision = pk_ble_owner_decide(
        owner_set, &owner, &peer, desc.sec_state.encrypted,
        desc.sec_state.bonded, window);
    if (decision == PK_BLE_OWNER_DENY) return false;
    if (decision == PK_BLE_OWNER_CLAIM) {
        if (!owner_save(&peer)) return false;
        portENTER_CRITICAL(&s_owner_mux);
        s_owner = peer;
        s_owner_set = true;
        s_owner_window_until_us = 0;
        portEXIT_CRITICAL(&s_owner_mux);
        ESP_LOGI(TAG, "AIRBAND BLE owner enrolled by local authorization");
    }
    return true;
}

/* --- Forward declarations -------------------------------------------- */

static int  gap_event_cb(struct ble_gap_event *event, void *arg);
static int  chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);
static int  chr_time_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg);
static void start_advertising(void);
static void emitter_task(void *arg);
static void audio_emitter_task(void *arg);
static void time_sync_kickoff(uint16_t conn_handle);
static int chr_audio_ctl_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg);

/* --- GATT service definition ----------------------------------------- */

static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid       = &s_chr_traffic_uuid.u,
        .access_cb  = chr_access_cb,
        .flags      = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_chr_traffic_handle,
    },
    {
        .uuid       = &s_chr_hb_uuid.u,
        .access_cb  = chr_access_cb,
        .flags      = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_chr_hb_handle,
    },
    {
        .uuid       = &s_chr_raw_uuid.u,
        .access_cb  = chr_access_cb,
        .flags      = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_chr_raw_handle,
    },
    {
        .uuid       = &s_chr_time_uuid.u,
        .access_cb  = chr_time_access_cb,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
        .val_handle = &s_chr_time_handle,
    },
    {
        .uuid       = &s_chr_audio_uuid.u,
        .access_cb  = chr_access_cb,
        .flags      = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_chr_audio_handle,
    },
    {
        .uuid       = &s_chr_audio_ctl_uuid.u,
        .access_cb  = chr_audio_ctl_access_cb,
        /* This write changes a physical output: PHONE and MUTE both silence
         * the box speaker.  Keep status readable for diagnostics, but let
         * NimBLE reject plaintext writes before the callback can mutate the
         * route.  The first protected write causes the platform's standard
         * Just Works pairing flow. */
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                      BLE_GATT_CHR_F_WRITE_ENC,
        .val_handle = &s_chr_audio_ctl_handle,
    },
    { 0 },
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &s_svc_uuid.u,
        .characteristics = s_chrs,
    },
    { 0 },
};

/* --- GATT access callback (read/write) -------------------------------
 *
 * The three notification characteristics (Traffic / Heartbeat / Raw)
 * have no readable value; CCCD subscribe events are routed through
 * gap_event_cb (BLE_GAP_EVENT_SUBSCRIBE), not this callback. We reject
 * any stray read here.
 */
static int chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)ctxt; (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

/* --- Time Sync access (READ + WRITE) --------------------------------- *
 *
 * Wire format for both directions: an 8-byte little-endian uint64_t
 * carrying Unix epoch milliseconds (UTC). READ returns the firmware's
 * current best-known epoch. WRITE seeds the firmware clock via pk_clock,
 * which rejects anything older than 2024-01-01 UTC (so an accidental
 * zero-init buffer can't roll the clock backwards) and enforces GPS
 * priority over BLE-sourced time.
 */

static int chr_time_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        int64_t now_ms = (int64_t)tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;
        uint8_t le[8];
        for (int i = 0; i < 8; ++i) {
            le[i] = (uint8_t)((now_ms >> (8 * i)) & 0xFF);
        }
        return (os_mbuf_append(ctxt->om, le, sizeof(le)) == 0)
                   ? 0
                   : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t le[8];
        uint16_t got = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, le, sizeof(le), &got);
        if (rc != 0 || got != sizeof(le)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        int64_t epoch_ms = 0;
        for (int i = 0; i < 8; ++i) {
            epoch_ms |= (int64_t)le[i] << (8 * i);
        }
        if (!pk_clock_apply_epoch_ms(epoch_ms, "ble-write")) {
            /* Rejected: too old, or GPS time is currently authoritative. */
            return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        }
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static void audio_route_apply(pk_airband_ble_route_t route)
{
    const pk_airband_ble_route_t old =
        (pk_airband_ble_route_t)atomic_exchange(&s_audio_route, (unsigned char)route);
    if (old != route) atomic_fetch_add(&s_audio_epoch, 1);
    pk_airband_speaker_set_remote_muted(
        atomic_load(&s_audio_connected) &&
        (route == PK_AIRBAND_BLE_ROUTE_PHONE || route == PK_AIRBAND_BLE_ROUTE_MUTE));
}

/* Control wire v1 (16 bytes, LE): version, route, last_error, flags
 * (connected/subscribed/mtu-paused), mtu, stream, next-seq, sample-clock,
 * reserved. Writes are exactly {version, route}; malformed writes never alter
 * the previous route. */
static int chr_audio_ctl_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                   struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (!owner_authorize_control(conn_handle)) {
            ESP_LOGW(TAG, "AIRBAND control write denied: not enrolled owner or pairing window closed");
            return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
        }
        uint8_t value[2]; uint16_t got = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, value, sizeof(value), &got);
        if (rc != 0 || got != sizeof(value)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (value[0] != PK_AIRBAND_BLE_PROTOCOL_VERSION ||
            value[1] > PK_AIRBAND_BLE_ROUTE_MUTE) {
            atomic_store(&s_audio_last_error, 1);
            return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        }
        if (value[1] == PK_AIRBAND_BLE_ROUTE_PHONE &&
            (!atomic_load(&s_audio_task_ready) ||
             !atomic_load(&s_audio_connected) || !atomic_load(&s_sub_audio) ||
             pk_airband_ble_fragment_count(PK_AIRBAND_BLE_FRAME_BYTES,
                                            atomic_load(&s_audio_mtu)) == 0)) {
            atomic_store(&s_audio_last_error, 2);
            return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        }
        atomic_store(&s_audio_last_error, 0);
        audio_route_apply((pk_airband_ble_route_t)value[1]);
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        pk_ble_airband_status_t d;
        (void)ble_gatt_airband_status(&d);
        uint8_t value[16] = {
            PK_AIRBAND_BLE_PROTOCOL_VERSION, (uint8_t)d.route, d.last_error,
            (uint8_t)((d.connected ? 1u : 0u) | (d.subscribed ? 2u : 0u) |
                      (d.mtu_paused ? 4u : 0u)),
            (uint8_t)d.mtu, (uint8_t)(d.mtu >> 8),
            (uint8_t)d.stream_id, (uint8_t)(d.stream_id >> 8),
            (uint8_t)d.frame_seq, (uint8_t)(d.frame_seq >> 8),
            (uint8_t)d.sample_clock, (uint8_t)(d.sample_clock >> 8),
            (uint8_t)(d.sample_clock >> 16), (uint8_t)(d.sample_clock >> 24),
            0, 0
        };
        return os_mbuf_append(ctxt->om, value, sizeof(value)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* --- CTS GATT client (post-connect, iOS auto-sync) ------------------- *
 *
 * On every BLE_GAP_EVENT_CONNECT we kick off:
 *   1. ble_gattc_disc_svc_by_uuid(0x1805)
 *   2. on hit, ble_gattc_disc_chrs_by_uuid(0x2A2B) inside the service
 *   3. on hit, ble_gattc_read(value_handle)
 *   4. parse the 10-byte CTS payload as UTC, settimeofday()
 *
 * Android phones don't expose CTS as a server out of the box; in that
 * case stage 1 fires once with status=BLE_HS_EDONE and no service hit,
 * we silently give up, and the firmware waits for an app-side write to
 * the Time Sync characteristic instead. */

static uint16_t s_cts_chr_val_handle;

static int cts_on_read(uint16_t conn_handle,
                       const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)arg;
    if (error == NULL || error->status != 0) {
        ESP_LOGD(TAG, "CTS read failed (status=%d) — leaving clock unset",
                 error ? error->status : -1);
        return 0;
    }
    uint8_t buf[10];
    uint16_t got = 0;
    int rc = ble_hs_mbuf_to_flat(attr->om, buf, sizeof(buf), &got);
    if (rc != 0 || got < 7) {
        ESP_LOGW(TAG, "CTS short read (got=%u rc=%d)", got, rc);
        return 0;
    }
    int year    = (int)buf[0] | ((int)buf[1] << 8);
    int month   = buf[2];
    int day     = buf[3];
    int hour    = buf[4];
    int minute  = buf[5];
    int second  = buf[6];
    int frac256 = (got >= 9) ? buf[8] : 0;

    if (year < 2024 || year > 2100 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) {
        ESP_LOGW(TAG, "CTS payload looks invalid (yr=%d mo=%d dy=%d %02d:%02d:%02d)",
                 year, month, day, hour, minute, second);
        return 0;
    }
    int64_t epoch_ms = pk_clock_civil_utc_to_epoch_ms(year, month, day,
                                                      hour, minute, second, frac256);
    pk_clock_apply_epoch_ms(epoch_ms, "ios-cts");
    return 0;
}

static int cts_on_chr_disc(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (error == NULL || error->status == BLE_HS_EDONE) {
        if (s_cts_chr_val_handle == 0) {
            ESP_LOGD(TAG, "peer has CTS service but no Current Time char");
        }
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGD(TAG, "CTS chr discovery error: %d", error->status);
        return 0;
    }
    if (chr != NULL) {
        s_cts_chr_val_handle = chr->val_handle;
        ble_gattc_read(conn_handle, chr->val_handle, cts_on_read, NULL);
    }
    return 0;
}

static int cts_on_svc_disc(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;
    if (error == NULL || error->status == BLE_HS_EDONE) {
        if (s_cts_chr_val_handle == 0) {
            ESP_LOGD(TAG, "peer does not expose CTS (0x1805) — "
                          "waiting for app-side Time Sync write");
        }
        return 0;
    }
    if (error->status != 0) {
        ESP_LOGD(TAG, "CTS svc discovery error: %d", error->status);
        return 0;
    }
    if (svc != NULL) {
        ble_gattc_disc_chrs_by_uuid(conn_handle,
                                    svc->start_handle, svc->end_handle,
                                    &s_cts_chr_uuid.u,
                                    cts_on_chr_disc, NULL);
    }
    return 0;
}

static void time_sync_kickoff(uint16_t conn_handle)
{
    if (pk_clock_is_synced()) return;  /* already synced; don't re-bother iOS */
    s_cts_chr_val_handle = 0;
    int rc = ble_gattc_disc_svc_by_uuid(conn_handle, &s_cts_svc_uuid.u,
                                        cts_on_svc_disc, NULL);
    if (rc != 0) {
        ESP_LOGD(TAG, "disc_svc_by_uuid(CTS) rc=%d", rc);
    }
}

/* --- GAP event handler ----------------------------------------------- */

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            atomic_store(&s_audio_connected, true);
            atomic_store(&s_audio_mtu, 23);
            atomic_store(&s_sub_audio, false);
            atomic_store(&s_audio_route, PK_AIRBAND_BLE_ROUTE_DEVICE);
            atomic_store(&s_audio_last_error, 0);
            atomic_fetch_add(&s_audio_epoch, 1);
            pk_airband_speaker_set_remote_muted(false);
            s_advertising = false;   /* NimBLE stops adv automatically on connect */
            ESP_LOGI(TAG, "peer connected (handle=%u)", s_conn_handle);
            /* Try to suck the wall-clock out of the peer's Current Time
             * Service. Silent no-op if the peer is Android / has no CTS. */
            time_sync_kickoff(s_conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed (status=%d) — re-advertising",
                     event->connect.status);
            start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "peer disconnected (reason=0x%02x) — re-advertising",
                 event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_sub_traffic = s_sub_hb = s_sub_raw = false;
        atomic_store(&s_audio_connected, false);
        atomic_store(&s_sub_audio, false);
        atomic_store(&s_audio_route, PK_AIRBAND_BLE_ROUTE_DEVICE);
        atomic_store(&s_audio_last_error, 0);
        atomic_fetch_add(&s_audio_epoch, 1);
        pk_airband_speaker_set_remote_muted(false);
        start_advertising();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        /* Encryption is the authorization boundary for AIRBAND route writes.
         * If re-keying fails or encryption is later lost, do not leave a
         * previously selected PHONE/MUTE route silencing the local speaker. */
        struct ble_gap_conn_desc desc;
        const bool encrypted = event->enc_change.status == 0 &&
            ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0 &&
            desc.sec_state.encrypted;
        if (!encrypted) {
            audio_route_apply(PK_AIRBAND_BLE_ROUTE_DEVICE);
            ESP_LOGW(TAG, "BLE encryption unavailable (status=%d); "
                          "AIRBAND route restored to DEVICE",
                     event->enc_change.status);
        } else {
            ESP_LOGI(TAG, "BLE link encrypted; AIRBAND control writes enabled");
        }
        return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* The phone can forget its OS-side bond while the Box still has the
         * persisted key.  Keeping that orphaned key would make every future
         * protected control write fail.  Drop only this connected peer's old
         * record, then let NimBLE restart the standard pairing procedure. */
        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc != 0) {
            ESP_LOGW(TAG, "repeat pairing peer lookup failed: %d", rc);
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        rc = ble_store_util_delete_peer(&desc.peer_id_addr);
        if (rc != 0) {
            ESP_LOGW(TAG, "repeat pairing bond delete failed: %d", rc);
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        ESP_LOGI(TAG, "stale BLE bond removed; retrying pairing");
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_SUBSCRIBE: {
        /* NimBLE on IDF v6 exposes the subscribe info via an anonymous
         * inner struct; access fields directly off `event->subscribe`. */
        bool subscribed = event->subscribe.cur_notify ||
                          event->subscribe.cur_indicate;
        uint16_t attr = event->subscribe.attr_handle;
        if (attr == s_chr_traffic_handle) {
            s_sub_traffic = subscribed;
        } else if (attr == s_chr_hb_handle) {
            s_sub_hb = subscribed;
        } else if (attr == s_chr_raw_handle) {
            s_sub_raw = subscribed;
        } else if (attr == s_chr_audio_handle) {
            atomic_store(&s_sub_audio, subscribed);
            atomic_fetch_add(&s_audio_epoch, 1);
            if (!subscribed &&
                atomic_load(&s_audio_route) == PK_AIRBAND_BLE_ROUTE_PHONE)
                audio_route_apply(PK_AIRBAND_BLE_ROUTE_DEVICE);
        }
        ESP_LOGI(TAG, "subscribe attr=%u notify=%d -> traffic=%d hb=%d raw=%d audio=%d",
                 attr, subscribed, s_sub_traffic, s_sub_hb, s_sub_raw,
                 atomic_load(&s_sub_audio));
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        atomic_store(&s_audio_mtu, event->mtu.value);
        atomic_fetch_add(&s_audio_epoch, 1);
        if (atomic_load(&s_audio_route) == PK_AIRBAND_BLE_ROUTE_PHONE &&
            pk_airband_ble_fragment_count(PK_AIRBAND_BLE_FRAME_BYTES,
                                           event->mtu.value) == 0)
            audio_route_apply(PK_AIRBAND_BLE_ROUTE_DEVICE);
        ESP_LOGI(TAG, "MTU updated: %u bytes (peer=%u)",
                 event->mtu.value, event->mtu.conn_handle);
        return 0;

    default:
        return 0;
    }
}

/*
 * 定出最终广播名并同步给 GAP：用户设过名字就**原样广播那一串**，没设过才走
 * 「出厂前缀 + MAC 后三字节」。
 *
 * 后缀只跟着默认名走，不跟着自定义名走——这不是漏了，理由见 config_devname.h
 * 顶部那段：后缀是给「所有设备同名」这一种情况准备的，而出厂默认名正是那种
 * 情况；用户自己取的名重不重名由他自己负责，固件不该在他改完的名字后面再挂
 * 一条他去不掉的小尾巴。
 *
 * 长度核算（BLE 4.x legacy adv，31 字节 payload）：
 *   flags AD              3
 *   name AD 头            2
 *   ──────────────────────
 *   留给名字本身         26
 *
 *   默认名 "Pilot Kit Box-AABBCC"   20 ≤ 26   （余 6，与改名前完全一致）
 *   自定义 "<≤26>"                 ≤26 ≤ 26   （顶格，多一个字符就溢出）
 *
 * 自定义名去掉后缀之后，上限直接顶到 adv 预算本身，余量为 0——所以下面那条
 * 断言比以前更要紧，改 PK_DEVNAME_MAX_LEN 之前必须把这笔账重算一遍。
 */
/* 「先把这笔账重算一遍」靠人自觉是不够的——上面那次 UUID 溢出就是算漏了。
 * 把预算钉成编译期断言：把上限调大到装不下，构建当场就红，而不是等真机上
 * ble_gap_adv_set_fields() 回一个 BLE_HS_EINVAL、广播静默地起不来。 */
#define BLE_ADV_NAME_BUDGET   26   /* 31 − flags(3) − name AD 头(2) */
#define BLE_ADDR_SUFFIX_LEN    6   /* "AABBCC" */
_Static_assert(sizeof(BLE_DEVICE_NAME_PREFIX) - 1 + 1 + BLE_ADDR_SUFFIX_LEN
                   <= BLE_ADV_NAME_BUDGET,
               "出厂默认广播名超出 adv 的 26 字节预算");
_Static_assert(PK_DEVNAME_MAX_LEN <= BLE_ADV_NAME_BUDGET,
               "自定义广播名可能超出 adv 的 26 字节预算");
_Static_assert(sizeof(BLE_DEVICE_NAME_PREFIX) + 1 + BLE_ADDR_SUFFIX_LEN
                   <= BLE_DEVICE_NAME_MAX,
               "s_device_name 装不下默认名，snprintf 会静默截断");
/* 自定义名走的是同一个缓冲。默认名 20 字节比它短，所以这条不是上一条的重复：
 * 上限放宽时先撞上的是这一条。 */
_Static_assert(PK_DEVNAME_MAX_LEN + 1 <= BLE_DEVICE_NAME_MAX,
               "s_device_name 装不下最长的自定义名，snprintf 会静默截断");
/* NimBLE 的 GAP Device Name 特征另有一条上限（sdkconfig 的
 * CONFIG_BT_NIMBLE_GAP_DEVICE_NAME_MAX_LEN，当前 31）。adv 预算 26 比它小，
 * 所以先撞的一定是 adv；这里只把它记下来，免得将来有人误以为 26 是唯一的墙。 */

static void compose_device_name(void)
{
    char user[PK_DEVNAME_BUF_SIZE];
    pk_devname_get(user, sizeof(user));

    if (user[0] != '\0') {
        /* 用户设过名字：原样广播，不加后缀、不加任何东西。 */
        snprintf(s_device_name, sizeof(s_device_name), "%s", user);
    } else if (s_addr_suffix[0] != '\0') {
        /* 没设过 → 出厂默认名，与这个功能上线之前的行为逐字节一致。 */
        snprintf(s_device_name, sizeof(s_device_name), "%s-%s",
                 BLE_DEVICE_NAME_PREFIX, s_addr_suffix);
    } else {
        /* 控制器还没同步出地址：先用不带后缀的前缀，on_sync() 会再拼一次。 */
        snprintf(s_device_name, sizeof(s_device_name), "%s",
                 BLE_DEVICE_NAME_PREFIX);
    }

    int rc = ble_svc_gap_device_name_set(s_device_name);
    if (rc != 0) ESP_LOGW(TAG, "device_name_set failed: %d", rc);
}

const char *pk_ble_device_name(void)
{
    return s_device_name;
}

void pk_ble_device_name_apply(void)
{
    /* 没起过 BLE（用户在设置页关掉了）就只更新名字缓冲，不去碰广播——
     * NimBLE 没初始化时调 ble_gap_adv_* 会踩空。名字本身仍要更新，设置页
     * 显示的就是它，否则用户改完看着像是没生效。 */
    compose_device_name();
    if (!s_advertising) {
        ESP_LOGI(TAG, "device name -> \"%s\" (not advertising)", s_device_name);
        return;
    }

    /* 广播中的名字改不了，只能停掉重开。断连路径本来就是这么复用
     * start_advertising() 的，不是为改名新开的一条路。 */
    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "adv_stop failed: %d", rc);
    }
    s_advertising = false;
    start_advertising();
}

static void start_advertising(void)
{
    /* BLE adv packet is capped at 31 bytes. flags(3) + complete name
     * "Pilot Kit Box-AABBCC"(22) + complete 128-bit UUID(18) = 43,
     * which overflows and ble_gap_adv_set_fields returns
     * BLE_HS_EINVAL. Standard split: name + flags in adv, UUID in
     * scan response. */
    struct ble_hs_adv_fields adv = { 0 };
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.name = (uint8_t *)s_device_name;
    adv.name_len = strlen(s_device_name);
    adv.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    struct ble_hs_adv_fields rsp = { 0 };
    rsp.uuids128 = (ble_uuid128_t *)&s_svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
        return;
    }
    s_advertising = true;
    ESP_LOGI(TAG, "advertising as \"%s\"", s_device_name);
}

/* --- Sync callback (controller ready) -------------------------------- */

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr failed: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer_auto failed: %d", rc);
        return;
    }
    uint8_t addr[6];
    rc = ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "BLE address %02x:%02x:%02x:%02x:%02x:%02x type=%d",
                 addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
                 s_own_addr_type);
        /* Append last 3 MAC bytes as a per-board suffix so multiple
         * Pilot Kit Boxes in the same hangar can be told apart in a
         * BLE scanner. addr[] is little-endian per NimBLE convention,
         * so the human-readable last three bytes are addr[2..0]. */
        snprintf(s_addr_suffix, sizeof(s_addr_suffix), "%02X%02X%02X",
                 addr[2], addr[1], addr[0]);
        compose_device_name();
    }
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "stack reset: %d", reason);
    s_advertising = false;
    atomic_store(&s_audio_connected, false);
    atomic_store(&s_sub_audio, false);
    atomic_store(&s_audio_route, PK_AIRBAND_BLE_ROUTE_DEVICE);
    atomic_store(&s_audio_last_error, 0);
    atomic_fetch_add(&s_audio_epoch, 1);
    pk_airband_speaker_set_remote_muted(false);
}

/* --- Host task (NimBLE event loop) ----------------------------------- */

static void nimble_host_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "NimBLE host task running");
    nimble_port_run();          /* blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* --- Notify helpers --------------------------------------------------- */

static int notify_bytes(uint16_t handle, const uint8_t *data, size_t len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return BLE_HS_ENOTCONN;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) return BLE_HS_ENOMEM;
    int rc = ble_gatts_notify_custom(s_conn_handle, handle, om);
    if (rc != 0) {
        ESP_LOGD(TAG, "notify(handle=%u) rc=%d", handle, rc);
    }
    return rc;
}

/* GDL90 is a byte stream: a large framed message may span consecutive ATT
 * notifications. Keep chunks ordered and never interleave another GDL90
 * message until the closing 0x7E has been sent. */
static int notify_stream_bytes(uint16_t handle, const uint8_t *data, size_t len)
{
    const uint16_t mtu = atomic_load(&s_audio_mtu);
    const size_t chunk = mtu > 3 ? (size_t)mtu - 3u : 20u;
    for (size_t off = 0; off < len; off += chunk) {
        const size_t n = len - off < chunk ? len - off : chunk;
        const int rc = notify_bytes(handle, data + off, n);
        if (rc != 0) return rc;
    }
    return 0;
}

static void tx_priority_begin(void)
{portENTER_CRITICAL(&s_tx_sched_mux);pk_ble_tx_scheduler_priority_begin(&s_tx_sched);portEXIT_CRITICAL(&s_tx_sched_mux);}
static void tx_priority_end(void)
{portENTER_CRITICAL(&s_tx_sched_mux);pk_ble_tx_scheduler_priority_end(&s_tx_sched);portEXIT_CRITICAL(&s_tx_sched_mux);}
static void tx_audio_begin(uint16_t fragments)
{portENTER_CRITICAL(&s_tx_sched_mux);pk_ble_tx_scheduler_audio_begin(&s_tx_sched,fragments);portEXIT_CRITICAL(&s_tx_sched_mux);}
static pk_ble_tx_choice_t tx_next(void)
{portENTER_CRITICAL(&s_tx_sched_mux);pk_ble_tx_choice_t c=pk_ble_tx_scheduler_next(&s_tx_sched);portEXIT_CRITICAL(&s_tx_sched_mux);return c;}
static void tx_audio_complete(bool success)
{portENTER_CRITICAL(&s_tx_sched_mux);pk_ble_tx_scheduler_audio_complete(&s_tx_sched,success);portEXIT_CRITICAL(&s_tx_sched_mux);}

/* --- 1 Hz GDL90 emitter task ----------------------------------------- */

static void emitter_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "GDL90 emitter task running");

    /* aircraft_t is ~72 bytes × 64 slots ≈ 4.5 KiB — would blow our 6 KiB
     * stack once NimBLE notify frames pile on. Pinned to PSRAM so it
     * doesn't squeeze internal RAM during early boot. */
    static EXT_RAM_BSS_ATTR aircraft_t snap[AIRCRAFT_TABLE_CAPACITY];
    uint8_t    frame[GDL90_MAX_UPLINK];

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(BLE_EMIT_PERIOD_MS);

    /* 5-second rolling counters so we can log notify throughput without
     * spamming a line per second. Reset every 5 emits. */
    uint32_t hb_count = 0, traffic_count = 0, raw_count = 0, uplink_count = 0;
    uint32_t prev_uplink = 0, prev_downlink = 0;
    int      summary_ticks = 0;
    bool     was_connected = false;

    while (1) {
        uint32_t uplink_total = 0, uplink_bad = 0, uplink_no_sync = 0;
        uat_ingest_get_stats(&uplink_total, &uplink_bad, &uplink_no_sync);
        (void)uplink_bad; (void)uplink_no_sync;
        uat_downlink_ingest_stats_t uat_dl = {0};
        uat_downlink_ingest_get_stats(&uat_dl);
        const uint32_t uplink_last_second = uplink_total - prev_uplink;
        const uint32_t downlink_last_second = uat_dl.frames_total - prev_downlink;
        prev_uplink = uplink_total;
        prev_downlink = uat_dl.frames_total;
        bool connected = (s_conn_handle != BLE_HS_CONN_HANDLE_NONE);
        if (connected && !was_connected) {
            ESP_LOGI(TAG, "peer connected, GDL90 notifies start "
                          "(sub_traffic=%d sub_hb=%d sub_raw=%d)",
                     s_sub_traffic, s_sub_hb, s_sub_raw);
        } else if (!connected && was_connected) {
            ESP_LOGI(TAG, "peer disconnected, GDL90 notifies paused");
        }
        was_connected = connected;

        if (connected) {
            /* Audio sender is lower priority and checks this gate before each
             * fragment. Heartbeat/traffic/raw therefore always enter NimBLE's
             * send path first at the 1 Hz deadline. */
            tx_priority_begin();
            /* Snapshot own-ship + GPS state once per tick; used by both
             * Heartbeat (gps_fix) and Ownship Report (own_valid/own). */
            int64_t now_us = esp_timer_get_time();
            aircraft_t own; pk_own_src_t own_src;
            bool own_valid = pk_own_ship_resolve(
                now_us, AIRCRAFT_STALE_AGE_US, &own, &own_src);
            pk_gps_state_t gps; bool gps_fix = pk_gps_get(&gps);
            (void)own_src;

            /*
             * ═══ 演示模式：只发心跳，不发任何位置/目标 ═══
             *
             * GDL90 的线上格式里**没有**"这是模拟数据"这一位（FAA 560-1058-00
             * 全篇没有 simulation flag），EFB 也没有任何一款会去渲染这种标记。
             * 所以手机上收到的假目标与真目标在像素级别完全一致——盒子屏上那枚
             * DEMO 徽标救不了 ForeFlight 里凭空多出来的一架迎头飞机。
             *
             * 既然接收机没有办法把数据标成假的，那就不能把它送出去。具体：
             *   - Ownship(0x0A) / Traffic(0x14)：**一条都不发**。
             *   - Heartbeat(0x00)：照发，但 gps_valid 强制为 0。发是为了让 App
             *     知道盒子还活着（多数 EFB 超过 5 s 收不到心跳就判定链路断开，
             *     会弹一个"接收机丢失"的告警，那是个假故障）；gps_valid 清零
             *     则让 App 显示"无 GPS"，与"没有任何目标"互相印证，用户一眼就
             *     知道这台盒子当前不提供可用数据。
             *   - Raw ts-line(0x…0003)：不受影响。演示数据从来没进过解码器，
             *     那条队列里只可能有真实报文，天然是空的。
             */
            const bool demo = pk_demo_enabled();
            if (demo) gps_fix = false;

            /* Heartbeat — required by ForeFlight-style EFB apps. */
            if (s_sub_hb) {
                struct timeval tv; gettimeofday(&tv, NULL);
                /* Seconds-since-midnight UTC (will read low until SNTP). */
                uint32_t sod = (uint32_t)(tv.tv_sec % 86400);
                /* 注意：没有 uat_initialised 实参——Status1 bit0 恒 1 是
                 * 编码器按 ICD §3.1.1 h) 无条件置位的（接口初始化
                 * talkback，与 UAT 接收能力无关），见 gdl90.h。 */
                size_t n = gdl90_encode_heartbeat(frame, sizeof(frame),
                                                  /*gps_valid=*/gps_fix,
                                                  /*utc_ok=*/false,
                                                  sod,
                                                  (uint8_t)(uplink_last_second > 31
                                                      ? 31 : uplink_last_second),
                                                  (uint16_t)(downlink_last_second > 1023
                                                      ? 1023 : downlink_last_second));
                if (n > 0) { notify_bytes(s_chr_hb_handle, frame, n); ++hb_count; }
            }

            /* Pass corrected ground uplinks through exactly as FAA GDL90
             * Message ID 7. The CC RAT clock is not UTC-coupled, so TOR is
             * explicitly invalid (FFFFFF) instead of inventing a timestamp. */
            if (s_sub_traffic && !demo && s_uat_queue != NULL) {
                uint8_t payload[GDL90_UPLINK_PAYLOAD_BYTES];
                while (xQueueReceive(s_uat_queue, payload, 0) == pdTRUE) {
                    const size_t n = gdl90_encode_uplink(
                        frame, sizeof frame, 0xFFFFFFu,
                        payload, sizeof payload);
                    if (n > 0 && notify_stream_bytes(
                                     s_chr_traffic_handle, frame, n) == 0)
                        ++uplink_count;
                }
            } else if (s_uat_queue != NULL) {
                xQueueReset(s_uat_queue);
            }

            /* Traffic Report per aircraft. */
            if (s_sub_traffic && !demo) {
                /* Ownship Report (msg 0x0A) — sent before other traffic.
                 *
                 * 发不发由 pk_own_gdl90_should_emit() 判：空地状态未知时整帧
                 * 不发。线上没有"未知"编码，Misc bit3 的 0 就是"在地面"，多数
                 * EFB 会据此抑制本机周边的交通告警——发一个宣称自己在地面的
                 * 本机报文，比不发危险。判据放在 own_ship.c 而不是这里：这个
                 * 任务依赖 NimBLE，没有 host 测试缝。 */
                if (pk_own_gdl90_should_emit(own_valid, &own)) {
                    size_t no = gdl90_encode_traffic(frame, sizeof(frame),
                        /*is_ownship=*/true,
                        own.icao24,
                        own.have_position, own.lat, own.lon,
                        /* have_altitude 是**气压高度**：GPS 兜底时它恒为 false
                         * （GGA 给的是 GNSS 正高 MSL，两个基准不可换），于是
                         * 线上编 0xFFF=未知，而不是把 MSL 冒充成压力高度让
                         * EFB 拿去跟别的飞机的压力高度比。见 own_ship.h。 */
                        own.have_altitude, own.altitude_ft,
                        own.have_ground_speed, own.ground_speed_kt,
                        own.have_heading, own.heading_deg,
                        own.have_vertical_rate, own.vert_rate_fpm,
                        own.have_air_ground, own.on_ground,
                        gdl90_emitter_from_wake((int)own.wake),
                        /*callsign=*/"", /*callsign_len=*/sizeof(""));
                    if (no > 0) { notify_bytes(s_chr_traffic_handle, frame, no); }
                } else if (own_valid) {
                    /* 压掉了本机报文要说出来：手机上"没有本机位置"与"盒子没
                     * 连上"长得一样，不留痕迹就会被当成 BLE 故障去查。 */
                    static int64_t ag_log_us;
                    if (now_us - ag_log_us > 10000000LL) {
                        ag_log_us = now_us;
                        ESP_LOGW(TAG, "ownship 0x0A suppressed: air/ground unknown "
                                      "(flight phase not settled) — GDL90 has no "
                                      "\"unknown\" encoding");
                    }
                }

                /* GDL90 traffic notifies only the "fresh contact" set;
                 * EFB clients expect a plane to disappear if it stops
                 * being seen for ~60 s. */
                size_t  n_ac   = aircraft_state_snapshot(snap,
                                                         sizeof(snap) / sizeof(snap[0]),
                                                         now_us,
                                                         AIRCRAFT_STALE_AGE_US);
                for (size_t i = 0; i < n_ac; ++i) {
                    const aircraft_t *a = &snap[i];
                    size_t n = gdl90_encode_traffic_typed(frame, sizeof(frame),
                        /*is_ownship=*/false,
                        a->address_type,
                        a->icao24,
                        a->have_position, a->lat, a->lon,
                        a->have_altitude, a->altitude_ft,
                        /* 三支有效位各走各的：地面帧常常只有其中一个，
                         * 用复合位串起来会把缺的那个编成 0 发出去。 */
                        a->have_ground_speed, a->ground_speed_kt,
                        a->have_heading, a->heading_deg,
                        a->have_vertical_rate, a->vert_rate_fpm,
                        a->have_air_ground, a->on_ground,
                        gdl90_emitter_from_wake((int)a->wake),
                        a->have_callsign ? a->callsign : "",
                        /*callsign_len=*/a->have_callsign ? sizeof(a->callsign) : 0);
                    if (n > 0) { notify_bytes(s_chr_traffic_handle, frame, n); ++traffic_count; }
                }
            }

            /* Raw ts-line drain. We drain whatever queued up since the
             * last tick rather than waiting per-item, so the BLE writer
             * keeps pace with the 1 Hz emit cadence. */
            if (s_sub_raw) {
                char line[BLE_RAW_LINE_MAX];
                while (xQueueReceive(s_raw_queue, line, 0) == pdTRUE) {
                    notify_bytes(s_chr_raw_handle, (const uint8_t *)line, strlen(line));
                    ++raw_count;
                }
            } else {
                /* No subscriber → flush queue so it doesn't fill up. */
                xQueueReset(s_raw_queue);
            }

            if (++summary_ticks >= 5) {
                ESP_LOGI(TAG, "BLE emit (last 5s): hb=%lu traffic=%lu uplink=%lu raw=%lu",
                         (unsigned long)hb_count,
                         (unsigned long)traffic_count,
                         (unsigned long)uplink_count,
                         (unsigned long)raw_count);
                /* traffic=0 在演示模式下不是故障。不说清楚的话，"屏上 17 架、
                 * BLE 发 0 架"会被当成 GDL90 编码器坏了去查。 */
                if (demo)
                    ESP_LOGW(TAG, "DEMO MODE — GDL90 ownship/traffic suppressed "
                                  "on purpose (simulated data must not leave "
                                  "the box)");
                hb_count = traffic_count = uplink_count = raw_count = 0;
                summary_ticks = 0;
            }
            tx_priority_end();
        }

        vTaskDelayUntil(&last_wake, period);
    }
}

static void audio_diag_publish(const pk_airband_ble_stream_t *s,
                               uint32_t age_ms, uint32_t max_age_ms,
                               uint8_t error)
{
    pk_ble_airband_status_t d = {
        .connected = s->connected, .subscribed = s->subscribed,
        .mtu_paused = pk_airband_ble_fragment_count(
            PK_AIRBAND_BLE_FRAME_BYTES, s->mtu) == 0,
        .mtu = s->mtu, .stream_id = s->stream_id,
        .frame_seq = s->next_seq, .sample_clock = s->sample_clock,
        .encoded = s->stats.encoded, .sent = s->stats.sent,
        .dropped = s->stats.dropped, .congested = s->stats.congested,
        .mtu_paused_count = s->stats.mtu_paused,
        .queue_drops = atomic_load(&s_audio_queue_drops),
        .current_age_ms = age_ms, .max_age_ms = max_age_ms,
        .route = s->route, .last_error = error ? error : atomic_load(&s_audio_last_error),
    };
    portENTER_CRITICAL(&s_audio_diag_mux); s_audio_diag = d;
    portEXIT_CRITICAL(&s_audio_diag_mux);
}

static void audio_emitter_task(void *arg)
{
    (void)arg;
    static EXT_RAM_BSS_ATTR pk_airband_ble_stream_t stream;
    pk_airband_ble_stream_init(&stream);
    uint32_t seen_epoch = 0, max_age_ms = 0;
    uint8_t last_error = 0;
    int64_t frame_captured_us = 0;
    ble_audio_raw_t raw;
    for (;;) {
        const bool connected = atomic_load(&s_audio_connected);
        const bool subscribed = atomic_load(&s_sub_audio);
        const uint16_t mtu = atomic_load(&s_audio_mtu);
        const pk_airband_ble_route_t route =
            (pk_airband_ble_route_t)atomic_load(&s_audio_route);
        const uint32_t epoch = atomic_load(&s_audio_epoch);
        if (epoch != seen_epoch) {
            pk_airband_ble_stream_link(&stream, connected, subscribed, mtu);
            pk_airband_ble_stream_set_route(&stream, route);
            pk_airband_ble_stream_break(&stream);
            xQueueReset(s_audio_raw_queue);
            frame_captured_us = 0; seen_epoch = epoch;
        }
        if (xQueueReceive(s_audio_raw_queue, &raw, pdMS_TO_TICKS(10)) != pdTRUE) {
            audio_diag_publish(&stream, 0, max_age_ms, last_error);
            continue;
        }
        if (raw.epoch != seen_epoch) continue;
        if (stream.partial_samples == 0) frame_captured_us = raw.captured_us;
        const bool produced = pk_airband_ble_stream_ingest(&stream, raw.pcm, raw.count);
        if (!produced) {
            audio_diag_publish(&stream, 0, max_age_ms, last_error);
            continue;
        }
        pk_airband_ble_packet_t packet;
        while (pk_airband_ble_stream_pop(&stream, &packet)) {
            const size_t fragments = pk_airband_ble_fragment_count(
                sizeof(packet.data), stream.mtu);
            tx_audio_begin((uint16_t)fragments);
            bool sent = fragments != 0;
            for (size_t i = 0; sent && i < fragments; ++i) {
                while (tx_next() == PK_BLE_TX_PRIORITY) vTaskDelay(1);
                if (atomic_load(&s_audio_epoch) != seen_epoch) { sent=false; break; }
                uint8_t fragment[PK_AIRBAND_BLE_FRAME_BYTES];
                const uint8_t *data = packet.data; size_t len = sizeof(packet.data);
                if (fragments > 1) {
                    len = pk_airband_ble_make_fragment(packet.stream_id,
                        packet.frame_seq, packet.data, sizeof(packet.data),
                        stream.mtu, i, fragment, sizeof(fragment));
                    data = fragment;
                }
                if (len == 0 || notify_bytes(s_chr_audio_handle, data, len) != 0) {
                    sent = false; stream.stats.congested++; last_error = 3;
                    atomic_store(&s_audio_last_error, 3);
                    tx_audio_complete(false);
                } else {
                    tx_audio_complete(true);
                    /* One fragment per scheduler turn. This prevents a small
                     * negotiated MTU from monopolising the host task. */
                    taskYIELD();
                }
            }
            if (!sent) {
                stream.stats.dropped += stream.queued + 1u;
                atomic_fetch_add(&s_audio_epoch, 1);
                xQueueReset(s_audio_raw_queue);
                pk_airband_ble_stream_break(&stream);
                frame_captured_us = 0;
                break;
            }
            stream.stats.sent++;
            last_error = 0;
            atomic_store(&s_audio_last_error, 0);
            uint32_t age = frame_captured_us > 0
                ? (uint32_t)((esp_timer_get_time()-frame_captured_us)/1000) : 0;
            if (age > max_age_ms) max_age_ms = age;
            audio_diag_publish(&stream, age, max_age_ms, last_error);
            frame_captured_us = 0;
        }
    }
}

/* --- Public API ------------------------------------------------------- */

bool ble_gatt_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

bool ble_gatt_is_advertising(void)
{
    return s_advertising;
}

static atomic_bool s_audio_capture_valid;

void ble_gatt_airband_stream_break(void)
{
    atomic_store(&s_audio_capture_valid, false);
    atomic_fetch_add(&s_audio_epoch, 1);
}

bool ble_gatt_airband_submit(const int16_t *pcm, size_t frames, bool valid)
{
    if (!valid || pcm == NULL || frames == 0) {
        if (atomic_exchange(&s_audio_capture_valid, false))
            atomic_fetch_add(&s_audio_epoch, 1);
        return false;
    }
    atomic_store(&s_audio_capture_valid, true);
    if (frames > BLE_AUDIO_RAW_SAMPLES || s_audio_raw_queue == NULL ||
        !atomic_load(&s_audio_task_ready) ||
        !atomic_load(&s_audio_connected) || !atomic_load(&s_sub_audio) ||
        atomic_load(&s_audio_route) != PK_AIRBAND_BLE_ROUTE_PHONE)
        return false;
    ble_audio_raw_t raw = {
        .count = (uint16_t)frames,
        .epoch = atomic_load(&s_audio_epoch),
        .captured_us = esp_timer_get_time(),
    };
    memcpy(raw.pcm, pcm, frames * sizeof(*pcm));
    if (xQueueSend(s_audio_raw_queue, &raw, 0) == pdTRUE) return true;
    atomic_fetch_add(&s_audio_queue_drops, 1);
    atomic_fetch_add(&s_audio_epoch, 1);
    return false;
}

bool ble_gatt_airband_status(pk_ble_airband_status_t *out)
{
    if (out == NULL) return false;
    portENTER_CRITICAL(&s_audio_diag_mux); *out = s_audio_diag;
    portEXIT_CRITICAL(&s_audio_diag_mux);
    out->connected = atomic_load(&s_audio_connected);
    out->subscribed = atomic_load(&s_sub_audio);
    out->mtu = atomic_load(&s_audio_mtu);
    out->route = (pk_airband_ble_route_t)atomic_load(&s_audio_route);
    out->mtu_paused = pk_airband_ble_fragment_count(
        PK_AIRBAND_BLE_FRAME_BYTES, out->mtu) == 0;
    out->last_error = atomic_load(&s_audio_last_error);
    return true;
}

bool ble_gatt_airband_owner_window_toggle(void)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_owner_mux);
    s_owner_window_until_us = pk_ble_owner_window_toggle_deadline(
        now, s_owner_window_until_us, BLE_OWNER_WINDOW_US);
    const bool active = s_owner_window_until_us > now;
    portEXIT_CRITICAL(&s_owner_mux);
    ESP_LOGI(TAG, "AIRBAND owner enrollment window %s",
             active ? "open for 60 seconds" : "cancelled");
    return active;
}

uint32_t ble_gatt_airband_owner_window_remaining_s(void)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_owner_mux);
    const int64_t until = s_owner_window_until_us;
    portEXIT_CRITICAL(&s_owner_mux);
    if (until <= now) return 0;
    return (uint32_t)((until - now + 999999LL) / 1000000LL);
}

void ble_gatt_notify_raw_line(const char *line)
{
    if (line == NULL || s_raw_queue == NULL) return;
    if (!s_sub_raw) return;  /* fast path: nobody listening */
    char buf[BLE_RAW_LINE_MAX];
    size_t n = strnlen(line, sizeof(buf) - 1);
    memcpy(buf, line, n);
    buf[n] = '\0';
    /* Non-blocking; if the queue is full, the line is dropped. */
    (void)xQueueSend(s_raw_queue, buf, 0);
}

bool ble_gatt_notify_uat_uplink(const uint8_t *payload, size_t len)
{
    if (payload == NULL || len != GDL90_UPLINK_PAYLOAD_BYTES ||
        s_uat_queue == NULL || !s_sub_traffic) {
        return false;
    }
    return xQueueSend(s_uat_queue, payload, 0) == pdTRUE;
}

esp_err_t ble_gatt_init(void)
{
    s_raw_queue = xQueueCreate(BLE_RAW_QUEUE_DEPTH, BLE_RAW_LINE_MAX);
    if (s_raw_queue == NULL) {
        ESP_LOGE(TAG, "raw queue alloc failed");
        return ESP_ERR_NO_MEM;
    }
    s_uat_queue = xQueueCreate(BLE_UAT_QUEUE_DEPTH,
                               GDL90_UPLINK_PAYLOAD_BYTES);
    if (s_uat_queue == NULL) {
        ESP_LOGW(TAG, "UAT GDL90 queue alloc failed; traffic remains available");
    }
    s_audio_raw_queue = xQueueCreate(BLE_AUDIO_RAW_DEPTH, sizeof(ble_audio_raw_t));
    if (s_audio_raw_queue == NULL) {
        /* AIRBAND audio is optional to the existing GDL90 service. Keep BLE
         * traffic alive; PHONE route remains fail-closed because ready=false. */
        ESP_LOGE(TAG, "AIRBAND audio queue alloc failed; GDL90 remains available");
        atomic_store(&s_audio_last_error, 4);
    }
    portENTER_CRITICAL(&s_audio_diag_mux);
    s_audio_diag = (pk_ble_airband_status_t){
        .mtu = 23, .route = PK_AIRBAND_BLE_ROUTE_DEVICE,
    };
    portEXIT_CRITICAL(&s_audio_diag_mux);
    pk_ble_tx_scheduler_init(&s_tx_sched);

    /* Bring up the SDIO transport to C6 first. Without this, the BT
     * controller RPC calls below fail immediately with ESP_FAIL
     * because esp_hosted's check_transport_up() short-circuits. */
    int rc_h = esp_hosted_connect_to_slave();
    if (rc_h != 0) {
        ESP_LOGE(TAG, "esp_hosted_connect_to_slave: %d", rc_h);
        return ESP_FAIL;
    }

    owner_load();

    /* The C6's BT controller is gated behind an RPC handshake — the
     * slave only powers up the controller when the host asks for it.
     * Without these two calls, nimble_port_init() succeeds (the
     * VHCI transport is up) but every HCI command times out because
     * there's nothing on the other side. Mirrors the official
     * managed_components/.../examples/host_nimble_bleprph_host_only_vhci
     * sequence: connect_to_slave → bt_controller_init → enable →
     * nimble_port_init. */
    esp_err_t err = esp_hosted_bt_controller_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_bt_controller_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_hosted_bt_controller_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hosted_bt_controller_enable: %s", esp_err_to_name(err));
        return err;
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s — ensure C6 ESP-Hosted slave"
                      " firmware is flashed and SDIO pins are correct",
                 esp_err_to_name(err));
        return err;
    }
    int mtu_rc = ble_att_set_preferred_mtu(256);
    if (mtu_rc != 0)
        ESP_LOGW(TAG, "preferred ATT MTU 256 rejected: %d", mtu_rc);

    ble_hs_cfg.sync_cb              = on_sync;
    ble_hs_cfg.reset_cb             = on_reset;
    ble_hs_cfg.gatts_register_cb    = NULL;
    ble_hs_cfg.store_status_cb      = ble_store_util_status_rr;

    /* AIRBAND Control is the only characteristic that mutates a physical
     * output.  Mirror ESP-IDF's NimBLE Security, bleprph and protocomm
     * patterns: Secure Connections + encrypted GATT permission + bonding.
     * The box has no trusted keyboard/display pairing channel, so claiming
     * MITM protection would be false; NO_IO deliberately selects Just Works.
     * NVS persistence is enabled in sdkconfig so the OS does not have to pair
     * again after every box reboot. */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_our_key_dist =
        BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist =
        BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_count_cfg: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_add_svcs: %d", rc);
        return ESP_FAIL;
    }

    /* Placeholder name — the MAC-suffixed version lands in on_sync()
     * once the controller has assigned us an address. */
    rc = ble_svc_gap_device_name_set(s_device_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "device_name_set: %d", rc);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(nimble_host_task);

    BaseType_t ok = xTaskCreatePinnedToCore(
        emitter_task, "ble_emit", BLE_EMIT_STACK, NULL, 3, NULL, 0);
    if (ok != pdTRUE) {
        ESP_LOGE(TAG, "emitter_task spawn failed");
        return ESP_ERR_NO_MEM;
    }
    if (s_audio_raw_queue != NULL) {
        ok = xTaskCreatePinnedToCore(
            audio_emitter_task, "ble_air_audio", BLE_AUDIO_STACK, NULL, 2, NULL, 0);
        if (ok == pdTRUE) {
            atomic_store(&s_audio_task_ready, true);
        } else {
            ESP_LOGE(TAG, "AIRBAND audio emitter spawn failed; GDL90 remains available");
            atomic_store(&s_audio_last_error, 4);
        }
    }

    return ESP_OK;
}
