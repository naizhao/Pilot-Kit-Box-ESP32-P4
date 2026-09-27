/*
 * test_adsb_link_dispatch.c — execute the production modes_ingest sink in
 * adsb_link_task.c and verify post-decoder fan-out.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -DPK_HOST_TEST -I firmware/test/host_stubs -I firmware/main \
 *      -I firmware/components/adsb_link_codec \
 *      -o /tmp/test_adsb_link_dispatch \
 *      firmware/test/test_adsb_link_dispatch.c \
 *      firmware/main/mode_s.c firmware/main/modes_ingest.c \
 *      firmware/main/aircraft_state.c firmware/main/pk_callsign.c \
 *      firmware/main/adsb_link_task.c firmware/main/vbus_sense.c -lm \
 *   && /tmp/test_adsb_link_dispatch
 *
 * The ESP/FreeRTOS pieces are stubbed, but mode_s_decode(), the CRC gate,
 * aircraft_state_ingest(), and the production on_ingest_msg() dispatcher are
 * the real implementations.  This targets the boundary missed by decoder-only
 * tests: DF18 is decoded and fused, but the link-layer switch must not drop
 * its identity/position fan-out on the floor.
 */
#include "aircraft_state.h"
#include "adsb_link.h"
#include "adsb_link_task.h"
#include "config_antenna.h"
#include "vbus_sense.h"
#include "cpr_decode.h"
#include "gps.h"
#include "mode_s.h"
#include "modes_ingest.h"
#include "pk_rec_ingest.h"
#include "record_sink.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int64_t g_now_us = 1000000;
int64_t esp_timer_get_time(void) { return g_now_us; }

static bool g_gps_valid;
static pk_gps_state_t g_gps;
static pk_ant_gnss_t g_requested_gnss = PK_ANT_GNSS_EXTERNAL;
static int g_apply_started;
static int g_apply_confirmed;
static int g_apply_failed;
static int g_config_pending;
static bool g_actual_known;
static pk_ant_gnss_t g_started_source;
static pk_ant_gnss_t g_confirmed_source[4];
static uint32_t g_position_icao;
static bool g_airborne_decode_called;
static uint32_t g_airborne_icao;

bool pk_gps_get(pk_gps_state_t *out)
{
    if (!g_gps_valid) return false;
    *out = g_gps;
    return true;
}

bool pk_demo_enabled(void) { return false; }

/* Antenna config lives in NVS on the P4 and is pushed to the RP2040 over
 * CONFIG_REQ.  This test exercises the message dispatcher, not the config
 * store, so the payload builder is stubbed out to "nothing to send" — which
 * also keeps link_tx() off the UART path entirely. */
size_t pk_antenna_build_config_payload(uint8_t *out, size_t cap)
{
    if (cap < 7) return 0;
    out[0] = 3;
    out[1] = ADSB_LINK_CFG_ANT_1090; out[2] = 0;
    out[3] = ADSB_LINK_CFG_ANT_GNSS; out[4] = (uint8_t)g_requested_gnss;
    out[5] = ADSB_LINK_CFG_THRESHOLD; out[6] = 0;
    return 7;
}

/* adsb_link_task.c 的 RP health 日志在 v1.3 里会读 P4 本地值做回退
 * （对面是旧 RP2040、只发 11 个字段时），这两个 getter 因此成为真实引用。
 * 旧版把它们写在 ESP_LOGI 的参数里，host 的 `if(0)` 日志桩会优化掉调用，
 * 所以此前不需要桩。本测试不验配置存储，给固定值即可。 */
