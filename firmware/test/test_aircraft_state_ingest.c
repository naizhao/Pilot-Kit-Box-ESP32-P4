/*
 * test_aircraft_state_ingest.c — raw Mode-S frame through the production
 * modes_ingest sink into aircraft_state, then snapshot observable state.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/test/host_stubs -I firmware/main \
 *      -o /tmp/test_aircraft_state_ingest \
 *      firmware/test/test_aircraft_state_ingest.c \
 *      firmware/main/mode_s.c firmware/main/modes_ingest.c \
 *      firmware/main/aircraft_state.c firmware/main/pk_callsign.c -lm \
 *   && /tmp/test_aircraft_state_ingest
 *
 * ESP/FreeRTOS stubs only replace locking/logging and demo/UI externals.  The
 * decoder, CRC gate, ingest dispatch, aggregation table and snapshot are the
 * production implementations.
 */
#include "aircraft_state.h"
#include "demo_data.h"
#include "mode_s.h"
#include "modes_ingest.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* 演示模式默认关；只有 test_demo_snapshot_keeps_synthetic_fields 会打开它。
 * 合成目标是 pk_demo_traffic 按 now_us 现算的，字段本身永远是新鲜的，"旧"只
 * 体现在 last_seen_us（目标级）——快照的字段级过期不得把它们误伤掉。 */
static bool g_demo_on;
static int64_t g_demo_last_seen_age_s;

bool pk_demo_enabled(void) { return g_demo_on; }
float pk_demo_yaw_deg(int64_t now_us) { (void)now_us; return 0.0f; }
int pk_demo_own_alt_ft(int64_t now_us) { (void)now_us; return 0; }
size_t pk_demo_traffic(aircraft_t *out, size_t cap, int64_t now_us,
                       int64_t anim_us, float own_yaw_deg, int own_alt_ft,
                       float extra_dist_nm, bool bare)
{
    (void)anim_us; (void)own_yaw_deg; (void)own_alt_ft;
    (void)extra_dist_nm; (void)bare;
    if (!g_demo_on || cap == 0) return 0;
    memset(out, 0, sizeof(*out));
    out->icao24            = 0x3C0000;
    out->last_seen_us      = now_us - g_demo_last_seen_age_s * 1000000LL;
    out->have_callsign     = true;
    memcpy(out->callsign, "DEMO1", 6);
    out->wake              = PK_WAKE_LARGE;
    out->have_altitude     = true;  out->altitude_ft     = 3000;
    out->have_position     = true;  out->lat = 31.0; out->lon = 121.0;
    out->have_ground_speed = true;  out->ground_speed_kt = 210;
    out->have_heading      = true;  out->heading_deg     = 175;
    out->have_velocity     = true;
    out->have_vertical_rate = true; out->vert_rate_fpm   = 800;
    out->have_squawk       = true;  out->squawk          = 7700;
    return 1;
}
uint32_t pk_ui_get_own_icao(void) { return 0; }

static int g_fail;
static int g_sink_calls;
static int64_t g_now_us;

#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("        at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)
#define CHECK_EQ_I(got, want, label) \
    CHECK((got) == (want), "%s got=%d want=%d\n", label, (int)(got), (int)(want))

static void state_sink(const struct mode_s_msg *mm,
                       const modes_ingest_meta_t *meta, void *user)
{
    (void)meta; (void)user;
    ++g_sink_calls;
    aircraft_state_ingest(mm, g_now_us++);
}

static void from_hex(const char *hex, unsigned char msg[14])
{
    for (int i = 0; i < 14; ++i) {
        unsigned int value = 0;
        (void)sscanf(hex + i * 2, "%2x", &value);
        msg[i] = (unsigned char)value;
    }
}

static void set_crc(unsigned char msg[14])
{
    uint32_t crc = mode_s_checksum(msg, 112);
    msg[11] = (unsigned char)(crc >> 16);
    msg[12] = (unsigned char)(crc >> 8);
    msg[13] = (unsigned char)crc;
}

/* 56-bit frames carry the parity in bytes 4..6. */
static void set_crc56(unsigned char msg[14])
{
    uint32_t crc = mode_s_checksum(msg, 56);
    msg[4] = (unsigned char)(crc >> 16);
    msg[5] = (unsigned char)(crc >> 8);
    msg[6] = (unsigned char)crc;
}

/* DFs with an Address/Parity field carry AP = CRC xor ICAO in the last three
 * bytes; the decoder recovers the address by xoring the recomputed CRC back
 * out (mode_s.c brute_force_ap), so the address must already be in the
 * recently-seen cache from a DF11/DF17 frame. */
static void set_crc_ap(unsigned char msg[14], uint32_t icao)
{
    uint32_t crc = mode_s_checksum(msg, 112);
    msg[11] = (unsigned char)((crc >> 16) ^ (icao >> 16));
    msg[12] = (unsigned char)((crc >> 8)  ^ (icao >> 8));
    msg[13] = (unsigned char)(crc         ^ icao);
}

static void build_df11(unsigned char msg[14], uint32_t icao)
{
    memset(msg, 0, 14);
    msg[0] = (unsigned char)(11 << 3);      /* CA = 0 */
    msg[1] = (unsigned char)(icao >> 16);
    msg[2] = (unsigned char)(icao >> 8);
    msg[3] = (unsigned char)icao;
    set_crc56(msg);
}

/* DF20 Comm-B altitude reply.  AC13 code 0x3B0 = Q=1, n=240 → 240*25-1000
 * = 5000 ft (mode_s.c decode_altitude_code).  Like DF21 it carries an AP
 * field, so the address must already be in the recently-seen cache. */
