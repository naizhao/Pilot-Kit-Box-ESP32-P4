/*
 * ble_gatt.c — NimBLE host + Pilot Kit GATT service + GDL90 emitter.
 *
 * Three pieces live in this file:
 *
 *   1. NimBLE bring-up — nimble_port_init() over ESP-Hosted's VHCI,
 *      callbacks for GAP advertising / connect / subscribe, and a
 *      single primary service with three notify characteristics.
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
 */

#include "ble_gatt.h"

#include <inttypes.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "esp_hosted.h"               /* esp_hosted_connect_to_slave */
#include "esp_hosted_misc.h"          /* esp_hosted_bt_controller_init/enable */

#include "aircraft_state.h"
#include "config_devname.h"           /* 用户自定义的广播名前半段 */
#include "gdl90.h"
#include "gps.h"
#include "own_ship.h"
#include "pk_clock.h"

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

static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool     s_sub_traffic;
static bool     s_sub_hb;
static bool     s_sub_raw;
static volatile bool s_advertising;   /* true while ble_gap_adv_start has been called
                                        * and no connect/stop has occurred yet */

static uint8_t       s_own_addr_type;
static QueueHandle_t s_raw_queue;   /* char[BLE_RAW_LINE_MAX] entries */

/* --- Forward declarations -------------------------------------------- */

static int  gap_event_cb(struct ble_gap_event *event, void *arg);
static int  chr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);
static int  chr_time_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg);
static void start_advertising(void);
static void emitter_task(void *arg);
static void time_sync_kickoff(uint16_t conn_handle);

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
        start_advertising();
        return 0;

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
        }
        ESP_LOGI(TAG, "subscribe attr=%u notify=%d -> traffic=%d hb=%d raw=%d",
                 attr, subscribed, s_sub_traffic, s_sub_hb, s_sub_raw);
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
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

static void notify_bytes(uint16_t handle, const uint8_t *data, size_t len)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) return;
    int rc = ble_gatts_notify_custom(s_conn_handle, handle, om);
    if (rc != 0) {
        ESP_LOGD(TAG, "notify(handle=%u) rc=%d", handle, rc);
    }
}

/* --- 1 Hz GDL90 emitter task ----------------------------------------- */

static void emitter_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "GDL90 emitter task running");

    /* aircraft_t is ~72 bytes × 64 slots ≈ 4.5 KiB — would blow our 6 KiB
     * stack once NimBLE notify frames pile on. Pinned to PSRAM so it
     * doesn't squeeze internal RAM during early boot. */
    static EXT_RAM_BSS_ATTR aircraft_t snap[AIRCRAFT_TABLE_CAPACITY];
    uint8_t    frame[GDL90_MAX_FRAME];

    TickType_t last_wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(BLE_EMIT_PERIOD_MS);

    /* 5-second rolling counters so we can log notify throughput without
     * spamming a line per second. Reset every 5 emits. */
    uint32_t hb_count = 0, traffic_count = 0, raw_count = 0;
    int      summary_ticks = 0;
    bool     was_connected = false;

    while (1) {
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
            /* Snapshot own-ship + GPS state once per tick; used by both
             * Heartbeat (gps_fix) and Ownship Report (own_valid/own). */
            int64_t now_us = esp_timer_get_time();
            aircraft_t own; pk_own_src_t own_src;
            bool own_valid = pk_own_ship_resolve(
                now_us, AIRCRAFT_STALE_AGE_US, &own, &own_src);
            pk_gps_state_t gps; bool gps_fix = pk_gps_get(&gps);
            (void)own_src;

            /* Heartbeat — required by ForeFlight-style EFB apps. */
            if (s_sub_hb) {
                struct timeval tv; gettimeofday(&tv, NULL);
                /* Seconds-since-midnight UTC (will read low until SNTP). */
                uint32_t sod = (uint32_t)(tv.tv_sec % 86400);
                size_t n = gdl90_encode_heartbeat(frame, sizeof(frame),
                                                  /*gps_valid=*/gps_fix,
                                                  /*uat_initialised=*/true,
                                                  /*utc_ok=*/false,
                                                  sod,
                                                  /*uplink=*/0,
                                                  /*basic_long=*/0);
                if (n > 0) { notify_bytes(s_chr_hb_handle, frame, n); ++hb_count; }
            }

            /* Traffic Report per aircraft. */
            if (s_sub_traffic) {
                /* Ownship Report (msg 0x0A) — sent before other traffic. */
                if (own_valid) {
                    size_t no = gdl90_encode_traffic(frame, sizeof(frame),
                        /*is_ownship=*/true,
                        own.icao24,
                        own.have_position, own.lat, own.lon,
                        own.have_altitude, own.altitude_ft,
                        own.have_velocity, own.heading_deg,
                        own.ground_speed_kt, own.vert_rate_fpm,
                        /*callsign=*/"");
                    if (no > 0) { notify_bytes(s_chr_traffic_handle, frame, no); }
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
                    size_t n = gdl90_encode_traffic(frame, sizeof(frame),
                        /*is_ownship=*/false,
                        a->icao24,
                        a->have_position, a->lat, a->lon,
                        a->have_altitude, a->altitude_ft,
                        a->have_velocity, a->heading_deg,
                        a->ground_speed_kt, a->vert_rate_fpm,
                        a->have_callsign ? a->callsign : "");
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
                ESP_LOGI(TAG, "BLE emit (last 5s): hb=%lu traffic=%lu raw=%lu",
                         (unsigned long)hb_count,
                         (unsigned long)traffic_count,
                         (unsigned long)raw_count);
                hb_count = traffic_count = raw_count = 0;
                summary_ticks = 0;
            }
        }

        vTaskDelayUntil(&last_wake, period);
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

esp_err_t ble_gatt_init(void)
{
    s_raw_queue = xQueueCreate(BLE_RAW_QUEUE_DEPTH, BLE_RAW_LINE_MAX);
    if (s_raw_queue == NULL) {
        ESP_LOGE(TAG, "raw queue alloc failed");
        return ESP_ERR_NO_MEM;
    }

    /* Bring up the SDIO transport to C6 first. Without this, the BT
     * controller RPC calls below fail immediately with ESP_FAIL
     * because esp_hosted's check_transport_up() short-circuits. */
    int rc_h = esp_hosted_connect_to_slave();
    if (rc_h != 0) {
        ESP_LOGE(TAG, "esp_hosted_connect_to_slave: %d", rc_h);
        return ESP_FAIL;
    }

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

    ble_hs_cfg.sync_cb              = on_sync;
    ble_hs_cfg.reset_cb             = on_reset;
    ble_hs_cfg.gatts_register_cb    = NULL;
    ble_hs_cfg.store_status_cb      = ble_store_util_status_rr;

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

    return ESP_OK;
}