pk_ant_1090_t pk_ant_1090_get(void) { return PK_ANT_1090_ONBOARD; }
pk_ant_gnss_t pk_ant_gnss_get(void) { return g_requested_gnss; }
void pk_ant_gnss_status_get(pk_ant_gnss_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->requested = g_requested_gnss;
    out->actual_known = g_actual_known;
    out->request_pending = g_config_pending != 0;
}
int pk_thresh_1090_get(void) { return 0; }
void pk_ant_gnss_apply_started(pk_ant_gnss_t source)
{
    ++g_apply_started;
    ++g_config_pending;
    g_started_source = source;
}
void pk_ant_gnss_apply_confirmed(pk_ant_gnss_t source)
{
    if (g_config_pending) --g_config_pending;
    g_actual_known = true;
    if (g_apply_confirmed < (int)(sizeof g_confirmed_source /
                                  sizeof g_confirmed_source[0]))
        g_confirmed_source[g_apply_confirmed] = source;
    ++g_apply_confirmed;
}
void pk_ant_gnss_apply_failed(pk_ant_gnss_t source)
{
    (void)source;
    if (g_config_pending) --g_config_pending;
    g_actual_known = false;
    ++g_apply_failed;
}
void pk_ant_config_link_reset(void)
{
    g_config_pending = 0;
    g_actual_known = false;
}

void pk_rec_ingest_init(void) {}
void pk_rec_ingest_position(uint32_t icao24, int64_t ts_ms, double lat,
                            double lon, bool have_alt, int alt_ft,
                            bool have_gs, int gs_kt, bool have_track,
                            int track_deg, bool have_vs, int vs_fpm,
                            bool on_ground, bool from_surface_cpr)
{
    (void)icao24; (void)ts_ms; (void)lat; (void)lon; (void)have_alt;
    (void)alt_ft; (void)have_gs; (void)gs_kt; (void)have_track;
    (void)track_deg; (void)have_vs; (void)vs_fpm; (void)on_ground;
    if (from_surface_cpr) g_position_icao = icao24;
}

static uint32_t g_identity_calls;
static uint32_t g_identity_icao;
static char g_identity_callsign[AIRCRAFT_CALLSIGN_LEN];
static bool g_surface_decode_called;

bool pk_demo_traffic(void) { return false; }
float pk_demo_yaw_deg(int64_t now_us) { (void)now_us; return 0.0f; }
int pk_demo_own_alt_ft(int64_t now_us) { (void)now_us; return 0; }
uint32_t pk_ui_get_own_icao(void) { return 0; }

void adsb_link_dec_init(adsb_link_dec_t *d, adsb_link_on_msg_fn cb, void *user)
{ (void)d; (void)cb; (void)user; }
/* adsb_link_task.c 的常驻任务循环（`while(1)` + uart_read_bytes）会调
 * adsb_link_dec_feed；本测试不编入 codec.c（否则与上面这些桩重复定义），
 * 所以必须给桩。**缺了它在 -O2 下也能链上**——那个任务函数无人引用，会被
 * 优化掉、连调用一起消失——但 -O0 或将来测试开始引用它就会暴露，属于靠
 * 优化级别维持的假通过。 */
void adsb_link_dec_feed(adsb_link_dec_t *d, const uint8_t *data, size_t n)
{ (void)d; (void)data; (void)n; }
size_t adsb_link_encode(uint8_t *out, size_t cap, uint8_t msg_type,
                        uint8_t seq, const uint8_t *payload, size_t payload_len)
{
    (void)msg_type; (void)seq; (void)payload;
    const size_t n = payload_len + ADSB_LINK_HDR_LEN + 2;
    if (cap < n) return 0;
    memset(out, 0, n);
    return n;
}
bool adsb_link_uat_uplink_decode(const uint8_t *payload, size_t len,
                                 uint8_t *rssi, uint32_t *rp_ts_us,
                                 const uint8_t **frame)
{
    (void)payload; (void)len; (void)rssi; (void)rp_ts_us; (void)frame;
    return false;
}
/* 本测试驱动的是分发和扫描结果表，不验字节编码（那是 test_adsb_link_codec.c
 * 的事）。config_encode 返回 0 让 link_tx 整条路径不被触发——UART 在 host
 * 上并不存在。返回类型与真声明保持一致，其余同上面几个桩的惯例。 */