static void build_df20(unsigned char msg[14], uint32_t icao)
{
    memset(msg, 0, 14);
    msg[0] = (unsigned char)(20 << 3);
    msg[2] = 0x03;                          /* AC13 高 5 位 */
    msg[3] = 0xB0;                          /* AC13 低 8 位 */
    set_crc_ap(msg, icao);
}

/* DF21 Comm-B identity reply — the only squawk source alongside DF5. */
static void build_df21(unsigned char msg[14], uint32_t icao)
{
    memset(msg, 0, 14);
    msg[0] = (unsigned char)(21 << 3);
    msg[2] = 0x0A;                          /* arbitrary identity bits */
    msg[3] = 0x05;
    set_crc_ap(msg, icao);
}

static void build_air_position(unsigned char msg[14], uint32_t icao,
                               int tc, unsigned int altitude_raw)
{
    memset(msg, 0, 14);
    msg[0] = (unsigned char)(17 << 3);
    msg[1] = (unsigned char)(icao >> 16);
    msg[2] = (unsigned char)(icao >> 8);
    msg[3] = (unsigned char)icao;
    msg[4] = (unsigned char)(tc << 3);
    msg[5] = (unsigned char)(altitude_raw >> 4);
    msg[6] = (unsigned char)((altitude_raw & 0x0F) << 4);
    set_crc(msg);
}

static void build_df18(unsigned char msg[14], int cf, int imf, uint32_t address)
{
    memset(msg, 0, 14);
    msg[0] = (unsigned char)((18 << 3) | (cf & 7));
    msg[1] = (unsigned char)(address >> 16);
    msg[2] = (unsigned char)(address >> 8);
    msg[3] = (unsigned char)address;
    /* Fine airborne-position TC11 carries IMF at ME bit 8 (msg[4] bit0). */
    msg[4] = (unsigned char)((11 << 3) | (imf & 1));
    msg[5] = 0x58;
    set_crc(msg);
}

static void build_surface(unsigned char msg[14], uint32_t icao, int movement,
                          int track_valid, int track)
{
    memset(msg, 0, 14);
    msg[0] = (unsigned char)(17 << 3);
    msg[1] = (unsigned char)(icao >> 16);
    msg[2] = (unsigned char)(icao >> 8);
    msg[3] = (unsigned char)icao;
    msg[4] = (unsigned char)((7 << 3) | ((movement >> 4) & 7));
    msg[5] = (unsigned char)(((movement & 15) << 4) |
                             ((track_valid & 1) << 3) | ((track >> 4) & 7));
    msg[6] = (unsigned char)((track & 15) << 4);
    set_crc(msg);
}

static bool get_aircraft(uint32_t icao, aircraft_t *out)
{
    aircraft_t all[AIRCRAFT_TABLE_CAPACITY];
    size_t n = aircraft_state_snapshot(all, AIRCRAFT_TABLE_CAPACITY,
                                       g_now_us + 1000, 1000000);
    for (size_t i = 0; i < n; ++i) {
        if (all[i].icao24 == icao) { *out = all[i]; return true; }
    }
    return false;
}

static void reset_chain(void)
{
    g_sink_calls = 0;
    g_now_us = 1000000;
    aircraft_state_init();
    modes_ingest_init(state_sink, NULL);
}

static void test_velocity_vectors_reach_state(void)
{
    unsigned char msg[14];
    aircraft_t a;
    reset_chain();

    from_hex("8D485020994409940838175B284F", msg);
    modes_ingest_feed(msg, 112, NULL);
    CHECK(get_aircraft(0x485020, &a), "subtype1 aircraft missing\n");
    CHECK_EQ_I(a.ground_speed_kt, 159, "subtype1 state ground speed");
    CHECK_EQ_I(a.heading_deg, 183, "subtype1 state rounded track");
    CHECK_EQ_I(a.vert_rate_fpm, -832, "subtype1 state vertical rate");
    CHECK(a.have_ground_speed, "subtype1 ground-speed validity missing\n");
    CHECK(a.have_heading, "subtype1 heading validity missing\n");
    CHECK(a.have_vertical_rate, "subtype1 vertical-rate validity missing\n");
    CHECK(a.have_velocity, "subtype1 state velocity must be valid\n");

    from_hex("8DA05F219B06B6AF189400CBC33F", msg);
    modes_ingest_feed(msg, 112, NULL);
    CHECK(get_aircraft(0xA05F21, &a), "subtype3 aircraft missing\n");
    CHECK(!a.have_velocity,
          "subtype3 airspeed must not masquerade as GDL90 ground speed\n");
    CHECK(a.have_airspeed, "subtype3 airspeed validity missing\n");
    CHECK_EQ_I(a.airspeed_kt, 375, "subtype3 state airspeed");
    CHECK(!a.have_heading, "subtype3 air heading must not become ground track\n");
    CHECK(a.have_airspeed_heading, "subtype3 air-heading validity missing\n");
    CHECK_EQ_I(a.airspeed_heading_deg, 244, "subtype3 rounded air heading");
    CHECK(a.have_vertical_rate, "subtype3 vertical-rate validity missing\n");
    CHECK_EQ_I(a.vert_rate_fpm, -2304, "subtype3 state vertical rate");

    /* A later explicit N/A must invalidate the corresponding state.  Keeping
     * it valid would refresh stale data via the aircraft-level timestamp. */
    msg[5] &= (unsigned char)~0x04; /* heading status = unavailable */
    msg[7] &= 0x80; msg[8] &= 0x07; /* airspeed and VR high bits = 0 */
    msg[8] &= 0xF8; msg[9] &= 0x03;
    set_crc(msg);
    modes_ingest_feed(msg, 112, NULL);
    CHECK(get_aircraft(0xA05F21, &a), "subtype3 N/A aircraft missing\n");
    CHECK(!a.have_airspeed, "N/A must invalidate prior airspeed\n");
    CHECK(!a.have_airspeed_heading, "N/A must invalidate prior air heading\n");
    CHECK(!a.have_vertical_rate, "N/A must invalidate prior vertical rate\n");
    CHECK_EQ_I(a.airspeed_kt,          0, "airspeed N/A 必须把值也清零");
    CHECK_EQ_I(a.airspeed_heading_deg, 0, "air-heading N/A 必须把值也清零");
    CHECK_EQ_I(a.vert_rate_fpm,        0, "subtype3 VR N/A 必须把值也清零");
}

