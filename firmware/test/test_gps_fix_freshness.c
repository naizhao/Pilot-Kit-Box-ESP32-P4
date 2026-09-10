/*
 * test_gps_fix_freshness.c — GPS 位置 fix 的新鲜度合同（P1-B）。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/test/host_stubs -I firmware/main \
 *      -o /tmp/test_gps_fix_freshness \
 *      firmware/test/test_gps_fix_freshness.c \
 *      firmware/main/gps_task.c firmware/main/gps_nmea.c \
 *      firmware/main/pk_clock.c -lm \
 *   && /tmp/test_gps_fix_freshness
 *
 * 越界/未定义行为检测（可选加强跑法，默认入口不执行）：
 *   cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined \
 *      -I firmware/test/host_stubs -I firmware/main \
 *      -o /tmp/test_gps_fix_freshness_asan \
 *      firmware/test/test_gps_fix_freshness.c \
 *      firmware/main/gps_task.c firmware/main/gps_nmea.c \
 *      firmware/main/pk_clock.c -lm && /tmp/test_gps_fix_freshness_asan
 *
 * 缺陷（修复前）
 * --------------
 * s_gps.have_fix 只在收到**新的**有效 RMC 时被写。天线被遮、模块掉线、UART
 * 断了——这些情况下没有任何一行 NMEA 会到来，于是 have_fix 保持 true、
 * lat/lon 冻在最后一次定位上，而 pk_gps_get() 不看 updated_us 就把它交出去。
 * 下游（own_ship 的 GPS 兜底、pk_own_sampler 的轨迹与落盘、地面 CPR 的参考
 * 点、BLE 心跳的 GPS-valid 位与 Ownship 报文）全都据此宣称"有定位"。
 *
 * 屏上与手机上看到的是一个静止不动但完全笃定的本机位置。这比显示"无 GPS"
 * 危险得多：后者会让人去看别的信息源，前者不会。
 *
 * 判据
 * ----
 * 时间由 host_stubs/esp_timer.h 注入，测试直接推进 g_now_us；喂数据走
 * pk_gps_feed_line()——那正是 UART 任务拼完一行之后调用的同一个函数，所以
 * 跑的是生产解析链路本身，不是它的复制品。
 *
 * 「无新数据」在这里被刻意分成三种，因为它们的现场表现完全不同：
 *   1. 一行都不来（模块掉线 / 线断）；
 *   2. 行照来但 checksum 全坏（UART 半死、只剩乱码）；
 *   3. 行照来且合法，但 RMC status = 'V'（有信号没定位，最常见的进隧道）。
 * 三种都必须让 pk_gps_get() 停止宣称有 fix。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gps.h"
#include "demo_data.h"

/* ── 注入时钟 ──────────────────────────────────────────────────────── */
static int64_t g_now_us;
int64_t esp_timer_get_time(void) { return g_now_us; }