size_t adsb_link_config_encode(uint8_t *out, size_t cap,
                               const adsb_link_cfg_item_t *items, uint8_t n)
{
    const size_t len = 1u + 2u * n;
    if (cap < len) return 0;
    out[0] = n;
    for (uint8_t i = 0; i < n; i++) {
        out[1 + 2 * i] = items[i].key;
        out[2 + 2 * i] = items[i].val;
    }
    return len;
}
bool adsb_link_sweep_row_decode(const uint8_t *payload, size_t len,
                                uint8_t *idx, uint8_t *total,
                                uint8_t *permille_val, uint16_t *level_mv,
                                uint32_t *frames, uint32_t *pre,
                                uint32_t *noise, uint32_t *ovr)
{
    (void)payload; (void)len; (void)idx; (void)total; (void)permille_val;
    (void)level_mv; (void)frames; (void)pre; (void)noise; (void)ovr;
    return false;
}
void cpr_init(void) {}
bool cpr_decode_position(uint32_t icao24, int fflag, bool is_surface,
                         int lat_cpr, int lon_cpr, int64_t now_us,
                         cpr_position_t *out_pos)
{
    (void)icao24; (void)fflag; (void)is_surface; (void)lat_cpr;
    (void)lon_cpr; (void)now_us;
    g_airborne_decode_called = true;
    g_airborne_icao = icao24;
    out_pos->valid = true;
    out_pos->lat = 37.0;
    out_pos->lon = -122.0;
    return true;
}
bool cpr_decode_surface_local(int fflag, int lat_cpr, int lon_cpr,
                               double ref_lat, double ref_lon,
                               cpr_position_t *out_pos)
{
    (void)fflag; (void)lat_cpr; (void)lon_cpr;
    (void)ref_lat; (void)ref_lon;
    g_surface_decode_called = true;
    out_pos->valid = true;
    out_pos->lat = 37.0;
    out_pos->lon = -122.0;
    return true;
}
void uat_ingest_init(void) {}
void uat_ingest_feed(void) {}
void uat_ingest_get_stats(void) {}
void record_dispatch(const record_t *rec) { (void)rec; }
bool record_sink_uart_stats(uint32_t *written, uint32_t *dropped,
                            uint32_t *pending)
{
    (void)written; (void)dropped; (void)pending;
    return false;
}

void pk_rec_ingest_identity(uint32_t icao24, int64_t ts_ms,
                            const char *callsign, uint8_t emitter_category)
{
    (void)ts_ms; (void)emitter_category;
    ++g_identity_calls;
    g_identity_icao = icao24;
    snprintf(g_identity_callsign, sizeof(g_identity_callsign), "%s", callsign);
}

bool pk_rec_ingest_stats(uint32_t *out_written, uint32_t *out_dropped)
{
    (void)out_written; (void)out_dropped;
    return false;
}

/* vbus_sense.c 要 pk_board_profile()。本测试不关心板型（HEALTH_STATS 只把
 * 中点电压存下来，换算在别处测），给个固定桩即可。 */
pk_board_profile_t pk_board_profile(void) { return PK_BOARD_PROFILE_V4; }

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { \
        printf("  [FAIL] " __VA_ARGS__); \
        printf("        at %s:%d\n", __FILE__, __LINE__); ++g_fail; \
    } } while (0)

static void set_crc(unsigned char msg[14])
{
    const uint32_t crc = mode_s_checksum(msg, 112);
    msg[11] = (unsigned char)(crc >> 16);
    msg[12] = (unsigned char)(crc >> 8);
    msg[13] = (unsigned char)crc;
}

static void build_df18_ident(unsigned char msg[14], uint32_t address)
{
    memset(msg, 0, 14);
    msg[0] = (unsigned char)(18 << 3);             /* CF0, IMF=0 */
    msg[1] = (unsigned char)(address >> 16);
    msg[2] = (unsigned char)(address >> 8);
    msg[3] = (unsigned char)address;
    msg[4] = 4 << 3;                               /* TC4, category B2 */
    /* Callsign charset: A=1..Z=26, blank=32. Build "DS18    ". */
    static const unsigned char chars[8] = {4, 19, 24, 32, 32, 32, 32, 32};
    msg[5] = (unsigned char)((chars[0] << 2) | (chars[1] >> 4));
    msg[6] = (unsigned char)((chars[1] << 4) | (chars[2] >> 2));
    msg[7] = (unsigned char)((chars[2] << 6) | chars[3]);
    msg[8] = (unsigned char)((chars[3] << 2) | (chars[4] >> 4));
    msg[9] = (unsigned char)((chars[4] << 4) | (chars[5] >> 2));
    msg[10] = (unsigned char)((chars[5] << 6) | chars[6]);
    set_crc(msg);
}