static void test_tc19_na_invalidates_stale_ground_vector(void)
{
    unsigned char msg[14];
    aircraft_t a;
    reset_chain();

    from_hex("8D485020994409940838175B284F", msg);
    modes_ingest_feed(msg, 112, NULL);
    msg[5] &= 0xFC; msg[6] = 0;       /* E/W component N/A. */
    msg[8] &= 0xF8; msg[9] &= 0x03;  /* Vertical rate N/A. */
    set_crc(msg);
    modes_ingest_feed(msg, 112, NULL);

    CHECK(get_aircraft(0x485020, &a), "ground-vector N/A aircraft missing\n");
    CHECK(!a.have_ground_speed, "component N/A must invalidate ground speed\n");
    CHECK(!a.have_heading, "component N/A must invalidate ground track\n");
    CHECK(!a.have_velocity, "component N/A must invalidate ground vector\n");
    CHECK(!a.have_vertical_rate, "VR N/A must invalidate vertical rate\n");
    /* 明确的 N/A 必须连值一起清掉，与按龄过期留下同一种残骸。留着 -832 的
     * 后果具体而可见：pk_traffic_rel_calc 之外任何漏检 have_* 的读法都会画出
     * 一支笃定的下降箭头。 */
    CHECK_EQ_I(a.vert_rate_fpm,    0, "VR N/A 必须把值也清零");
    CHECK_EQ_I(a.ground_speed_kt,  0, "地速 N/A 必须把值也清零");
    CHECK_EQ_I(a.heading_deg,      0, "航迹 N/A 必须把值也清零");
}

/* Regression: subtype 3/4 carries magnetic/true air-heading plus IAS/TAS,
 * not the ground track consumed by map/GDL90.  Re-address the captured
 * subtype-3 payload to the subtype-1 ICAO so both frames hit one real fusion
 * slot; its previous ground vector must remain coherent. */
static void test_airspeed_heading_does_not_replace_ground_track(void)
{
    unsigned char msg[14];
    aircraft_t a;
    reset_chain();

    from_hex("8D485020994409940838175B284F", msg);
    modes_ingest_feed(msg, 112, NULL);

    from_hex("8DA05F219B06B6AF189400CBC33F", msg);
    msg[1] = 0x48; msg[2] = 0x50; msg[3] = 0x20;
    set_crc(msg);
    modes_ingest_feed(msg, 112, NULL);

    CHECK(get_aircraft(0x485020, &a), "combined velocity aircraft missing\n");
    CHECK(a.have_velocity, "prior complete ground vector must remain valid\n");
    CHECK_EQ_I(a.ground_speed_kt, 159, "prior ground speed preserved");
    CHECK_EQ_I(a.heading_deg, 183, "air heading must not replace ground track");
    CHECK(a.have_airspeed, "subtype3 airspeed must still be retained\n");
    CHECK_EQ_I(a.airspeed_kt, 375, "subtype3 airspeed retained");
    CHECK(a.have_airspeed_heading, "subtype3 air heading must be retained\n");
    CHECK_EQ_I(a.airspeed_heading_deg, 244, "subtype3 air heading retained");
}

static void test_surface_composite_validity(void)
{
    unsigned char msg[14];
    aircraft_t a;
    reset_chain();

    build_surface(msg, 0x510001, 39, 0, 64);
    modes_ingest_feed(msg, 112, NULL);
    CHECK(get_aircraft(0x510001, &a), "movement-only surface target missing\n");
    CHECK(a.have_ground_speed, "movement-only ground speed missing\n");
    CHECK(!a.have_heading, "S=0 must not create heading\n");
    CHECK(!a.have_velocity, "movement-only must not satisfy composite velocity\n");

    build_surface(msg, 0x510002, 0, 1, 64);
    modes_ingest_feed(msg, 112, NULL);
    CHECK(get_aircraft(0x510002, &a), "track-only surface target missing\n");
    CHECK(!a.have_ground_speed, "MOV=0 must not create ground speed\n");
    CHECK(a.have_heading, "track-only heading missing\n");
    CHECK(!a.have_velocity, "track-only must not satisfy composite velocity\n");
}

