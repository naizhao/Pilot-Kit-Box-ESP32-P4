/*
 * test_own_ship_datum.c — 本机高度基准不得混用（Phase 1 独立复审 P1/A）。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/test/host_stubs -I firmware/main \
 *      -o /tmp/test_own_ship_datum firmware/test/test_own_ship_datum.c \
 *      firmware/main/own_ship.c firmware/main/gps_task.c \
 *      firmware/main/gps_nmea.c firmware/main/pk_clock.c \
 *      firmware/main/gdl90.c firmware/main/traffic_geom.c \
 *      firmware/main/geo.c -lm \
 *   && /tmp/test_own_ship_datum
 *
 * 缺陷（修复前）
 * --------------
 * 三种高度在这条链路上是三个**互不可换**的量：
 *   - 气压高度（1013.25 标准基准）：ADS-B 目标的 Mode-C/DF17 高度、GDL90
 *     0x0A/0x14 的高度字段，唯一能与目标相减的量；
 *   - GNSS 正高 MSL：GGA 第 9 字段给的（gps.h:49），与气压高度差着当地
 *     气压偏差，非标准日下地面就能差上千英尺；
 *   - GNSS 椭球高：ADS-B TC20-22 给的（aircraft_state.h:93），与 MSL 差
 *     一个大地水准面起伏。
 *
 * own_ship.c 的 GPS 兜底把 GGA 的 MSL 写进了 aircraft_t.altitude_ft
 * ——那个字段的定义是"barometric pressure altitude"——于是：
 *   1. ble_gatt.c 把它当压力高度编进 GDL90 Ownship(0x0A)，EFB 拿它跟别的
 *      飞机的压力高度比；
 *   2. traffic_page / adsb_list / pfd_hsi_traffic 在没有绑定 ADS-B 本机时
 *      退回**舱内** BMP388 的标准气压高度当基准。增压座舱里那个数恒等于
 *      座舱高度（约 8000 ft），于是巡航在 FL350、目标也在 FL350 时，屏上
 *      算出 +27000 ft —— 同高度的迎头目标被显示成远在高空、告警被抑制。
 *      spec §4.2 原文就写着「气压高度在增压环境不可信，不替代 ADS-B /
 *      绑定飞机的高度作为权威源」。
 *
 * 判据（跑生产链路本身：NMEA 行 → gps_task 解析 → own_ship 解算 → 线字节）
 * ----------------------------------------------------------------------
 *   1. GPS-only 且有 GGA 高度时，resolved ownship 的**压力高度必须无效**，
 *      MSL 单独出现在 pk_own_alt_t 里，不许挤进 aircraft_t 的压力/椭球字段；
 *   2. 该 ownship 编出来的 GDL90 0x0A，高度字段必须是 0xFFF（未知），
 *      不是 1789 ft；
 *   3. 绑定 ADS-B 本机时压力高度照常出现在 0x0A 里（不能把功能修没）；
 *   4. 增压舱 8000 ft + 目标 FL350 + 无可信本机压力高度 → 相对高度
 *      **不可用**，而不是 +27000 ft。
 *
 * 不在本文件范围内
 * ----------------
 * GDL90 Ownship Geometric Altitude(0x0B) 仍是**未实现的登记决策点**：
 * 仓库内没有 FAA 560-1058-00 的线格式原文（hardware/datasheets/ 无此件），
 * 参考实现 tmp/adsbee 也只声明了消息 ID、没有编码器。凭记忆写一个高度帧
 * 的代价是 EFB 上一个看起来正常的错高度，所以本轮只把 0x0A 修成"安全的
 * 未知"，0x0B 留待拿到规范原文后另做 golden vector。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aircraft_state.h"
#include "gdl90.h"
#include "gps.h"
#include "own_ship.h"
#include "pk_flight_phase.h"
#include "traffic_geom.h"

/* ── 断言 ─────────────────────────────────────────────────────────── */
static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("         at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

/* ── 注入时钟 ─────────────────────────────────────────────────────── */
static int64_t g_now_us;
int64_t esp_timer_get_time(void) { return g_now_us; }

/* ── 协作者桩 ─────────────────────────────────────────────────────── */
bool pk_demo_enabled(void) { return false; }
bool pk_demo_gps(int64_t now_us, pk_gps_state_t *out)
{ (void)now_us; (void)out; return false; }

/* 绑定的本机（ADS-B）。g_bound_icao=0 表示未绑定 → own_ship 走 GPS 兜底。 */
static uint32_t   g_bound_icao;
static aircraft_t g_bound_ac;
static bool       g_bound_fresh;

uint32_t pk_ui_get_own_icao(void) { return g_bound_icao; }

bool aircraft_state_get_own(uint32_t icao24, int64_t now_us,
                            int64_t max_age_us, aircraft_t *out)
{
    (void)now_us; (void)max_age_us;
    if (!g_bound_fresh || icao24 == 0 || icao24 != g_bound_ac.icao24) return false;
    *out = g_bound_ac;
    return true;
}

/* 相位：本文件不测 A/G（那是 test_own_ship_gps_validity.c 的事），
 * 固定给"在空中"，免得高度断言被 A/G 策略牵连。 */
static pk_flight_phase_t g_phase = PK_PHASE_AIRBORNE;
pk_flight_phase_t pk_own_sampler_get_phase(void) { return g_phase; }

/* ── NMEA 喂线（与 test_gps_fix_freshness.c 同一套） ───────────────── */
static void feed(const char *body)
{
    char line[128];
    unsigned char ck = 0;
    for (const char *p = body; *p; ++p) ck ^= (unsigned char)*p;
    snprintf(line, sizeof(line), "$%s*%02X", body, ck);
    pk_gps_feed_line(line);
}

static void feed_valid_rmc(void)
{
    feed("GPRMC,123519,A,3100.0000,N,12100.0000,E,120.0,90.0,230326,,");
}

/* GGA q=1、545.4 m → 1789 ft MSL（换算与 test_gps_fix_freshness.c 同源）。 */
static void feed_gga_545m(void)
{
    feed("GPGGA,123519,3100.0000,N,12100.0000,E,1,08,0.9,545.4,M,46.9,M,,");
}

static void reset_world(void)
{
    g_now_us = 100000000;
    g_bound_icao = 0;
    g_bound_fresh = false;
    memset(&g_bound_ac, 0, sizeof(g_bound_ac));
    g_phase = PK_PHASE_AIRBORNE;
    pk_gps_state_init();
}

/* ── GDL90 0x0A 线字节解码（只取本测试要的字段） ───────────────────── *
 *
 * 帧结构见 gdl90.c：0x7E | id | payload(27) | CRC16 | 0x7E，且 0x7E/0x7D
 * 会被字节填充。这里先反填充再取 payload。 */
typedef struct {
    bool     ok;
    uint8_t  id;
    uint16_t alt_enc;      /* 12 bit；0xFFF = 未知 */
    uint8_t  misc;         /* 低 4 bit：bit3 airborne，bit1..0 = tt */
    uint16_t horiz_vel;    /* 12 bit；0xFFF = 未知 */
    uint8_t  track_byte;
} gdl90_report_t;

static gdl90_report_t decode_report(const uint8_t *frame, size_t n)
{
    gdl90_report_t r;
    memset(&r, 0, sizeof(r));
    if (n < 4 || frame[0] != 0x7E || frame[n - 1] != 0x7E) return r;

    uint8_t body[64];
    size_t  bn = 0;
    for (size_t i = 1; i + 1 < n; ++i) {
        uint8_t b = frame[i];
        if (b == 0x7D) { ++i; b = (uint8_t)(frame[i] ^ 0x20); }
        if (bn < sizeof(body)) body[bn++] = b;
    }
    if (bn < 1 + 27 + 2) return r;

    const uint8_t *p = body + 1;      /* payload 起点（跳过 msg id） */
    r.ok         = true;
    r.id         = body[0];
    r.alt_enc    = (uint16_t)(((uint16_t)p[10] << 4) | (p[11] >> 4));
    r.misc       = (uint8_t)(p[11] & 0x0F);
    r.horiz_vel  = (uint16_t)(((uint16_t)p[13] << 4) | (p[14] >> 4));
    r.track_byte = p[16];
    return r;
}

/* resolved ownship → GDL90 0x0A，与 ble_gatt.c 的实参一一对应。 */
static gdl90_report_t encode_ownship(const aircraft_t *own)
{
    uint8_t frame[64];
    size_t n = gdl90_encode_traffic(frame, sizeof(frame),
        /*is_ownship=*/true, own->icao24,
        own->have_position, own->lat, own->lon,
        own->have_altitude, own->altitude_ft,
        own->have_ground_speed, own->ground_speed_kt,
        own->have_heading, own->heading_deg,
        own->have_vertical_rate, own->vert_rate_fpm,
        own->have_air_ground, own->on_ground,
        gdl90_emitter_from_wake((int)own->wake),
        "", sizeof(""));
    return decode_report(frame, n);
}

/* ════════════════════════════════════════════════════════════════════
 * 1. GPS-only：GGA 的 MSL 不得变成压力高度
 * ════════════════════════════════════════════════════════════════════ */
static void test_gps_only_msl_is_not_pressure_altitude(void)
{
    reset_world();
    feed_valid_rmc();
    feed_gga_545m();

    aircraft_t   own;
    pk_own_src_t src;
    pk_own_alt_t alt;
    bool ok = pk_own_ship_resolve_ex(g_now_us, 5000000, &own, &src, &alt);

    CHECK(ok, "GPS 有定位时应解算出本机\n");
    CHECK(src == PK_OWN_SRC_GPS, "来源应为 GPS got=%d\n", (int)src);

    /* 核心：压力高度必须无效——GPS 给不出气压高度。 */
    CHECK(!own.have_altitude,
          "GPS 兜底不得声明压力高度有效（altitude_ft=%d）\n", own.altitude_ft);
    CHECK(!alt.have_press_alt, "pk_own_alt_t.have_press_alt 必须为假\n");

    /* MSL 必须单独出现，且不得挤进 TC20-22 的椭球高字段。 */
    CHECK(alt.have_gnss_msl, "GGA 有高度时 have_gnss_msl 应为真\n");
    CHECK(alt.gnss_msl_ft == 1789, "MSL 应为 1789 ft got=%d\n", alt.gnss_msl_ft);
    CHECK(!alt.have_gnss_ellipsoid,
          "GPS 兜底没有椭球高（那是 ADS-B TC20-22 的量），不得置位\n");
    CHECK(!own.have_gnss_altitude,
          "不得把 MSL 偷塞进 aircraft_t 的 GNSS 椭球高字段（值=%d）\n",
          own.gnss_altitude_ft);
}

/* 2. 线上：没有真实压力高度时 0x0A 的高度字段必须是 0xFFF。 */
static void test_gps_only_ownship_wire_altitude_is_unknown(void)
{
    reset_world();
    feed_valid_rmc();
    feed_gga_545m();

    aircraft_t own; pk_own_src_t src;
    CHECK(pk_own_ship_resolve(g_now_us, 5000000, &own, &src), "前置：应有本机\n");

    gdl90_report_t r = encode_ownship(&own);
    CHECK(r.ok, "0x0A 帧解码失败\n");
    CHECK(r.id == GDL90_ID_OWNSHIP, "msg id 应为 0x0A got=0x%02X\n", r.id);
    CHECK(r.alt_enc == 0xFFF,
          "无真实压力高度时线上高度必须是 0xFFF，got=0x%03X（"
          "= %d ft，正是被当成压力高度的 GPS MSL）\n",
          r.alt_enc, (int)r.alt_enc * 25 - 1000);
}

/* 3. 绑定 ADS-B 本机：压力高度照常上线（别把功能修没）。 */
static void test_bound_adsb_pressure_altitude_still_encoded(void)
{
    reset_world();
    g_bound_icao = 0x780ABC;
    g_bound_fresh = true;
    g_bound_ac.icao24        = 0x780ABC;
    g_bound_ac.have_position = true;
    g_bound_ac.lat = 31.0; g_bound_ac.lon = 121.0;
    g_bound_ac.have_altitude = true;
    g_bound_ac.altitude_ft   = 35000;
    g_bound_ac.have_gnss_altitude = true;
    g_bound_ac.gnss_altitude_ft   = 35120;   /* TC20-22 椭球高，故意与压力高度不同 */

    aircraft_t   own;
    pk_own_src_t src;
    pk_own_alt_t alt;
    CHECK(pk_own_ship_resolve_ex(g_now_us, 5000000, &own, &src, &alt),
          "绑定本机应解算成功\n");
    CHECK(src == PK_OWN_SRC_BOUND_ADSB, "来源应为绑定 ADS-B\n");
    CHECK(alt.have_press_alt && alt.press_alt_ft == 35000,
          "绑定本机的压力高度应为 35000 got(valid=%d, %d)\n",
          (int)alt.have_press_alt, alt.press_alt_ft);
    CHECK(alt.have_gnss_ellipsoid && alt.gnss_ellipsoid_ft == 35120,
          "TC20-22 椭球高应原样带出 got(valid=%d, %d)\n",
          (int)alt.have_gnss_ellipsoid, alt.gnss_ellipsoid_ft);
    CHECK(!alt.have_gnss_msl, "ADS-B 给不出 MSL，不得置位\n");

    gdl90_report_t r = encode_ownship(&own);
    /* (35000 + 1000) / 25 = 1440 */
    CHECK(r.ok && r.alt_enc == 1440,
          "绑定本机时线上高度应为 1440(=35000ft) got=0x%03X\n", r.alt_enc);
}

/* ════════════════════════════════════════════════════════════════════
 * 4. 增压舱：舱内 BMP388 绝不能当相对高度基准
 * ════════════════════════════════════════════════════════════════════ */
static void test_pressurized_cabin_relative_altitude_unavailable(void)
{
    /* 场景：巡航 FL350，舱压等效 8000 ft，未绑定 ADS-B 本机。
     * 目标也在 FL350（同高度迎头）。 */
    const int cabin_baro_ft = 8000;
    const int tgt_alt_ft    = 35000;

    int own_palt = pk_traffic_own_press_alt(/*own_bound_adsb=*/false,
                                            /*own_has_press_alt=*/false,
                                            /*own_press_alt_ft=*/cabin_baro_ft);
    CHECK(own_palt == PK_ALT_UNAVAIL,
          "未绑定 ADS-B 本机时相对高度基准必须不可用，got=%d\n", own_palt);

    pk_traffic_rel_t rel = pk_traffic_rel_calc(
        /*own_has_pos=*/true, 31.0, 121.0, /*own_hdg=*/0.0f, /*mag_var=*/0.0f,
        own_palt,
        /*tgt_has_pos=*/true, 31.1, 121.0,
        /*tgt_has_alt=*/true, tgt_alt_ft,
        /*tgt_has_vs=*/false, 0);

    CHECK(!rel.rel_alt_valid,
          "增压舱内不得算出相对高度（本该 N/A，got %+d ft）\n", rel.rel_alt_ft);
    CHECK(rel.rel_alt_ft != tgt_alt_ft - cabin_baro_ft,
          "相对高度等于 目标−舱压高度 = %+d ft，正是要修的缺陷\n",
          tgt_alt_ft - cabin_baro_ft);
}

/* 5. 绑定本机时相对高度照常可用（同基准相减）。 */
static void test_bound_ownship_relative_altitude_available(void)
{
    int own_palt = pk_traffic_own_press_alt(/*own_bound_adsb=*/true,
                                            /*own_has_press_alt=*/true,
                                            /*own_press_alt_ft=*/35000);
    CHECK(own_palt == 35000, "绑定本机的基准应是它的压力高度 got=%d\n", own_palt);

    pk_traffic_rel_t rel = pk_traffic_rel_calc(
        true, 31.0, 121.0, 0.0f, 0.0f, own_palt,
        true, 31.1, 121.0, true, 35000, false, 0);
    CHECK(rel.rel_alt_valid, "绑定本机时相对高度应可用\n");
    CHECK(rel.rel_alt_ft == 0, "同高度目标相对高度应为 0 got=%+d\n", rel.rel_alt_ft);
}

/* 6. 绑定本机但它自己没有压力高度（只发过 DF11）→ 仍然不可用。 */
static void test_bound_without_pressure_altitude_is_unavailable(void)
{
    int own_palt = pk_traffic_own_press_alt(true, false, 8000);
    CHECK(own_palt == PK_ALT_UNAVAIL,
          "绑定本机但无压力高度时基准必须不可用 got=%d\n", own_palt);
}

int main(void)
{
    test_gps_only_msl_is_not_pressure_altitude();
    test_gps_only_ownship_wire_altitude_is_unknown();
    test_bound_adsb_pressure_altitude_still_encoded();
    test_pressurized_cabin_relative_altitude_unavailable();
    test_bound_ownship_relative_altitude_available();
    test_bound_without_pressure_altitude_is_unavailable();

    if (g_fail) {
        printf("test_own_ship_datum: %d assertion(s) FAILED\n", g_fail);
        return 1;
    }
    printf("test_own_ship_datum: all passed\n");
    return 0;
}