static void build_df18_surface(unsigned char msg[14], int cf)
{
    memset(msg, 0, 14);
    msg[0] = (unsigned char)((18 << 3) | cf);
    msg[1] = 0xB0;
    msg[2] = 0x00;
    msg[3] = 0x03;
    msg[4] = 6 << 3;           /* TC6; ME position fields remain zero. */
    set_crc(msg);
}

static void from_hex(const char *hex, unsigned char msg[14])
{
    for (int i = 0; i < 14; ++i) {
        unsigned int value = 0;
        (void)sscanf(hex + i * 2, "%2x", &value);
        msg[i] = (unsigned char)value;
    }
}

static bool get_aircraft(uint32_t icao, aircraft_t *out)
{
    aircraft_t all[AIRCRAFT_TABLE_CAPACITY];
    const size_t n = aircraft_state_snapshot(all, AIRCRAFT_TABLE_CAPACITY,
                                             esp_timer_get_time() + 1000,
                                             1000000);
    for (size_t i = 0; i < n; ++i) {
        if (all[i].icao24 == icao) { *out = all[i]; return true; }
    }
    return false;
}

static void test_df18_identity_reaches_traffic_record(void)
{
    unsigned char msg[14];
    aircraft_t a;
    g_identity_calls = 0;
    g_identity_icao = 0;
    memset(g_identity_callsign, 0, sizeof(g_identity_callsign));
    aircraft_state_init();

    build_df18_ident(msg, 0xA00001);
    modes_ingest_feed(msg, 112, NULL);

    CHECK(get_aircraft(0xA00001, &a), "DF18 aircraft missing from fusion table\n");
    CHECK(a.have_callsign, "DF18 callsign missing from fusion table\n");
    CHECK(g_identity_calls == 1,
          "DF18 identity frame must write exactly one traffic.trk record\n");
    CHECK(g_identity_icao == 0xA00001, "DF18 identity record ICAO");
    CHECK(strcmp(g_identity_callsign, "DSX") == 0,
          "DF18 identity record callsign got='%s'\n", g_identity_callsign);
}

static void test_nonicao_df18_surface_does_not_create_target(void)
{
    unsigned char msg[14];
    aircraft_t a;
    aircraft_state_init();
    g_gps_valid = true;
    g_gps.have_fix = true;
    g_gps.lat = 37.0;
    g_gps.lon = -122.0;
    g_surface_decode_called = false;
    g_position_icao = 0;

    build_df18_surface(msg, 3);
    modes_ingest_feed(msg, 112, NULL);
    CHECK(!g_surface_decode_called,
          "non-ICAO DF18 must not reach the standard surface CPR decoder\n");
    CHECK(g_position_icao == 0,
          "non-ICAO DF18 must not write a traffic record\n");
    CHECK(!get_aircraft(0xB00003, &a),
          "non-ICAO DF18 CF3 must not create a fusion target\n");

    build_df18_surface(msg, 1);
    modes_ingest_feed(msg, 112, NULL);
    CHECK(!get_aircraft(0xB00001, &a),
          "non-ICAO DF18 CF1 must not create a fusion target\n");

    g_gps_valid = false;
}

