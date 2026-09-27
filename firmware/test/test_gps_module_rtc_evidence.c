/*
 * test_gps_module_rtc_evidence.c — 模块自身 RTC 的走时证据（GNSS VBAT 方案判据）
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/test/host_stubs -I firmware/main \
 *      -o /tmp/test_gps_module_rtc_evidence \
 *      firmware/test/test_gps_module_rtc_evidence.c \
 *      firmware/main/gps_task.c firmware/main/gps_nmea.c \
 *      firmware/main/pk_clock.c -lm \
 *   && /tmp/test_gps_module_rtc_evidence
 *
 * 背景
 * ----
 * ATGM336H 模块内部自带 RTC（6N 手册 §2.4：pin6 VBAT = "RTC 及 SRAM 后备电源，
 * 供电范围 1.5~3.6V 以保证模块热启动"；§2.5：Ibat 典型 10µA）。V4.9 网表中
 * pin6 已接 GNSS_VBAT：主电池经 U23/D7 常供，BT1 经 R63 备份。这个测试只验证
 * 固件能保留模块在无定位状态下持续输出的 RTC 时间；断主电源后的保持能力仍须
 * 由 GNSS_VBAT 电压和重新上电后的 RMC/热启动实测证明。
 *
 * 方案成立的前提是一条可证伪的断言：
 *
 *     **模块的 RTC 独立于定位在走时——即使没有 fix，RMC 也照样吐出时间和日期。**
 *
 * 本测试把这条前提钉在解析链路上，诊断页的 "RMC UTC" 行则把它显示到屏幕上
 * 供实机取证（室外定位成功后拔掉天线，看 status 变 V 之后时间是否继续前进）。
 *
 * 判据
 * ----
 * 时间由 host_stubs/esp_timer.h 注入；喂数据走 pk_gps_feed_line()——UART 任务
 * 拼完一行之后调用的同一个函数，跑的是生产解析链路本身。
 *
 * 关键的一条是 test_module_time_survives_fix_expiry：新增的三个字段必须**不被**
 * gps_apply_freshness() 过期清零。"位置有没有"和"模块的钟在不在走"是两个问题，
 * 天线拔掉时前者必须立刻作废、后者恰恰应该继续跳。把新字段顺手并进那个过期
 * 分支，这条就会变红。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gps.h"
#include "demo_data.h"
#include "pk_clock.h"   /* 期望值用同一个历法换算函数算，不在测试里抄一份 */

/* ── 注入时钟 ──────────────────────────────────────────────────────── */
static int64_t g_now_us;
int64_t esp_timer_get_time(void) { return g_now_us; }

/* ── 演示模式：本测试永远关闭（否则 pk_gps_get 交出的是合成数据） ── */
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

static void reset(int64_t t0)
{
    g_now_us = t0;
    pk_gps_state_init();
    /* 这些用例验证已选定天线下的 NMEA/RTC 语义；生产路径只有在 RP 的
     * CONFIG_ACK 确认来源后才采信 RF 证据，夹具必须建立同一前置条件。 */
    pk_gps_antenna_source_confirmed(PK_GPS_SOURCE_EXTERNAL);
}

/* 2026-03-23 12:35:19 UTC —— 两个夹具共用同一时刻，差别只在 status。 */
#define T_HHMMSS "123519"
#define T_DDMMYY "230326"

static void feed_rmc_fix(const char *hhmmss)
{
    char body[96];
    snprintf(body, sizeof(body),
             "GPRMC,%s,A,3100.0000,N,12100.0000,E,5.0,90.0,%s,,",
             hhmmss, T_DDMMYY);
    feed(body);
}

/* status='V'：模块在讲话、句子合法、时间日期齐全，但没有定位解。
 * 这正是「拔掉天线之后」的现场，也是本方案要取的证。 */
static void feed_rmc_nofix(const char *hhmmss)
{
    char body[96];
    snprintf(body, sizeof(body), "GPRMC,%s,V,,,,,,,%s,,", hhmmss, T_DDMMYY);
    feed(body);
}

/* 2026-03-23 12:35:19 UTC 的 epoch 毫秒，用于核对落地值不是随便一个数。 */
static int64_t expect_ms(int hh, int mi, int ss)
{
    return pk_clock_civil_utc_to_epoch_ms(2026, 3, 23, hh, mi, ss, 0);
}

/* 核心：没有 fix 也必须记下模块报的时间。
 * 把记录语句挪进 if(valid) 分支，这条立刻变红。 */