static void test_surface_na_invalidates_prior_components(void)
{
    unsigned char msg[14];
    aircraft_t a;
    reset_chain();

    /* MOV=0 explicitly supersedes the prior movement while the new track is
     * still valid.  Aircraft-level last_seen must not make the old speed look
     * fresh. */
    build_surface(msg, 0x510101, 39, 1, 64);
    modes_ingest_feed(msg, 112, NULL);
    build_surface(msg, 0x510101, 0, 1, 80);
    modes_ingest_feed(msg, 112, NULL);
    CHECK(get_aircraft(0x510101, &a), "surface MOV N/A target missing\n");
    CHECK(!a.have_ground_speed, "MOV=0 must invalidate prior ground speed\n");
    CHECK_EQ_I(a.ground_speed_kt, 0, "地面 MOV=0 必须把地速值也清零");
    CHECK(a.have_heading, "valid track must survive MOV=0\n");
    CHECK_EQ_I(a.heading_deg, 225, "new surface track retained");
    CHECK(!a.have_velocity, "MOV=0 must invalidate composite velocity\n");

    /* S=0 explicitly supersedes the prior track while movement remains valid. */
    build_surface(msg, 0x510102, 39, 1, 64);
    modes_ingest_feed(msg, 112, NULL);
    build_surface(msg, 0x510102, 40, 0, 80);
    modes_ingest_feed(msg, 112, NULL);
    CHECK(get_aircraft(0x510102, &a), "surface track N/A target missing\n");
    CHECK(a.have_ground_speed, "valid movement must survive S=0\n");
    CHECK(!a.have_heading, "S=0 must invalidate prior surface track\n");
    CHECK(!a.have_velocity, "S=0 must invalidate composite velocity\n");
    CHECK_EQ_I(a.heading_deg, 0, "地面 S=0 必须把航迹值也清零");
}

static void test_valid_zero_and_gnss_altitude_are_distinct(void)
{
    const uint32_t icao = 0x7C6296;
    unsigned char msg[14];
    aircraft_t a;
    reset_chain();

    /* Q=0 Gillham 000000011010 (AC12 0x20A) is a valid zero feet. */
    build_air_position(msg, icao, 9, 0x20A);
    modes_ingest_feed(msg, 112, NULL);
    CHECK(get_aircraft(icao, &a), "zero-foot aircraft missing\n");
    CHECK(a.have_altitude, "valid 0 ft must set have_altitude\n");
    CHECK_EQ_I(a.altitude_ft, 0, "valid zero-foot baro altitude");

    /* Establish a non-zero pressure altitude, then receive captured TC20
     * GNSS HAE=2570 m = 8431 ft for the same ICAO.  GNSS must be retained
     * separately and must not replace the barometric datum used by GDL90. */
    build_air_position(msg, icao, 9, 0x1F0); /* Q=1: 5000 ft. */
    modes_ingest_feed(msg, 112, NULL);
    from_hex("8E7C6296A0A0A59C64468D6F4EDD", msg);
    modes_ingest_feed(msg, 112, NULL);
    CHECK(get_aircraft(icao, &a), "TC20 aircraft missing\n");
    CHECK_EQ_I(a.altitude_ft, 5000, "TC20 must preserve pressure altitude");
    CHECK(a.have_gnss_altitude, "TC20 GNSS HAE must be retained\n");
    CHECK_EQ_I(a.gnss_altitude_ft, 8431, "TC20 GNSS HAE feet");
}

static void test_df18_address_semantics(void)
{
    unsigned char msg[14];
    aircraft_t a;
    reset_chain();

    build_df18(msg, 0, 0, 0xA00000);
    modes_ingest_feed(msg, 112, NULL);
    CHECK(get_aircraft(0xA00000, &a), "DF18 CF0 ICAO target missing\n");

    /* CF2 fine TIS-B and CF6 ADS-R use IMF=0 for ICAO, IMF=1 for Mode-A /
     * anonymous addresses.  Both ICAO forms must aggregate. */
    const int icao_cfs[] = {2, 6};
    for (size_t i = 0; i < sizeof(icao_cfs) / sizeof(icao_cfs[0]); ++i) {
        uint32_t addr = 0xA10000u + (uint32_t)icao_cfs[i];
        build_df18(msg, icao_cfs[i], 0, addr);
        modes_ingest_feed(msg, 112, NULL);
        CHECK(get_aircraft(addr, &a), "DF18 CF%d IMF0 ICAO target missing\n",
              icao_cfs[i]);

        addr += 0x100;
        build_df18(msg, icao_cfs[i], 1, addr);
        modes_ingest_feed(msg, 112, NULL);
        CHECK(!get_aircraft(addr, &a), "DF18 CF%d IMF1 polluted ICAO table\n",
              icao_cfs[i]);
    }

    /* CF1/5 are explicitly non-ICAO. CF3 is a different coarse format,
     * CF4 is management and CF7 reserved. Every CRC-valid frame reaches the
     * transport sink, but none may create an ICAO-keyed aircraft slot. */
    const int cfs[] = {1, 3, 4, 5, 7};
    for (size_t i = 0; i < sizeof(cfs) / sizeof(cfs[0]); ++i) {
        uint32_t addr = 0xB00000u + (uint32_t)cfs[i];
        build_df18(msg, cfs[i], 0, addr);
        int before = g_sink_calls;
        modes_ingest_feed(msg, 112, NULL);
        CHECK_EQ_I(g_sink_calls, before + 1, "valid DF18 reaches sink");
        CHECK(!get_aircraft(addr, &a), "DF18 CF%d polluted ICAO table\n", cfs[i]);
    }
}

