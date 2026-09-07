/*
 * test_power_service.c — power_service 公共电源状态模型的 host 单测
 * （WP-D Task 1，TDD 先红后绿）。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -DPOWER_SERVICE_HOST_TEST \
 *      -o /tmp/test_power_service firmware/test/test_power_service.c \
 *      firmware/main/power_service.c && /tmp/test_power_service
 *
 * 只测纯模型部分（注册 / 选择 / stale 判定 / 钳位）：POWER_SERVICE_HOST_TEST
 * 隔离，模式照 test_qmc5883p.c。目标端 FreeRTOS 胶水（1 Hz 轮询任务、
 * esp_timer 取时）不伪造覆盖，靠两板系构建验证。
 *
 * 钉死的契约：
 *   1. 注册：poll 必填（poll=NULL 整体拒收）；同一指针重复注册幂等；
 *      容量 2（WP-D 只有 SY6970/ETA6098 两个 backend），超容量静默忽略、
 *      不得挤掉已注册者；
 *   2. 选择：注册次序 = 优先级。snapshot_at() 返回第一个「未 stale」的
 *      backend 快照；全部 stale 时返回最近更新的一份并置 stale=true
 *      （信息保留给 UI 自行决定隐藏方式）；无 backend → UNKNOWN +
 *      stale（显示宁可"无数据"也不能拿旧数冒充现值）；
 *   3. stale 判定：updated_us<=0 视为从未报数恒 stale；距今 >5 s 为
 *      stale，恰好 5 s 边界不算 stale（> 是严格不等）；
 *   4. 钳位：pct_est>100 一律收 100（uint8_t 无负值，下界 0 天然成立）
 *      ——backend 自身的 bug 不得把百分比显示成 250；
 *   5. poll 合同：backend 收到的 now_us 与轮询拍传参一致（µs 单调时钟，
 *      与 esp_timer_get_time() 同源同单位）；
 *   6. backend 身份戳：注册项自报的 id 由服务盖进聚合快照（诊断页同源
 *      守卫的依据）；未声明身份的 backend 盖 NONE；
 *   7. seqlock：序号奇数（写到一半）时读者重试耗尽后按「本拍没读到」
 *      处理（UNKNOWN/stale），绝不交出撕裂副本；写者下一拍恢复偶数后
 *      数据重新可读。单线程可测的靠山是 host seam：把序号掰成奇数，
 *      模拟读者撞上槽位写到一半。
 */

#include <stdio.h>

#include "../main/power_service.h"

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

/* ── 可编程假 backend ────────────────────────────────────────────────
 * 每个槽位：预设的返回快照 + 记录最近一次收到的 now_us（契约 5）。 */
static int64_t          s_got_us[3];
static power_snapshot_t s_ret[3];

static power_snapshot_t fake_poll(int i, int64_t now_us)
{
    s_got_us[i] = now_us;
    return s_ret[i];
}
static power_snapshot_t fake_poll_0(int64_t us) { return fake_poll(0, us); }
static power_snapshot_t fake_poll_1(int64_t us) { return fake_poll(1, us); }
static power_snapshot_t fake_poll_2(int64_t us) { return fake_poll(2, us); }

static const power_backend_t s_b0 = { "fake-0", POWER_BACKEND_NONE,
                                      fake_poll_0 };
static const power_backend_t s_b1 = { "fake-1", POWER_BACKEND_NONE,
                                      fake_poll_1 };
static const power_backend_t s_b2 = { "fake-2", POWER_BACKEND_NONE,
                                      fake_poll_2 };

static power_snapshot_t mk_snap(power_src_t src, int64_t updated_us,
                                uint16_t mv, uint8_t pct)
{
    power_snapshot_t s;
    s.source           = src;
    s.charging         = false;
    s.vbus_present     = false;
    s.batt_mv          = mv;
    s.pct_est          = pct;
    s.pct_valid        = true;
    s.time_degraded_na = false;
    s.updated_us       = updated_us;
    s.stale            = false;   /* 服务端按 updated_us 重算，不信 backend */
    s.backend          = POWER_BACKEND_NONE;   /* 身份戳由服务端盖，自报不作数 */
    return s;
}