static void test_tc20_position_reaches_airborne_cpr(void)
{
    unsigned char msg[14];
    aircraft_t a;
    aircraft_state_init();
    g_airborne_decode_called = false;
    g_airborne_icao = 0;

    /* Captured TC20 vector from test_mode_s_extended.c. DF17 here isolates
     * metype routing from DF18 address semantics. */
    from_hex("8E7C6296A0A0A59C64468D6F4EDD", msg);
    modes_ingest_feed(msg, 112, NULL);

    CHECK(g_airborne_decode_called,
          "TC20 GNSS position frame must reach airborne CPR decoder\n");
    CHECK(g_airborne_icao == 0x7C6296, "TC20 airborne CPR ICAO");
    CHECK(get_aircraft(0x7C6296, &a), "TC20 aircraft missing from fusion table\n");
    CHECK(a.have_position, "TC20 decoded position missing from fusion table\n");
}

/*
 * 门限扫描结果表（v1.3）。这里测的是表本身的行为——下标推进、越界拒收、
 * 清空语义；SWEEP_ROW 的字节解码另由 test_adsb_link_codec.c 覆盖。
 */
static void test_sweep_table(void)
{
    pk_sweep_row_t rows[PK_SWEEP_MAX_ROWS];
    int total = -1;

    pk_sweep_start();
    CHECK(pk_sweep_snapshot(rows, PK_SWEEP_MAX_ROWS, &total) == 0,
          "start 应清空上一轮\n");
    CHECK(total == 0, "start 后 total 应为 0，实际 %d\n", total);
    /* 这里原来断言的是 !running —— 把缺陷行为写成了期望，于是 host 全绿而
     * 实机上「点开始没反应」。running 的语义由 test_sweep_running_state()
     * 专门锁，这一条只确认 start 不会把表留脏。 */
    CHECK(pk_sweep_running(), "start 之后应立刻进入『扫描中』\n");

    pk_sweep_ingest_row(0, 9, 200, 724, 0, 0, 175027, 289423);
    CHECK(pk_sweep_snapshot(rows, PK_SWEEP_MAX_ROWS, &total) == 1,
          "收到第一行后应有 1 行\n");
    CHECK(total == 9, "total 应为 9，实际 %d\n", total);
    CHECK(pk_sweep_running(), "1/9 时应报『扫描中』\n");
    CHECK(rows[0].permille == 200 && rows[0].level_mv == 724 &&
          rows[0].noise == 175027 && rows[0].ovr == 289423,
          "第一行字段错\n");

    /* 中间丢一行：跳着收 idx=3。已收行数必须按最大下标推进到 4，而不是
     * 停在 2——用 ++ 计数的话进度会永远差一格，最后一行到了也判不出结束。 */
    pk_sweep_ingest_row(3, 9, 500, 1636, 2, 9, 1234217, 240930);
    CHECK(pk_sweep_snapshot(rows, PK_SWEEP_MAX_ROWS, &total) == 4,
          "跳号后应按最大下标推进到 4 行\n");
    CHECK(rows[3].permille == 500 && rows[3].frames == 2, "第四行字段错\n");

    /* 收到最后一行 → 不再是"扫描中" */
    pk_sweep_ingest_row(8, 9, 1000, 3087, 0, 12, 27940, 2);
    CHECK(pk_sweep_snapshot(rows, PK_SWEEP_MAX_ROWS, &total) == 9,
          "收到末行后应为 9 行\n");
    CHECK(!pk_sweep_running(), "9/9 之后不该再报『扫描中』\n");

    /* 越界行整条拒收，且不得污染 total —— 把 total 截断成我们能存下的数目，
     * 会让对面还在扫的时候就判成已结束。 */
    pk_sweep_ingest_row(PK_SWEEP_MAX_ROWS, 9, 600, 0, 0, 0, 0, 0);
    pk_sweep_ingest_row(0, PK_SWEEP_MAX_ROWS + 1, 600, 0, 0, 0, 0, 0);
    CHECK(pk_sweep_snapshot(rows, PK_SWEEP_MAX_ROWS, &total) == 9 &&
          total == 9, "越界行不该改变表状态（n=%d total=%d）\n",
          pk_sweep_snapshot(rows, PK_SWEEP_MAX_ROWS, NULL), total);

    /* cap 小于已收行数时只拷 cap 行，不越界写调用方缓冲 */
    pk_sweep_row_t small[2];
    CHECK(pk_sweep_snapshot(small, 2, NULL) == 2, "cap 限制应生效\n");

    /* 重新开始必须把上一轮抹掉：两轮之间天线和环境都可能变了，残留的尾部
     * 会被当成本轮结果读。 */
    pk_sweep_start();
    CHECK(pk_sweep_snapshot(rows, PK_SWEEP_MAX_ROWS, &total) == 0 && total == 0,
          "重新 start 未清空\n");
}