/* ── 字段级新鲜度 ──────────────────────────────────────────────────────
 *
 * 「目标还在」与「这个字段还新鲜」是两件事。DF11 全呼叫应答只带地址，收到
 * 它只能证明这架飞机还在天上；它不该让 t0 那一帧的高度/位置/速度/垂速无限
 * 续命，然后一路流进地图、威胁判定、落盘、本机绑定与 BLE/GDL90。
 *
 * 判据落在**生产读取边界**（aircraft_state_snapshot / _get_own），不是某一个
 * 页面：任何一个消费者绕过 UI 直接读表都必须拿到同一份已失效的字段。
 */

/* 起始时刻必须**大于**字段窗口，否则「时间戳漏打（保持 0）」与「打对了」
 * 在 t0 处产生同一个结果（0 距 t0 不足 60 s = 仍然新鲜），突变测试杀不掉漏打。
 * 取 1 小时：未打戳的字段在 t0 就已经过期，t0 的"字段到齐"断言立刻变红。 */
#define FIELD_TEST_T0  (3600LL * 1000000LL)

/* t0 一次性喂齐：身份(呼号+尾流等级)、气压高度、GNSS 高度、地速/航迹/垂速、
 * CPR 位置、squawk。之后只喂 DF11，看这些字段能不能被续命。 */
static void feed_full_field_set(uint32_t icao, int64_t t0)
{
    unsigned char msg[14];

    g_now_us = t0;
    /* 身份帧：呼号 + metype4/mesub3 → PK_WAKE_LARGE。 */
    from_hex("8D4840D6202CC371C32CE0576098", msg);
    msg[1] = (unsigned char)(icao >> 16);
    msg[2] = (unsigned char)(icao >> 8);
    msg[3] = (unsigned char)icao;
    msg[4] = (unsigned char)((4 << 3) | 3);
    set_crc(msg);
    modes_ingest_feed(msg, 112, NULL);

    /* 气压高度 5000 ft（Q=1）。 */
    g_now_us = t0;
    build_air_position(msg, icao, 9, 0x1F0);
    modes_ingest_feed(msg, 112, NULL);

    /* TC20 GNSS 高度。 */
    g_now_us = t0;
    from_hex("8E7C6296A0A0A59C64468D6F4EDD", msg);
    msg[1] = (unsigned char)(icao >> 16);
    msg[2] = (unsigned char)(icao >> 8);
    msg[3] = (unsigned char)icao;
    set_crc(msg);
    modes_ingest_feed(msg, 112, NULL);

    /* TC19 subtype 1：地速 159 kt / 航迹 183° / 垂速 -832 fpm。
     * 喂两帧（相隔 1 s）才会产生转弯率估计——单帧时 have_turn_rate 恒为
     * false，那条断言就不携带任何信息。 */
    from_hex("8D485020994409940838175B284F", msg);
    msg[1] = (unsigned char)(icao >> 16);
    msg[2] = (unsigned char)(icao >> 8);
    msg[3] = (unsigned char)icao;
    set_crc(msg);
    g_now_us = t0 - 1000000;
    modes_ingest_feed(msg, 112, NULL);
    g_now_us = t0;
    modes_ingest_feed(msg, 112, NULL);

    /* squawk（DF21 需要地址已进最近可见缓存——上面的 DF17 已经放进去了）。 */
    g_now_us = t0;
    build_df21(msg, icao);
    modes_ingest_feed(msg, 112, NULL);

    /* CPR 解出的位置由 adsb_link_task 单独回灌。 */
    aircraft_state_update_position(icao, 31.2000, 121.4000, t0);
}

/* 只喂 DF11：每秒一帧，持续 secs 秒。返回最后一帧的时间戳。 */
static int64_t age_with_df11_only(uint32_t icao, int64_t t0, int secs)
{
    unsigned char msg[14];
    int64_t t = t0;
    build_df11(msg, icao);
    for (int i = 1; i <= secs; ++i) {
        t = t0 + (int64_t)i * 1000000LL;
        g_now_us = t;
        modes_ingest_feed(msg, 56, NULL);
    }
    return t;
}

static void check_all_fields_expired(const aircraft_t *a, const char *who)
{
    CHECK(!a->have_callsign,       "%s: 陈旧呼号仍然有效\n", who);
    CHECK(a->wake == PK_WAKE_NONE, "%s: 陈旧尾流等级仍然有效\n", who);
    CHECK(!a->have_altitude,       "%s: 陈旧气压高度仍然有效\n", who);
    CHECK(!a->have_gnss_altitude,  "%s: 陈旧 GNSS 高度仍然有效\n", who);
    CHECK(!a->have_position,       "%s: 陈旧位置仍然有效\n", who);
    CHECK(!a->have_ground_speed,   "%s: 陈旧地速仍然有效\n", who);
    CHECK(!a->have_heading,        "%s: 陈旧航迹仍然有效\n", who);
    CHECK(!a->have_velocity,       "%s: 陈旧速度矢量仍然有效\n", who);
    CHECK(!a->have_vertical_rate,  "%s: 陈旧垂速仍然有效\n", who);
    CHECK(!a->have_squawk,         "%s: 陈旧 squawk 仍然有效\n", who);
    CHECK(!a->have_turn_rate,      "%s: 陈旧转弯率仍然有效\n", who);
    CHECK(!a->have_air_ground,     "%s: 陈旧空地状态仍然有效\n", who);
    CHECK(!a->on_ground,           "%s: 陈旧 on_ground 仍然为真\n", who);
}