/* ── 1 无 backend：UNKNOWN / stale，绝不编造数据 ───────────────────── */
static void test_no_backend_unknown_stale(void)
{
    power_service_reset();
    const int64_t now = 10000000;   /* 10 s（任意非零参照） */
    power_snapshot_t s = power_service_snapshot_at(now);
    CHECK(s.source == POWER_SRC_UNKNOWN);
    CHECK(s.stale == true);
    CHECK(s.pct_valid == false);
    CHECK(s.updated_us == 0);
    CHECK(s.time_degraded_na == true);  /* 没有数据 → 剩余时间必然不可估 */
}

/* ── 2 注册 + 轮询后：聚合到最新 backend 快照 ──────────────────────── */
static void test_register_poll_then_snapshot_fresh(void)
{
    power_service_reset();
    CHECK(power_service_backend_count() == 0);

    power_service_register(&s_b0);
    CHECK(power_service_backend_count() == 1);

    /* 注册后未轮询：槽位是"从未报数"，必须 stale（不许把初始零值当现值） */
    power_snapshot_t s = power_service_snapshot_at(10000000);
    CHECK(s.stale == true);
    CHECK(s.source == POWER_SRC_UNKNOWN);

    const int64_t now = 12000000;
    s_ret[0] = mk_snap(POWER_SRC_BATTERY, now, 3700, 50);
    power_service_poll_tick(now);
    CHECK(s_got_us[0] == now);          /* 契约 5：now_us 透传给 backend */

    s = power_service_snapshot_at(now);
    CHECK(s.source == POWER_SRC_BATTERY);
    CHECK(s.batt_mv == 3700);
    CHECK(s.pct_est == 50);
    CHECK(s.pct_valid == true);
    CHECK(s.stale == false);
}

/* ── 3 选择：注册次序 = 优先级，首选未 stale 的第一个 ──────────────── */
static void test_selection_prefers_first_registered_when_both_fresh(void)
{
    power_service_reset();
    power_service_register(&s_b0);
    power_service_register(&s_b1);

    const int64_t now = 20000000;
    s_ret[0] = mk_snap(POWER_SRC_BATTERY, now, 1001, 10);
    s_ret[1] = mk_snap(POWER_SRC_BATTERY, now, 2002, 20);
    power_service_poll_tick(now);

    const power_snapshot_t s = power_service_snapshot_at(now);
    CHECK(s.batt_mv == 1001);           /* b0 先注册：两个都 fresh 时它赢 */
    CHECK(s.pct_est == 10);
}

/* ── 4 选择：首选 stale 时自动回落到后面的 backend ─────────────────── */
static void test_selection_falls_back_when_first_stale(void)
{
    power_service_reset();
    power_service_register(&s_b0);
    power_service_register(&s_b1);

    const int64_t now = 30000000;
    s_ret[0] = mk_snap(POWER_SRC_BATTERY, now - 10000000, 1001, 10); /* 10 s 前 */
    s_ret[1] = mk_snap(POWER_SRC_BATTERY, now, 2002, 20);
    power_service_poll_tick(now);

    const power_snapshot_t s = power_service_snapshot_at(now);
    CHECK(s.batt_mv == 2002);           /* b0 数据过期 → 回落 b1 */
    CHECK(s.stale == false);
}

/* ── 5 全部 stale：返回最近更新的一份并如实置 stale ────────────────── */
static void test_all_stale_returns_freshest_with_stale_flag(void)
{
    power_service_reset();
    power_service_register(&s_b0);
    power_service_register(&s_b1);

    const int64_t now = 40000000;
    s_ret[0] = mk_snap(POWER_SRC_BATTERY, now - 10000000, 1001, 10);
    s_ret[1] = mk_snap(POWER_SRC_BATTERY, now - 8000000, 2002, 20);
    power_service_poll_tick(now);

    const power_snapshot_t s = power_service_snapshot_at(now);
    CHECK(s.batt_mv == 2002);           /* 8 s 前比 10 s 前新 */
    CHECK(s.updated_us == now - 8000000);
    CHECK(s.stale == true);             /* 但必须如实报 stale，不许冒充现值 */
}

/* ── 6 stale 边界：>5 s 才 stale，恰好 5 s 不算 ─────────────────────── */
static void test_stale_boundary_is_strictly_greater(void)
{
    power_service_reset();
    power_service_register(&s_b0);

    const int64_t now = 50000000;
    s_ret[0] = mk_snap(POWER_SRC_BATTERY, now - POWER_SERVICE_STALE_US,
                       3700, 50);
    power_service_poll_tick(now);
    CHECK(power_service_snapshot_at(now).stale == false); /* 恰好 5 s：新鲜 */

    power_service_reset();
    power_service_register(&s_b0);
    s_ret[0] = mk_snap(POWER_SRC_BATTERY, now - POWER_SERVICE_STALE_US - 1,
                       3700, 50);
    power_service_poll_tick(now);
    CHECK(power_service_snapshot_at(now).stale == true);  /* 多 1 µs：过期 */
}