/*
 * 「是否在扫」的状态机。两条都是实机上暴露出来的真实故障（2026-09-13）：
 * 用户报「点开始没反应」「点停止又不会停」。
 *
 * 根因是把它写成了 (total > 0 && n < total) —— 一个靠对面回包推断的量：
 *   · 刚启动时 total 还是 0，要等第一行回来（10 秒）按钮才翻，看起来就是死的；
 *   · 中途停止只发报文不清本地量，n < total 依旧成立，按钮永久卡在「停止」。
 * 按钮状态必须在点下去那一帧就翻过来，不能取决于对面什么时候回话。
 */
static void test_sweep_running_state(void)
{
    /* 启动的**那一刻**就必须为真——这正是"点了没反应"的那 10 秒 */
    pk_sweep_start();
    CHECK(pk_sweep_running(),
          "start 之后立刻就该是『扫描中』，不能等第一行回来\n");

    /* 收到中间几行，仍在扫 */
    pk_sweep_ingest_row(0, 9, 200, 700, 0, 0, 0, 0);
    CHECK(pk_sweep_running(), "收到第 1/9 行后仍应是『扫描中』\n");

    /* 中途停止：本地状态必须立刻翻，不等对面确认 */
    pk_sweep_stop();
    CHECK(!pk_sweep_running(),
          "stop 之后必须立刻退出『扫描中』，否则按钮永久卡住\n");

    /* 停止后即便又飘回来一行（在途报文），也不该把状态又拉回『扫描中』 */
    pk_sweep_ingest_row(1, 9, 300, 980, 0, 0, 0, 0);
    CHECK(!pk_sweep_running(), "停止后在途报文不该让状态复活\n");

    /* 跑满自然收尾：不用人再点一次停止 */
    pk_sweep_start();
    CHECK(pk_sweep_running(), "重新 start 应进入『扫描中』\n");
    for (int i = 0; i < 9; i++)
        pk_sweep_ingest_row((uint8_t)i, 9, (uint16_t)((i + 2) * 100),
                            700, 0, 1, 0, 0);
    CHECK(!pk_sweep_running(),
          "收满 9/9 后应自动收尾，不该还显示『扫描中』\n");
}

static void dispatch_ack(uint8_t applied, uint8_t rejected)
{
    adsb_link_msg_t m = { .type = ADSB_LINK_MSG_CONFIG_ACK,
                          .payload_len = 2 };
    m.payload[0] = applied;
    m.payload[1] = rejected;
    pk_adsb_link_test_dispatch(&m);
}

/* 请求值只是 P4 的意图。只有与已发送完整 CONFIG_REQ 对应、且 3 项全接受的
 * ACK 才能把 GNSS 来源升级为“RP2040 已确认实际生效”。 */