/* ── 演示模式外部依赖：本测试永远关闭演示（否则拿到的是合成数据） ── */
bool pk_demo_enabled(void) { return false; }
bool pk_demo_gps(int64_t now_us, pk_gps_state_t *out)
{ (void)now_us; (void)out; return false; }

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("         at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

/* NMEA checksum = '$' 与 '*' 之间所有字节的异或，两位大写十六进制。 */
static void feed(const char *body)
{
    char line[128];
    unsigned char ck = 0;
    for (const char *p = body; *p; ++p) ck ^= (unsigned char)*p;
    snprintf(line, sizeof(line), "$%s*%02X", body, ck);
    pk_gps_feed_line(line);
}

/* 一条有效定位 RMC：status='A'，31°00.0000'N 121°00.0000'E，5 kt，航迹 90°。 */
static void feed_valid_rmc(void)
{
    feed("GPRMC,123519,A,3100.0000,N,12100.0000,E,5.0,90.0,230326,,");
}

/* status='V'：模块在讲话、句子合法，但没有定位解。 */
static void feed_novalid_rmc(void)
{
    feed("GPRMC,123519,V,,,,,,,230326,,");
}

static void reset(int64_t t0)
{
    g_now_us = t0;
    pk_gps_state_init();
}

/* 基线：刚收到有效 RMC 时必须有 fix，且经纬度落地正确。
 * 没有这一条，下面所有 "false" 断言都可能只是因为解析压根没成功。 */
static void test_fresh_rmc_gives_fix(void)
{
    pk_gps_state_t g;
    reset(100000000);
    feed_valid_rmc();
    CHECK(pk_gps_get(&g), "新鲜 RMC 之后应当有 fix\n");
    CHECK(g.have_fix, "have_fix 应为真\n");
    CHECK(g.lat > 30.99 && g.lat < 31.01, "纬度落地错误 got=%f\n", g.lat);
    CHECK(g.lon > 120.99 && g.lon < 121.01, "经度落地错误 got=%f\n", g.lon);
    CHECK(g.ground_speed_kt == 5, "地速落地错误 got=%d\n", g.ground_speed_kt);
    CHECK(g.track_deg == 90, "航迹落地错误 got=%d\n", g.track_deg);

    /* 窗口之内不得误伤——把窗口缩到 0 的"修复"必须在这里变红。 */
    g_now_us += PK_GPS_FIX_MAX_AGE_US - 1000;
    CHECK(pk_gps_get(&g), "窗口内不得撤销 fix\n");
    CHECK(g.have_fix, "窗口内 have_fix 应仍为真\n");
}

/* 情形 1：模块掉线，一行都不来。 */
static void test_fix_expires_with_no_nmea_at_all(void)
{
    pk_gps_state_t g;
    reset(100000000);
    feed_valid_rmc();
    CHECK(pk_gps_get(&g), "前置条件：应先有 fix\n");

    g_now_us += PK_GPS_FIX_MAX_AGE_US + 1000;
    CHECK(!pk_gps_get(&g), "超窗无 NMEA：pk_gps_get 不得再返回有 fix\n");
    CHECK(!g.have_fix, "超窗无 NMEA：have_fix 必须为假\n");
    /* 位置一并清掉：留着旧经纬度等于把"冻结的坐标"继续递给消费者，
     * 而 own_ship / pk_own_sampler / 地面 CPR 参考点都只看 have_fix
     * 之外的这几个数。 */
    CHECK(g.lat == 0.0 && g.lon == 0.0,
          "超窗后经纬度必须清零 got=%f,%f\n", g.lat, g.lon);
    CHECK(g.ground_speed_kt == 0, "超窗后地速必须清零 got=%d\n", g.ground_speed_kt);
    CHECK(g.track_deg == 0, "超窗后航迹必须清零 got=%d\n", g.track_deg);
}

/* 情形 2：行照来但 checksum 全坏。last_nmea_us 会一直被刷新（它连坏行也算
 * 「在讲话」），所以拿 last_nmea_us 当新鲜度依据的实现会在这里放行。 */
static void test_fix_expires_when_only_corrupt_lines_arrive(void)
{
    pk_gps_state_t g;
    reset(100000000);
    feed_valid_rmc();

    for (int i = 1; i <= 10; ++i) {
        g_now_us = 100000000 + (int64_t)i * 1000000;
        char junk[64];
        snprintf(junk, sizeof(junk), "$GPRMC,123519,A,3100.0000,N,,,,,%02d0326,,*00", i);
        pk_gps_feed_line(junk);          /* checksum 恒不匹配 → 整行丢弃 */
    }

    CHECK(!pk_gps_get(&g),
          "只剩坏 checksum 的行：不得据此维持 fix（last_nmea_us 不是判据）\n");
    CHECK(!g.have_fix, "坏行不得维持 have_fix\n");
}

/* 情形 3：合法 RMC 但 status='V'。这条本来就会把 have_fix 打成 false，
 * 断言在于**位置也要一起撤**——否则 have_fix=false 而 lat/lon 还是旧值，
 * 任何只读坐标的路径照样拿到冻结位置。 */
static void test_status_v_clears_position(void)
{
    pk_gps_state_t g;
    reset(100000000);
    feed_valid_rmc();
    CHECK(pk_gps_get(&g), "前置条件：应先有 fix\n");

    g_now_us += 1000000;
    feed_novalid_rmc();
    CHECK(!pk_gps_get(&g), "status=V 之后不得再有 fix\n");
    CHECK(g.lat == 0.0 && g.lon == 0.0,
          "status=V 之后经纬度必须清零 got=%f,%f\n", g.lat, g.lon);
}

/* GGA 高度是独立的一路（q>0 才置 have_altitude），同样不能被无关数据续命：
 * RMC 一直新鲜、GGA 停了，高度必须过期。 */
static void test_gga_altitude_expires_independently(void)
{
    pk_gps_state_t g;
    reset(100000000);
    feed_valid_rmc();
    feed("GPGGA,123519,3100.0000,N,12100.0000,E,1,08,0.9,545.4,M,46.9,M,,");
    CHECK(pk_gps_get(&g), "前置条件：应有 fix\n");
    CHECK(g.have_altitude, "GGA q=1 应给出高度\n");
    /* 545.4 m × 3.28084 = 1789.33 ft，parse_gga 的 +0.5 后取整 → 1789。 */
    CHECK(g.altitude_ft == 1789,
          "545.4 m 应换算为 1789 ft got=%d\n", g.altitude_ft);

    /* RMC 每秒照来（fix 保持新鲜），但 GGA 断供。 */
    for (int i = 1; i <= 10; ++i) {
        g_now_us = 100000000 + (int64_t)i * 1000000;
        feed_valid_rmc();
    }
    CHECK(pk_gps_get(&g), "RMC 仍新鲜，fix 应保持\n");
    CHECK(g.have_fix, "RMC 仍新鲜，have_fix 应为真\n");
    CHECK(!g.have_altitude, "GGA 断供超窗：高度必须失效\n");
    CHECK(g.altitude_ft == 0, "高度失效后值必须清零 got=%d\n", g.altitude_ft);
}

/* 恢复：重新收到有效 RMC 之后 fix 必须立刻回来（过期不是一次性闩锁）。 */
static void test_fix_recovers_after_new_rmc(void)
{
    pk_gps_state_t g;
    reset(100000000);
    feed_valid_rmc();
    g_now_us += PK_GPS_FIX_MAX_AGE_US + 1000;
    CHECK(!pk_gps_get(&g), "前置条件：应已过期\n");

    feed_valid_rmc();
    CHECK(pk_gps_get(&g), "新 RMC 之后 fix 必须恢复\n");
    CHECK(g.lat > 30.99 && g.lat < 31.01, "恢复后纬度应重新可用 got=%f\n", g.lat);
}

/*
 * 时间锁定（time_locked）与位置 fix 是两条**独立**的状态，不得互相顶替：
 * time_locked 还额外要求 PPS 在跳。host 上没有 PPS 沿，所以即便位置 fix
 * 完全新鲜，time_locked 也必须是 false——把"有定位"当成"时间可信"用，会让
 * pk_clock 的来源判定失去意义。
 */
static void test_time_lock_not_implied_by_position_fix(void)
{
    pk_gps_state_t g;
    reset(100000000);
    feed_valid_rmc();
    CHECK(pk_gps_get(&g), "前置条件：应有 fix\n");
    CHECK(!g.time_locked, "没有 PPS 时不得声称时间锁定\n");
}

int main(void)
{
    test_fresh_rmc_gives_fix();
    test_fix_expires_with_no_nmea_at_all();
    test_fix_expires_when_only_corrupt_lines_arrive();
    test_status_v_clears_position();
    test_gga_altitude_expires_independently();
    test_fix_recovers_after_new_rmc();
    test_time_lock_not_implied_by_position_fix();

    if (g_fail == 0) { printf("test_gps_fix_freshness: all OK\n"); return 0; }
    printf("test_gps_fix_freshness: %d FAIL\n", g_fail);
    return 1;
}