static void test_snapshot_expires_stale_fields_but_keeps_entry(void)
{
    const uint32_t icao = 0x4CA111;
    const int64_t  t0   = FIELD_TEST_T0;
    aircraft_t a;
    reset_chain();

    feed_full_field_set(icao, t0);

    /* 先证明这些字段确实到齐过——否则下面的"全 false"可能只是从来没写进去。 */
    aircraft_t all[AIRCRAFT_TABLE_CAPACITY];
    size_t n = aircraft_state_snapshot(all, AIRCRAFT_TABLE_CAPACITY, t0,
                                       AIRCRAFT_STALE_AGE_US);
    CHECK(n == 1, "t0 快照应有 1 架，实得 %d\n", (int)n);
    if (n == 1) {
        CHECK(all[0].have_callsign,      "t0 呼号缺失\n");
        CHECK(all[0].wake == PK_WAKE_LARGE, "t0 尾流等级缺失\n");
        CHECK(all[0].have_altitude,      "t0 气压高度缺失\n");
        CHECK(all[0].have_gnss_altitude, "t0 GNSS 高度缺失\n");
        CHECK(all[0].have_position,      "t0 位置缺失\n");
        CHECK(all[0].have_ground_speed,  "t0 地速缺失\n");
        CHECK(all[0].have_heading,       "t0 航迹缺失\n");
        CHECK(all[0].have_velocity,      "t0 速度矢量缺失\n");
        CHECK(all[0].have_vertical_rate, "t0 垂速缺失\n");
        CHECK(all[0].have_squawk,        "t0 squawk 缺失\n");
        CHECK(all[0].have_turn_rate,     "t0 转弯率缺失\n");
        CHECK(all[0].have_air_ground,    "t0 空地证据缺失（TC19/空中位置帧）\n");
    }

    /* 之后 61 s 只有 DF11 全呼叫应答。 */
    const int64_t t_end = age_with_df11_only(icao, t0, 61);

    n = aircraft_state_snapshot(all, AIRCRAFT_TABLE_CAPACITY, t_end,
                                AIRCRAFT_STALE_AGE_US);
    CHECK(n == 1, "DF11 应保持目标在表内，实得 %d 架\n", (int)n);
    if (n != 1) return;
    a = all[0];
    CHECK(a.last_seen_us == t_end, "DF11 必须刷新目标级 last_seen\n");
    check_all_fields_expired(&a, "snapshot");
}

/* get_own 是 PFD/本机绑定/落盘的读取边界，必须与 snapshot 同一套判据。
 * 注意调用方传的是 24 h 的查表窗口（adsb_link_task 的 PK_REC_LOOKUP_MAX_AGE_US）：
 * 目标级窗口再大，也不能让字段跟着活 24 小时。 */
static void test_get_own_applies_same_field_freshness(void)
{
    const uint32_t icao = 0x4CA222;
    const int64_t  t0   = FIELD_TEST_T0;
    aircraft_t a;
    reset_chain();

    feed_full_field_set(icao, t0);
    CHECK(aircraft_state_get_own(icao, t0, 24LL * 3600 * 1000000, &a),
          "t0 get_own 未命中\n");
    CHECK(a.have_altitude && a.have_position && a.have_ground_speed,
          "t0 get_own 字段缺失\n");

    const int64_t t_end = age_with_df11_only(icao, t0, 61);
    CHECK(aircraft_state_get_own(icao, t_end, 24LL * 3600 * 1000000, &a),
          "DF11 之后 get_own 应仍命中目标\n");
    check_all_fields_expired(&a, "get_own");
}

/* 字段各自独立过期：新鲜的速度帧只复活速度，高度/位置/呼号照旧失效。
 * 一刀切地"收到任何一帧就全部复活"与不做失效是同一个缺陷。 */
static void test_fresh_frame_revives_only_its_own_fields(void)
{
    const uint32_t icao = 0x4CA333;
    const int64_t  t0   = FIELD_TEST_T0;
    unsigned char msg[14];
    aircraft_t a;
    reset_chain();

    feed_full_field_set(icao, t0);
    const int64_t t_end = age_with_df11_only(icao, t0, 61);

    g_now_us = t_end;
    from_hex("8D485020994409940838175B284F", msg);
    msg[1] = (unsigned char)(icao >> 16);
    msg[2] = (unsigned char)(icao >> 8);
    msg[3] = (unsigned char)icao;
    set_crc(msg);
    modes_ingest_feed(msg, 112, NULL);

    CHECK(aircraft_state_get_own(icao, t_end, AIRCRAFT_STALE_AGE_US, &a),
          "复活帧之后 get_own 未命中\n");
    CHECK(a.have_ground_speed, "新鲜 TC19 必须复活地速\n");
    CHECK(a.have_heading,      "新鲜 TC19 必须复活航迹\n");
    CHECK(a.have_vertical_rate,"新鲜 TC19 必须复活垂速\n");
    CHECK_EQ_I(a.ground_speed_kt, 159, "复活后的地速");
    CHECK(!a.have_altitude,    "TC19 不携带气压高度，不得复活它\n");
    CHECK(!a.have_position,    "TC19 不携带位置，不得复活它\n");
    CHECK(!a.have_callsign,    "TC19 不携带呼号，不得复活它\n");
    CHECK(!a.have_squawk,      "TC19 不携带 squawk，不得复活它\n");
}

/* 空地状态是三态。「不知道」不得被当成「已知在空中」——那是 GDL90 的
 * airborne 位和 pk_flight_phase 的 UC6 矛盾检测都会拿去用的正向断言。
 *
 * 只发身份帧/DF11/DF20/DF21 的目标，从头到尾都该是"不知道"。 */