static void test_gnss_actual_source_requires_matching_ack(void)
{
    g_apply_started = g_apply_confirmed = g_apply_failed = 0;
    g_config_pending = 0;
    g_actual_known = false;

    /* 链路锁还没建立，实际一字节都没发；不得进入“正在切换”。 */
    g_requested_gnss = PK_ANT_GNSS_ONBOARD;
    pk_adsb_link_push_config();
    CHECK(g_apply_started == 0 && g_apply_confirmed == 0,
          "未发送的请求不得改变实际来源状态\n");

    pk_adsb_link_start();
    pk_adsb_link_push_config();
    CHECK(g_apply_started == 1 && g_started_source == PK_ANT_GNSS_ONBOARD,
          "成功发送完整配置后应进入 ONBOARD pending\n");
    dispatch_ack(3, 0);
    CHECK(g_apply_confirmed == 1 &&
          g_confirmed_source[0] == PK_ANT_GNSS_ONBOARD,
          "3/0 ACK 应确认发送时快照 ONBOARD\n");

    pk_adsb_link_push_config();
    dispatch_ack(2, 1);
    CHECK(g_apply_confirmed == 1 && g_apply_failed == 1,
          "有 rejected 的 ACK 不得确认实际来源\n");

    /* 两个在途请求必须按发送快照 FIFO 对应，不能在 ACK 时重读最新请求值。 */
    g_requested_gnss = PK_ANT_GNSS_EXTERNAL;
    pk_adsb_link_push_config();
    g_requested_gnss = PK_ANT_GNSS_ONBOARD;
    pk_adsb_link_push_config();
    dispatch_ack(3, 0);
    dispatch_ack(3, 0);
    CHECK(g_confirmed_source[1] == PK_ANT_GNSS_EXTERNAL &&
          g_confirmed_source[2] == PK_ANT_GNSS_ONBOARD,
          "ACK 必须确认发送时 FIFO 快照，不得两次都读成最新请求\n");

    /* SWEEP 也是 CONFIG_REQ/ACK，但只有一项；其 ACK 不得误认成天线确认。 */
    pk_sweep_start();
    dispatch_ack(1, 0);
    CHECK(g_apply_confirmed == 3,
          "扫描 ACK 不得冒充 GNSS 配置确认\n");
}

/* P4 单独复位/刷机时，RP2040 不会复位，可能已经处于 linked 状态而不再发
 * HELLO。此时第一帧普通数据必须触发完整配置重下发，否则 actual 永远未知，
 * 新的 GNSS 来源门控会永久拒绝定位。 */
static void test_link_data_reapplies_config_when_p4_missed_hello(void)
{
    g_apply_started = g_apply_confirmed = g_apply_failed = 0;
    g_config_pending = 0;
    g_actual_known = false;
    g_requested_gnss = PK_ANT_GNSS_EXTERNAL;

    adsb_link_msg_t health = {
        .type = ADSB_LINK_MSG_HEALTH_STATS,
        .payload_len = 40,
    };
    pk_adsb_link_test_dispatch(&health);
    CHECK(g_apply_started == 1 && g_config_pending == 1,
          "已 linked 的 RP 首帧数据应补发配置，不能依赖 HELLO\n");

    pk_adsb_link_test_dispatch(&health);
    CHECK(g_apply_started == 1,
          "等待 ACK 期间普通数据不得重复塞入配置请求\n");

    /* UART 协议不重传；ACK 丢失后若永远把 pending 当成活请求，GNSS 会永久
     * 拒绝定位。持续收到 HEALTH 证明 RP 在线，超时后必须清会话并重发。 */
    g_now_us += 3000000;
    pk_adsb_link_test_dispatch(&health);
    CHECK(g_apply_started == 2 && g_config_pending == 1,
          "CONFIG_ACK 丢失后应在普通在线帧驱动下超时重发\n");

    dispatch_ack(3, 0);
    CHECK(g_apply_confirmed == 1 && g_actual_known && g_config_pending == 0,
          "补发配置的 3/0 ACK 应确认实际 GNSS 来源\n");
}

int main(void)
{
    /* adsb_link_task.c registers this production sink with modes_ingest_init()
     * in its task loop.  The host cannot run that loop, so expose the same
     * registration directly through the production initialization function. */
    modes_ingest_init(on_ingest_msg, NULL);
    test_df18_identity_reaches_traffic_record();
    test_nonicao_df18_surface_does_not_create_target();
    test_tc20_position_reaches_airborne_cpr();
    test_sweep_table();
    test_sweep_running_state();
    test_gnss_actual_source_requires_matching_ack();
    test_link_data_reapplies_config_when_p4_missed_hello();

    if (g_fail) {
        printf("FAILED: %d check(s)\n", g_fail);
        return 1;
    }
    printf("OK: all ADS-B link dispatch checks passed\n");
    return 0;
}