static void test_nofix_rmc_still_records_module_time(void)
{
    pk_gps_state_t g;
    reset(100000000);
    feed_rmc_nofix(T_HHMMSS);

    CHECK(!pk_gps_get(&g), "status='V' 不该有 fix\n");
    CHECK(g.rmc_epoch_ms != 0, "无 fix 时也必须记下模块时间，got=0\n");
    CHECK(g.rmc_epoch_ms == expect_ms(12, 35, 19),
          "模块时间落地错误 got=%lld want=%lld\n",
          (long long)g.rmc_epoch_ms, (long long)expect_ms(12, 35, 19));
    CHECK(!g.rmc_epoch_from_fix, "这条 RMC 没有定位，from_fix 应为假\n");
    CHECK(g.rmc_epoch_us == 100000000, "收到时刻应为注入的当前时间 got=%lld\n",
          (long long)g.rmc_epoch_us);
}

/* 有 fix 时同样记录，并标明这一条是带定位的。 */
static void test_fix_rmc_marks_from_fix(void)
{
    pk_gps_state_t g;
    reset(100000000);
    feed_rmc_fix(T_HHMMSS);

    CHECK(pk_gps_get(&g), "status='A' 应当有 fix\n");
    CHECK(g.rmc_epoch_ms == expect_ms(12, 35, 19),
          "模块时间落地错误 got=%lld\n", (long long)g.rmc_epoch_ms);
    CHECK(g.rmc_epoch_from_fix, "这条 RMC 带定位，from_fix 应为真\n");
}

/* 「钟在走」而不是「有个冻住的旧值」：连续两条无 fix 的 RMC，时间必须前进。
 * 只记第一次、之后不再更新的实现会在这里变红。 */
static void test_module_clock_advances_without_fix(void)
{
    pk_gps_state_t g;
    reset(100000000);

    feed_rmc_nofix("123519");
    pk_gps_get(&g);
    const int64_t first = g.rmc_epoch_ms;

    g_now_us += 1000000;              /* 1 s 后的下一条 */
    feed_rmc_nofix("123520");
    pk_gps_get(&g);

    CHECK(g.rmc_epoch_ms == first + 1000,
          "无 fix 时模块时间应逐秒前进 got=%lld want=%lld\n",
          (long long)g.rmc_epoch_ms, (long long)(first + 1000));
    CHECK(g.rmc_epoch_us == 101000000,
          "收到时刻应跟着更新 got=%lld\n", (long long)g.rmc_epoch_us);
}

/* 新鲜度过期只管「位置」，不许把「模块的钟」一起清掉。
 *
 * 现场就是拔天线：fix 必须立刻作废，而模块时间恰恰要留着——它是判断
 * 「模块 RTC 是否在独立走时」的唯一依据，清了就什么都看不到了。 */
static void test_module_time_survives_fix_expiry(void)
{
    pk_gps_state_t g;
    reset(100000000);
    feed_rmc_fix(T_HHMMSS);
    CHECK(pk_gps_get(&g), "前置条件：应先有 fix\n");

    g_now_us += PK_GPS_FIX_MAX_AGE_US + 1000;   /* 跨过新鲜度窗口 */

    CHECK(!pk_gps_get(&g), "过期后不得再宣称有 fix\n");
    CHECK(!g.have_fix, "过期后 have_fix 应为假\n");
    CHECK(g.lat == 0.0 && g.lon == 0.0, "过期后坐标必须清零\n");
    CHECK(g.rmc_epoch_ms == expect_ms(12, 35, 19),
          "模块时间不得被新鲜度过期清掉 got=%lld\n", (long long)g.rmc_epoch_ms);
    CHECK(g.rmc_epoch_from_fix, "from_fix 也不该被过期改写\n");
}

/* 时间字段为空 → 不是「模块报了 0 点」，而是「什么都没报」，必须保持未记录。
 * 用 0 当哨兵值的前提是永远不会把一个解析失败写成 0。 */
static void test_missing_time_field_not_recorded(void)
{
    pk_gps_state_t g;
    reset(100000000);
    feed("GPRMC,,V,,,,,,,230326,,");

    pk_gps_get(&g);
    CHECK(g.rmc_epoch_ms == 0,
          "时间字段为空时不得记录 got=%lld\n", (long long)g.rmc_epoch_ms);
}

/* 日期字段为空同理——只有时分秒推不出 epoch。 */
static void test_missing_date_field_not_recorded(void)
{
    pk_gps_state_t g;
    reset(100000000);
    feed("GPRMC,123519,V,,,,,,,,,");

    pk_gps_get(&g);
    CHECK(g.rmc_epoch_ms == 0,
          "日期字段为空时不得记录 got=%lld\n", (long long)g.rmc_epoch_ms);
}

int main(void)
{
    printf("test_gps_module_rtc_evidence\n");
    test_nofix_rmc_still_records_module_time();
    test_fix_rmc_marks_from_fix();
    test_module_clock_advances_without_fix();
    test_module_time_survives_fix_expiry();
    test_missing_time_field_not_recorded();
    test_missing_date_field_not_recorded();

    if (g_fail) { printf("FAILED: %d\n", g_fail); return 1; }
    printf("OK\n");
    return 0;
}