static void test_air_ground_unknown_until_a_real_source_says_so(void)
{
    const uint32_t icao = 0x4CA777;
    const int64_t  t0   = FIELD_TEST_T0;
    unsigned char msg[14];
    aircraft_t a;
    reset_chain();

    /* 身份帧（metype 4）：带呼号与尾流等级，但**不含**空地信息。 */
    g_now_us = t0;
    from_hex("8D4840D6202CC371C32CE0576098", msg);
    msg[1] = (unsigned char)(icao >> 16);
    msg[2] = (unsigned char)(icao >> 8);
    msg[3] = (unsigned char)icao;
    set_crc(msg);
    modes_ingest_feed(msg, 112, NULL);

    /* DF11 全呼叫应答：只有地址。 */
    g_now_us = t0;
    build_df11(msg, icao);
    modes_ingest_feed(msg, 56, NULL);

    CHECK(aircraft_state_get_own(icao, t0, AIRCRAFT_STALE_AGE_US, &a),
          "只发身份帧/DF11 的目标应在表内\n");
    CHECK(a.have_callsign, "前置条件：身份帧应已落地\n");
    CHECK(!a.have_air_ground,
          "身份帧/DF11 不携带空地信息，不得置 have_air_ground\n");
    CHECK(!a.on_ground, "未知时 on_ground 必须是 false\n");

    /* TC19 空中速度帧：DO-260B 里它只在空中发送 → 这是一次真实的空中证据。 */
    g_now_us = t0 + 1000000;
    from_hex("8D485020994409940838175B284F", msg);
    msg[1] = (unsigned char)(icao >> 16);
    msg[2] = (unsigned char)(icao >> 8);
    msg[3] = (unsigned char)icao;
    set_crc(msg);
    modes_ingest_feed(msg, 112, NULL);
    CHECK(aircraft_state_get_own(icao, t0 + 1000000, AIRCRAFT_STALE_AGE_US, &a),
          "TC19 之后未命中\n");
    CHECK(a.have_air_ground, "TC19 必须给出空地证据\n");
    CHECK(!a.on_ground, "TC19 = 空中\n");
}

/* 空地状态过期后必须回到"不知道"，两位一起清：只清 on_ground 会把它变成
 * "已知在空中"，一架早已落地的飞机会被 GDL90 断言成空中交通。 */
static void test_on_ground_expires(void)
{
    const uint32_t icao = 0x4CA444;
    const int64_t  t0   = FIELD_TEST_T0;
    unsigned char msg[14];
    aircraft_t a;
    reset_chain();

    g_now_us = t0;
    build_surface(msg, icao, 39, 1, 64);
    modes_ingest_feed(msg, 112, NULL);
    CHECK(aircraft_state_get_own(icao, t0, AIRCRAFT_STALE_AGE_US, &a),
          "地面目标 t0 未命中\n");
    CHECK(a.on_ground, "t0 on_ground 应为真\n");
    CHECK(a.have_air_ground, "地面帧必须给出空地证据\n");
    /* 地面帧自带 MOV/TRK——地面目标永远不发 TC19，这是它唯一的速度来源，
     * 所以这两个字段的打戳必须由地面分支自己负责。 */
    CHECK(a.have_ground_speed, "t0 地面帧地速缺失\n");
    CHECK(a.have_heading,      "t0 地面帧航迹缺失\n");

    const int64_t t_end = age_with_df11_only(icao, t0, 61);
    CHECK(aircraft_state_get_own(icao, t_end, AIRCRAFT_STALE_AGE_US, &a),
          "地面目标过期后应仍在表内\n");
    CHECK(!a.on_ground, "陈旧 on_ground 必须失效\n");
    CHECK(!a.have_air_ground,
          "陈旧空地状态必须回到'不知道'，不能变成'已知在空中'\n");
    CHECK(!a.have_ground_speed, "陈旧地面地速必须失效\n");
    CHECK(!a.have_heading,      "陈旧地面航迹必须失效\n");

    /* 起飞：空中位置帧把 on_ground 打回 false，这是一次**有据的**否定。
     * 值本身在过期后也是 false，两者无法靠 on_ground 区分——判据只能是
     * 时间戳本身（它是 aircraft_t 公开契约的一部分）。不这么钉，空中分支
     * 的打戳就是一行永远无法被证伪的代码。 */
    const int64_t t_air = t_end + 1000000;
    g_now_us = t_air;
    build_air_position(msg, icao, 9, 0x1F0);
    modes_ingest_feed(msg, 112, NULL);
    CHECK(aircraft_state_get_own(icao, t_air, AIRCRAFT_STALE_AGE_US, &a),
          "起飞后 get_own 未命中\n");
    CHECK(!a.on_ground, "空中位置帧必须清掉 on_ground\n");
    CHECK(a.have_air_ground, "空中位置帧是有据的断言，必须置 have_air_ground\n");
    CHECK(a.air_ground_us == t_air,
          "空中位置帧必须刷新空地时间戳 got=%lld want=%lld\n",
          (long long)a.air_ground_us, (long long)t_air);
}

/* DF20 是 TC9-18 之外的第二条气压高度入口（Comm-B altitude reply），
 * 它自己那一处打戳漏掉时，DF20-only 的目标高度会在 60 s 后凭空过期。 */
