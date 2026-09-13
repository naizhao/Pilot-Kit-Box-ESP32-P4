/*
 * test_pk_clock_priority.c — 校时来源的可信度阶梯
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/test/host_stubs -I firmware/main \
 *      -o /tmp/test_pk_clock_priority \
 *      firmware/test/test_pk_clock_priority.c \
 *      firmware/main/pk_clock.c -lm \
 *   && /tmp/test_pk_clock_priority
 *
 * 背景
 * ----
 * 原先 pk_clock 只有一条特例规则：「GPS 精校后 5 分钟内，非 GPS 来源不许覆盖」。
 * 来源只有 GPS 和 BLE 两类时够用。2026-09-13 加了 NVS 兜底（开机从 flash 里
 * 读回上次关机时刻）之后不够了——NVS 那个值必然是**偏早**的（关机期间时间照
 * 流逝，而我们不知道流逝了多久），它一旦能覆盖 GPS 校准过的时钟，就是把准的
 * 换成不准的。
 *
 * 于是把特例升级成显式阶梯：
 *
 *     gps(卫星fix) > ble-write / ios-cts(手机) > gps-coarse(GNSS自己的RTC) > nvs
 *
 * 规则两条：
 *   1. 等级 >= 当前来源 → 直接接受（同级按后来居上，卫星授时每秒都该能刷新）
 *   2. 等级 <  当前来源 → 只有当前这个值已经**不新鲜**（超过优先窗口）才接受
 *
 * 判据
 * ----
 * 只断言 pk_clock_apply_epoch_ms() 的接受/拒绝与 pk_clock_source()，
 * **不去读系统墙钟**：host 上非 root 时 settimeofday() 会失败，拿 gettimeofday()
 * 读回来的是机器真实时间，断言会随执行时刻漂——那种测试的绿灯不携带信息。
 * 时间由 host_stubs/esp_timer.h 注入，测试直接推进 g_now_us。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pk_clock.h"

/* ── 注入单调时钟 ──────────────────────────────────────────────── */
static int64_t g_now_us;
int64_t esp_timer_get_time(void) { return g_now_us; }

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("         at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

/* 2026-03-23 12:35:19 UTC 附近，远在 2024 下限之上。 */
static int64_t t_base(void) { return pk_clock_civil_utc_to_epoch_ms(2026, 3, 23, 12, 35, 19, 0); }

/* 每个用例都要一张白纸：pk_clock 的状态是文件级静态量，上一条用例校过的
 * 来源会一直留着。没有这个入口，用例顺序就成了隐藏的依赖。 */
static void reset(int64_t t0)
{
    g_now_us = t0;
    pk_clock_reset_for_test();
}

/* 基线：开机第一次，什么来源都该能写进去。
 * 没有这一条，下面所有"被拒绝"的断言都可能只是因为压根没接受过任何值。 */
static void test_first_write_accepted_from_any_source(void)
{
    reset(1000000);
    CHECK(!pk_clock_is_synced(), "复位后不该是已校时状态\n");
    CHECK(pk_clock_apply_epoch_ms(t_base(), "nvs"),
          "开机第一次，最低级的 nvs 也该被接受\n");
    CHECK(pk_clock_is_synced(), "写入后应为已校时\n");
    CHECK(!strcmp(pk_clock_source(), "nvs"),
          "来源应为 nvs，got=%s\n", pk_clock_source());
}

/* 低级不得覆盖高级：GPS 校过之后，NVS 那个偏早的值必须被挡住。
 * 这是整条阶梯存在的理由——放进来就是拿不准的换掉准的。 */
static void test_low_tier_cannot_override_fresh_high_tier(void)
{
    reset(1000000);
    CHECK(pk_clock_apply_epoch_ms(t_base(), "gps"), "前置条件：gps 应写入成功\n");

    g_now_us += 1000000;   /* 1 s 后 */
    CHECK(!pk_clock_apply_epoch_ms(t_base() + 60000, "nvs"),
          "gps 仍新鲜时，nvs 不得覆盖\n");
    CHECK(!strcmp(pk_clock_source(), "gps"),
          "被拒之后来源必须仍是 gps，got=%s\n", pk_clock_source());

    CHECK(!pk_clock_apply_epoch_ms(t_base() + 60000, "gps-coarse"),
          "gps 仍新鲜时，gps-coarse 不得覆盖\n");
    CHECK(!pk_clock_apply_epoch_ms(t_base() + 60000, "ble-write"),
          "gps 仍新鲜时，手机也不得覆盖（卫星授时更准）\n");
}

/* 高级随时可以覆盖低级：开机先用 NVS 兜底，GPS 一有 fix 就该立刻纠正过来。 */
static void test_high_tier_overrides_low_tier_immediately(void)
{
    reset(1000000);
    CHECK(pk_clock_apply_epoch_ms(t_base(), "nvs"), "前置条件：nvs 应写入成功\n");

    CHECK(pk_clock_apply_epoch_ms(t_base() + 3600000, "gps-coarse"),
          "gps-coarse 等级高于 nvs，应立刻接受\n");
    CHECK(!strcmp(pk_clock_source(), "gps-coarse"), "来源应更新为 gps-coarse\n");

    CHECK(pk_clock_apply_epoch_ms(t_base() + 3600000, "ble-write"),
          "手机等级高于 gps-coarse，应立刻接受\n");
    CHECK(pk_clock_apply_epoch_ms(t_base() + 3600000, "gps"),
          "卫星 fix 等级最高，应立刻接受\n");
    CHECK(!strcmp(pk_clock_source(), "gps"), "来源应更新为 gps\n");
}

/* 同级后来居上：卫星授时每秒都在刷新，同一个来源不该被自己挡住。 */
static void test_same_tier_latest_wins(void)
{
    reset(1000000);
    CHECK(pk_clock_apply_epoch_ms(t_base(), "gps"), "前置条件\n");
    g_now_us += 1000000;
    CHECK(pk_clock_apply_epoch_ms(t_base() + 1000, "gps"),
          "同级应后来居上\n");

    /* 手机两个来源同级，互相之间也不该挡。 */
    reset(1000000);
    CHECK(pk_clock_apply_epoch_ms(t_base(), "ble-write"), "前置条件\n");
    CHECK(pk_clock_apply_epoch_ms(t_base() + 1000, "ios-cts"),
          "ble-write 与 ios-cts 同级，应互相允许\n");
}

/* 高级的值放久了就不再新鲜，低级可以接管。
 *
 * 现场：GPS 校过之后进了隧道／室内，几小时都没有新的卫星授时，此时手机连上来
 * 报了个时间——该采信手机，而不是抱着几小时前的旧值不放。 */
static void test_stale_high_tier_yields_to_low_tier(void)
{
    reset(1000000);
    CHECK(pk_clock_apply_epoch_ms(t_base(), "gps"), "前置条件\n");

    /* 窗口之内仍要挡住——把窗口缩到 0 的"修复"必须在这里变红。 */
    g_now_us += PK_CLOCK_PRIORITY_US - 1000;
    CHECK(!pk_clock_apply_epoch_ms(t_base() + 60000, "ble-write"),
          "窗口内不得被低级覆盖\n");

    g_now_us += 2000;   /* 跨过窗口 */
    CHECK(pk_clock_apply_epoch_ms(t_base() + 60000, "ble-write"),
          "窗口外应允许低级接管\n");
    CHECK(!strcmp(pk_clock_source(), "ble-write"), "来源应更新为 ble-write\n");
}

/* 2024 下限对**每一个**来源都生效，包括最高级的 gps。
 * 这道闸挡的是"模块报了个 2000 年"这类脏数据，与来源等级无关。 */
static void test_min_epoch_applies_to_all_sources(void)
{
    const int64_t too_old = 1000000000000LL;   /* 2001-09-09，早于 2024 */

    reset(1000000);
    CHECK(!pk_clock_apply_epoch_ms(too_old, "gps"),
          "早于下限的 epoch 即使来自 gps 也必须拒绝\n");
    CHECK(!pk_clock_is_synced(), "被下限挡下后不该变成已校时\n");

    CHECK(!pk_clock_apply_epoch_ms(too_old, "nvs"),
          "早于下限的 epoch 来自 nvs 同样拒绝\n");
}

/* 未知来源不得冒充高级：拼错的 "gpss" 只能落到最低档，不能压过真 GPS。 */
static void test_unknown_source_is_lowest(void)
{
    reset(1000000);
    CHECK(pk_clock_apply_epoch_ms(t_base(), "gps"), "前置条件\n");
    CHECK(!pk_clock_apply_epoch_ms(t_base() + 60000, "gpss"),
          "未知来源不得覆盖新鲜的 gps\n");
}

int main(void)
{
    printf("test_pk_clock_priority\n");
    test_first_write_accepted_from_any_source();
    test_low_tier_cannot_override_fresh_high_tier();
    test_high_tier_overrides_low_tier_immediately();
    test_same_tier_latest_wins();
    test_stale_high_tier_yields_to_low_tier();
    test_min_epoch_applies_to_all_sources();
    test_unknown_source_is_lowest();

    if (g_fail) { printf("FAILED: %d\n", g_fail); return 1; }
    printf("OK\n");
    return 0;
}