/* ── 7 updated_us<=0 视为从未报数，恒 stale ────────────────────────── */
static void test_nonpositive_updated_us_is_stale(void)
{
    power_service_reset();
    power_service_register(&s_b0);

    const int64_t now = 60000000;
    s_ret[0] = mk_snap(POWER_SRC_BATTERY, 0, 3700, 50);   /* 0 = 从未报数哨兵 */
    power_service_poll_tick(now);
    CHECK(power_service_snapshot_at(now).stale == true);
}

/* ── 8 pct_est 钳位 0..100（backend bug 防线）──────────────────────── */
static void test_pct_est_clamped(void)
{
    power_service_reset();
    power_service_register(&s_b0);

    const int64_t now = 70000000;
    s_ret[0] = mk_snap(POWER_SRC_BATTERY, now, 4200, 250);
    power_service_poll_tick(now);
    CHECK(power_service_snapshot_at(now).pct_est == 100); /* 250 → 100 */

    power_service_reset();
    power_service_register(&s_b0);
    s_ret[0] = mk_snap(POWER_SRC_BATTERY, now, 4200, 100);
    power_service_poll_tick(now);
    CHECK(power_service_snapshot_at(now).pct_est == 100); /* 合法 100 不动 */

    power_service_reset();
    power_service_register(&s_b0);
    s_ret[0] = mk_snap(POWER_SRC_BATTERY, now, 3300, 0);
    power_service_poll_tick(now);
    CHECK(power_service_snapshot_at(now).pct_est == 0);   /* 合法 0 不动 */
}

/* ── 9 容量上限 2：第三个注册被忽略，不挤掉已注册者 ────────────────── */
static void test_capacity_capped_at_two(void)
{
    power_service_reset();
    power_service_register(&s_b0);
    power_service_register(&s_b1);
    power_service_register(&s_b2);
    CHECK(power_service_backend_count() == 2);

    const int64_t now = 80000000;
    s_ret[0] = mk_snap(POWER_SRC_BATTERY, now, 1001, 10);
    s_ret[1] = mk_snap(POWER_SRC_BATTERY, now, 2002, 20);
    s_ret[2] = mk_snap(POWER_SRC_BATTERY, now, 3003, 30);
    power_service_poll_tick(now);
    CHECK(s_got_us[2] == 0);            /* b2 没进注册表 → 没被轮询 */
    CHECK(power_service_snapshot_at(now).batt_mv == 1001); /* 次序未被扰动 */
}

/* ── 10 重复注册同一指针幂等：不占双槽、不挤掉别人 ─────────────────── */
static void test_duplicate_register_is_idempotent(void)
{
    power_service_reset();
    power_service_register(&s_b0);
    power_service_register(&s_b0);      /* 同指针再来一次 */
    power_service_register(&s_b1);
    CHECK(power_service_backend_count() == 2);

    const int64_t now = 90000000;
    s_ret[0] = mk_snap(POWER_SRC_BATTERY, now - 10000000, 1001, 10);
    s_ret[1] = mk_snap(POWER_SRC_BATTERY, now, 2002, 20);
    power_service_poll_tick(now);
    CHECK(power_service_snapshot_at(now).batt_mv == 2002); /* b1 必须在表里 */
}

/* ── 11 非法注册拒收：NULL 指针 / poll 缺失 ────────────────────────── */
static void test_invalid_register_rejected(void)
{
    power_service_reset();
    power_service_register(NULL);
    CHECK(power_service_backend_count() == 0);

    static const power_backend_t no_poll = { "no-poll", POWER_BACKEND_NONE,
                                             NULL };
    power_service_register(&no_poll);
    CHECK(power_service_backend_count() == 0);
}