static void test_df20_altitude_freshness(void)
{
    const uint32_t icao = 0x4CA666;
    const int64_t  t0   = FIELD_TEST_T0;
    unsigned char msg[14];
    aircraft_t a;
    reset_chain();

    /* DF20 的地址靠 AP 反解，先用一帧 DF11 把它放进最近可见缓存。 */
    g_now_us = t0;
    build_df11(msg, icao);
    modes_ingest_feed(msg, 56, NULL);

    g_now_us = t0;
    build_df20(msg, icao);
    modes_ingest_feed(msg, 112, NULL);
    CHECK(aircraft_state_get_own(icao, t0, AIRCRAFT_STALE_AGE_US, &a),
          "DF20 目标 t0 未命中\n");
    CHECK(a.have_altitude, "DF20 气压高度缺失\n");
    CHECK_EQ_I(a.altitude_ft, 5000, "DF20 AC13 解出的气压高度");

    const int64_t t_end = age_with_df11_only(icao, t0, 61);
    CHECK(aircraft_state_get_own(icao, t_end, AIRCRAFT_STALE_AGE_US, &a),
          "DF20 目标过期后应仍在表内\n");
    CHECK(!a.have_altitude, "陈旧 DF20 高度必须失效\n");
}

/* airspeed / air-heading（TC19 subtype 3/4）与地速矢量是两条独立的链路，
 * 同样不得被无关帧续命。 */
static void test_airspeed_fields_expire(void)
{
    const uint32_t icao = 0x4CA555;
    const int64_t  t0   = FIELD_TEST_T0;
    unsigned char msg[14];
    aircraft_t a;
    reset_chain();

    g_now_us = t0;
    from_hex("8DA05F219B06B6AF189400CBC33F", msg);
    msg[1] = (unsigned char)(icao >> 16);
    msg[2] = (unsigned char)(icao >> 8);
    msg[3] = (unsigned char)icao;
    set_crc(msg);
    modes_ingest_feed(msg, 112, NULL);
    CHECK(aircraft_state_get_own(icao, t0, AIRCRAFT_STALE_AGE_US, &a),
          "airspeed 目标 t0 未命中\n");
    CHECK(a.have_airspeed && a.have_airspeed_heading,
          "t0 airspeed 字段缺失\n");

    const int64_t t_end = age_with_df11_only(icao, t0, 61);
    CHECK(aircraft_state_get_own(icao, t_end, AIRCRAFT_STALE_AGE_US, &a),
          "airspeed 目标过期后应仍在表内\n");
    CHECK(!a.have_airspeed,         "陈旧 airspeed 必须失效\n");
    CHECK(!a.have_airspeed_heading, "陈旧 air-heading 必须失效\n");
}

/* 演示模式走的是 snapshot 里另一条分支（合成目标不进 s_table）。同一道字段级
 * 过期在那条分支上也会跑，所以合成字段必须被正确打戳——漏打的症状是屏上 17
 * 架飞机的高度/速度/呼号整片变成 ---，而目标本身还在。 */
static void test_demo_snapshot_keeps_synthetic_fields(void)
{
    aircraft_t all[AIRCRAFT_TABLE_CAPACITY];
    const int64_t now = FIELD_TEST_T0;
    reset_chain();

    g_demo_on = true;
    g_demo_last_seen_age_s = 45;   /* 目标级 45 s：仍在 60 s 窗口内 */
    size_t n = aircraft_state_snapshot(all, AIRCRAFT_TABLE_CAPACITY, now,
                                       AIRCRAFT_STALE_AGE_US);
    CHECK(n == 1, "演示快照应有 1 架，实得 %d\n", (int)n);
    if (n == 1) {
        CHECK(all[0].have_callsign,       "演示呼号被误伤\n");
        CHECK(all[0].wake == PK_WAKE_LARGE, "演示尾流等级被误伤\n");
        CHECK(all[0].have_altitude,       "演示高度被误伤\n");
        CHECK(all[0].have_position,       "演示位置被误伤\n");
        CHECK(all[0].have_ground_speed,   "演示地速被误伤\n");
        CHECK(all[0].have_heading,        "演示航迹被误伤\n");
        CHECK(all[0].have_velocity,       "演示速度矢量被误伤\n");
        CHECK(all[0].have_vertical_rate,  "演示垂速被误伤\n");
        CHECK(all[0].have_squawk,         "演示 squawk 被误伤\n");
    }

    /* 目标级窗口照旧生效：超龄的合成目标整条消失，不是字段变空。 */
    g_demo_last_seen_age_s = 90;
    n = aircraft_state_snapshot(all, AIRCRAFT_TABLE_CAPACITY, now,
                                AIRCRAFT_STALE_AGE_US);
    CHECK(n == 0, "超过 60 s 的演示目标应整条被滤掉，实得 %d\n", (int)n);
    g_demo_on = false;
}

int main(void)
{
    test_velocity_vectors_reach_state();
    test_tc19_na_invalidates_stale_ground_vector();
    test_airspeed_heading_does_not_replace_ground_track();
    test_surface_composite_validity();
    test_surface_na_invalidates_prior_components();
    test_valid_zero_and_gnss_altitude_are_distinct();
    test_df18_address_semantics();
    test_snapshot_expires_stale_fields_but_keeps_entry();
    test_get_own_applies_same_field_freshness();
    test_fresh_frame_revives_only_its_own_fields();
    test_air_ground_unknown_until_a_real_source_says_so();
    test_on_ground_expires();
    test_df20_altitude_freshness();
    test_airspeed_fields_expire();
    test_demo_snapshot_keeps_synthetic_fields();
    printf(g_fail ? "FAIL (%d)\n" : "PASS: raw Mode-S to aircraft state\n", g_fail);
    return g_fail ? 1 : 0;
}