/* ── 12 字段透传：charging / vbus / time_degraded_na / 各电源档 ────── */
static void test_fields_pass_through(void)
{
    power_service_reset();
    power_service_register(&s_b0);

    const int64_t now = 100000000;
    s_ret[0] = mk_snap(POWER_SRC_SY6970_VBUS, now, 4100, 90);
    s_ret[0].charging         = true;
    s_ret[0].vbus_present     = true;
    s_ret[0].time_degraded_na = true;
    power_service_poll_tick(now);

    const power_snapshot_t s = power_service_snapshot_at(now);
    CHECK(s.source == POWER_SRC_SY6970_VBUS);
    CHECK(s.charging == true);
    CHECK(s.vbus_present == true);
    CHECK(s.time_degraded_na == true);

    /* source=EXTERNAL 同样透传（载板有外部电、无电池的档位） */
    power_service_reset();
    power_service_register(&s_b0);
    s_ret[0] = mk_snap(POWER_SRC_EXTERNAL, now, 0, 0);
    s_ret[0].pct_valid    = false;
    s_ret[0].vbus_present = true;
    power_service_poll_tick(now);
    const power_snapshot_t e = power_service_snapshot_at(now);
    CHECK(e.source == POWER_SRC_EXTERNAL);
    CHECK(e.pct_valid == false);
    CHECK(e.vbus_present == true);
}

/* ── 13 backend 身份戳：服务把赢家的 id 盖进聚合快照 ───────────────── */
static void test_backend_id_stamped_into_snapshot(void)
{
    const int64_t now = 110000000;

    /* 注册项声明了身份：快照盖它的 id（诊断页同源守卫的依据） */
    power_service_reset();
    static const power_backend_t b_id = { "fake-sy", POWER_BACKEND_SY6970,
                                          fake_poll_0 };
    power_service_register(&b_id);
    s_ret[0] = mk_snap(POWER_SRC_SY6970_VBUS, now, 3700, 50);
    power_service_poll_tick(now);
    CHECK(power_service_snapshot_at(now).backend == POWER_BACKEND_SY6970);

    /* 未声明身份的 backend：盖 NONE（消费方据此拒绝同源合并） */
    power_service_reset();
    power_service_register(&s_b0);
    s_ret[0] = mk_snap(POWER_SRC_BATTERY, now, 3700, 50);
    power_service_poll_tick(now);
    CHECK(power_service_snapshot_at(now).backend == POWER_BACKEND_NONE);
}

/* ── 14 seqlock：序号奇数（写到一半）读者不得拿到撕裂副本 ──────────── */
static void test_seqlock_write_in_progress_readers_get_no_data(void)
{
    power_service_reset();
    power_service_register(&s_b0);

    const int64_t now = 120000000;
    s_ret[0] = mk_snap(POWER_SRC_BATTERY, now, 3700, 50);
    power_service_poll_tick(now);

    /* 正常路径：已提交快照的一次一致读 */
    power_snapshot_t s = power_service_snapshot_at(now);
    CHECK(s.stale == false);
    CHECK(s.batt_mv == 3700);

    /* 把序号掰成奇数，模拟读者撞上「槽位写到一半」 */
    power_service_test_seq_break(0);
    s = power_service_snapshot_at(now);
    /* 重试耗尽：本拍按「没读到」处理——宁可 UNKNOWN/stale，也绝不把
     * 可能撕裂的副本交出去（RV32 上 int64_t updated_us 撕了就是垃圾） */
    CHECK(s.source == POWER_SRC_UNKNOWN);
    CHECK(s.stale == true);
    CHECK(s.updated_us == 0);

    /* 写者下一拍走完整写协议（收尾必回偶数），数据重新可读 */
    power_service_poll_tick(now + 1000000);
    s = power_service_snapshot_at(now + 1000000);
    CHECK(s.stale == false);
    CHECK(s.batt_mv == 3700);
}

int main(void)
{
    test_no_backend_unknown_stale();
    test_register_poll_then_snapshot_fresh();
    test_selection_prefers_first_registered_when_both_fresh();
    test_selection_falls_back_when_first_stale();
    test_all_stale_returns_freshest_with_stale_flag();
    test_stale_boundary_is_strictly_greater();
    test_nonpositive_updated_us_is_stale();
    test_pct_est_clamped();
    test_capacity_capped_at_two();
    test_duplicate_register_is_idempotent();
    test_invalid_register_rejected();
    test_fields_pass_through();
    test_backend_id_stamped_into_snapshot();
    test_seqlock_write_in_progress_readers_get_no_data();

    if (g_fail == 0) {
        printf("test_power_service: all OK\n");
        return 0;
    }
    printf("test_power_service: %d FAIL\n", g_fail);
    return 1;
}
