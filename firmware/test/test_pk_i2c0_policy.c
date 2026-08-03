/* test_pk_i2c0_policy.c — host proof for pk_i2c0_policy（I²C0 总线级恢复的策略层）。
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 *      -o /tmp/test_i2c0 firmware/test/test_pk_i2c0_policy.c && /tmp/test_i2c0
 *
 *   ASan/UBSan：
 *   cc -std=c11 -Wall -Wextra -Werror -O0 -g -fsanitize=address,undefined \
 *      -I firmware/main -o /tmp/test_i2c0_asan \
 *      firmware/test/test_pk_i2c0_policy.c && /tmp/test_i2c0_asan
 *
 * 同 test_pk_vib.c / test_touch_arbiter.c 惯例：把被测 .c 直接 #include 进同一 TU。
 *
 * 测的是 2026-08-03 那次总线塌陷（见 pk_i2c0_policy.h 文件头的日志）之后
 * 加的四条规矩，没有一条能靠"读代码觉得对"来保证：
 *
 *   去抖   —— 偶尔一次 NACK 不许把整条总线 reset 掉；
 *   冷却   —— 一轮恢复之后要隔一段时间，否则"恢复 → 器件还没 init 完 →
 *             又判失败 → 又恢复"会自己咬尾巴；
 *   退避   —— 连续救不回来时间隔要涨，不能把 CPU 占满；
 *   不重入 —— 总线是共享的，imu 和 baro 两个任务不能同时 reset。
 */
#include <stdio.h>

#include "../main/pk_i2c0_policy.c"

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

#define SEC(x)  ((int64_t)((x) * 1000000LL))

/* ==================================================== 去抖（detector） */

/* baro 的实参：连续 5 次失败**且**这串失败已持续 ≥2 s。
 * 正常读数循环是 100 ms 一轮，所以真正的判据是那 2 秒。 */
static void det_baro(pk_i2c0_detector_t *d)
{
    pk_i2c0_detector_init(d, 5, SEC(2));
}

static void test_single_glitch_never_asks(void)
{
    pk_i2c0_detector_t d; det_baro(&d);

    /* 一次失败夹在成功之间：总线只是抖了一下，绝不能因此 reset。 */
    for (int i = 0; i < 100; i++) {
        int64_t t = SEC(0) + i * 100000LL;
        CHECK(pk_i2c0_detector_report(&d, (i % 7) != 0, t) == false);
    }
}

static void test_count_threshold_alone_is_not_enough(void)
{
    pk_i2c0_detector_t d; det_baro(&d);

    /* 次数够了（5 次）但时间没到（0.4 s）——不许喊。
     * 这条挡的是"轮询快的器件用半秒就把总线 reset 了"。 */
    for (int i = 0; i < 5; i++) {
        CHECK(pk_i2c0_detector_report(&d, false, i * 100000LL) == false);
    }
}

static void test_span_threshold_alone_is_not_enough(void)
{
    pk_i2c0_detector_t d; det_baro(&d);

    /* 时间够了（10 s）但只失败了 3 次——不许喊。
     * 这条挡的是"5 秒才轮询一次的调用者，两三次失败就把总线 reset 了"。 */
    CHECK(pk_i2c0_detector_report(&d, false, SEC(0))  == false);
    CHECK(pk_i2c0_detector_report(&d, false, SEC(5))  == false);
    CHECK(pk_i2c0_detector_report(&d, false, SEC(10)) == false);
}

static void test_real_incident_timing(void)
{
    pk_i2c0_detector_t d; det_baro(&d);

    /* 复刻真机：baro 从 13.619 s 起每 100 ms 失败一次，再没恢复。
     * 期望在 ~15.6 s（第一次失败之后满 2 s 的那一轮）喊一次。 */
    const int64_t t0 = 13619000LL;
    int   asked_at_idx = -1;
    for (int i = 0; i < 30; i++) {
        if (pk_i2c0_detector_report(&d, false, t0 + i * 100000LL)) {
            asked_at_idx = i;
            break;
        }
    }
    CHECK(asked_at_idx == 20);   /* 第 21 轮 = t0 + 2.0 s */
}

static void test_success_clears_the_streak(void)
{
    pk_i2c0_detector_t d; det_baro(&d);

    /* 失败 19 轮（1.9 s）之后成功了一次，计数必须清零：
     * 否则再失败一次就够 2 s 门槛，等于去抖形同虚设。 */
    for (int i = 0; i < 19; i++) {
        CHECK(pk_i2c0_detector_report(&d, false, i * 100000LL) == false);
    }
    CHECK(pk_i2c0_detector_report(&d, true, SEC(1.9)) == false);
    CHECK(d.fail_count == 0);
    CHECK(pk_i2c0_detector_report(&d, false, SEC(2.0)) == false);
}

static void test_asking_rearms_the_debounce_window(void)
{
    pk_i2c0_detector_t d; det_baro(&d);

    /* 一串永不结束的失败：每过一个完整的去抖窗口最多喊一次，
     * 不能每 100 ms 都喊（那是请求风暴），也不能喊完一次就再也不喊
     * （那样这轮恢复失败之后就没人再提了）。 */
    int asks = 0;
    for (int i = 0; i < 200; i++) {   /* 20 s */
        if (pk_i2c0_detector_report(&d, false, i * 100000LL)) asks++;
    }
    CHECK(asks == 9);   /* 20 s / 2 s 窗口，首轮从 i=0 起算 */
}

static void test_imu_params_two_stalls(void)
{
    /* imu 的实参：门槛 2 次、不看时长（上游 stall watchdog 已带 5 s 去抖）。 */
    pk_i2c0_detector_t d;
    pk_i2c0_detector_init(&d, 2, 0);

    CHECK(pk_i2c0_detector_report(&d, false, SEC(18)) == false);  /* 第 1 次 stall */
    CHECK(pk_i2c0_detector_report(&d, false, SEC(26)) == true);   /* 第 2 次 → 升级 */
}

static void test_zero_count_is_clamped(void)
{
    /* min_fail_count=0 是配置写错，不能变成"一次都不用失败就 reset 总线"。 */
    pk_i2c0_detector_t d;
    pk_i2c0_detector_init(&d, 0, 0);
    CHECK(d.min_fail_count == 1);
    CHECK(pk_i2c0_detector_report(&d, true, 0) == false);
}

/* ============================================ 冷却 / 退避 / 不重入（gate） */

static void gate_std(pk_i2c0_gate_t *g)
{
    pk_i2c0_gate_init(g, SEC(2), SEC(30));   /* 与 pk_i2c0_recover.c 同参 */
}

static void test_first_request_always_passes(void)
{
    pk_i2c0_gate_t g; gate_std(&g);
    /* 开机第一次不该被"上次是 0 时刻"这种初值歧义挡掉。 */
    CHECK(pk_i2c0_gate_begin(&g, 0) == PK_I2C0_GATE_GO);
}

static void test_not_reentrant(void)
{
    pk_i2c0_gate_t g; gate_std(&g);

    /* imu 拿到了闸门，baro 在同一瞬间也来请求 —— 必须被挡在外面，
     * 否则两个任务会同时 i2c_master_bus_reset()。 */
    CHECK(pk_i2c0_gate_begin(&g, SEC(10)) == PK_I2C0_GATE_GO);
    CHECK(pk_i2c0_gate_begin(&g, SEC(10)) == PK_I2C0_GATE_BUSY);
    CHECK(pk_i2c0_gate_begin(&g, SEC(11)) == PK_I2C0_GATE_BUSY);   /* 恢复还没跑完 */

    pk_i2c0_gate_finish(&g, true, SEC(11));
    CHECK(g.busy == false);
}

static void test_finish_always_releases_even_on_failure(void)
{
    pk_i2c0_gate_t g; gate_std(&g);
    /* 恢复失败也必须解锁，否则一次异常路径就把闸门永久焊死。 */
    CHECK(pk_i2c0_gate_begin(&g, SEC(0)) == PK_I2C0_GATE_GO);
    pk_i2c0_gate_finish(&g, false, SEC(0));
    CHECK(g.busy == false);
}

static void test_cooldown_after_success(void)
{
    pk_i2c0_gate_t g; gate_std(&g);

    CHECK(pk_i2c0_gate_begin(&g, SEC(10)) == PK_I2C0_GATE_GO);
    pk_i2c0_gate_finish(&g, true, SEC(11));

    /* 成功之后仍要冷却 base（2 s）：器件那一侧还要 ~1 s 才 init 完，
     * 这段时间里它们照样在报失败。 */
    CHECK(pk_i2c0_gate_begin(&g, SEC(11.5)) == PK_I2C0_GATE_COOLDOWN);
    CHECK(pk_i2c0_gate_begin(&g, SEC(12.9)) == PK_I2C0_GATE_COOLDOWN);
    CHECK(pk_i2c0_gate_begin(&g, SEC(13.0)) == PK_I2C0_GATE_GO);
}

static void test_backoff_grows_then_caps(void)
{
    pk_i2c0_gate_t g; gate_std(&g);

    const double want[] = { 2, 4, 8, 16, 30, 30, 30 };   /* base 翻倍，封顶 30 s */
    int64_t now = 0;
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        CHECK(pk_i2c0_gate_begin(&g, now) == PK_I2C0_GATE_GO);
        pk_i2c0_gate_finish(&g, false, now);
        CHECK(pk_i2c0_gate_cooldown_us(&g) == SEC(want[i]));

        /* 退避期内一律挡下 —— 这是"连续恢复失败不能变成死循环把 CPU 占满"。 */
        CHECK(pk_i2c0_gate_begin(&g, now + SEC(want[i]) - 1) == PK_I2C0_GATE_COOLDOWN);
        now += SEC(want[i]);
    }
}

static void test_success_resets_backoff(void)
{
    pk_i2c0_gate_t g; gate_std(&g);

    /* 失败三轮把退避推到 8 s，然后成功一次 —— 退避必须回到 base，
     * 否则一次坏运气会让后面几分钟内的真故障都反应迟钝。 */
    int64_t now = 0;
    for (int i = 0; i < 3; i++) {
        CHECK(pk_i2c0_gate_begin(&g, now) == PK_I2C0_GATE_GO);
        pk_i2c0_gate_finish(&g, false, now);
        now += pk_i2c0_gate_cooldown_us(&g);
    }
    CHECK(g.consec_fail == 3);

    CHECK(pk_i2c0_gate_begin(&g, now) == PK_I2C0_GATE_GO);
    pk_i2c0_gate_finish(&g, true, now);
    CHECK(g.consec_fail == 0);
    CHECK(pk_i2c0_gate_cooldown_us(&g) == SEC(2));
}

static void test_generation_counts_only_successes(void)
{
    pk_i2c0_gate_t g; gate_std(&g);

    /* 代数是各器件"该重放自己的 bring-up 了"的唯一信号。复位没救回来时
     * 不能 ++：那会让 imu/baro 白白各花 ~1 s 去对着一条死总线重初始化。 */
    CHECK(g.generation == 0);

    int64_t now = 0;
    CHECK(pk_i2c0_gate_begin(&g, now) == PK_I2C0_GATE_GO);
    pk_i2c0_gate_finish(&g, false, now);
    CHECK(g.generation == 0);

    now += SEC(2);
    CHECK(pk_i2c0_gate_begin(&g, now) == PK_I2C0_GATE_GO);
    pk_i2c0_gate_finish(&g, true, now);
    CHECK(g.generation == 1);
}

/* ============================================================ 场景对拍 */

static void test_incident_end_to_end(void)
{
    /* 把真机那条时间线整条走一遍：
     *   13.619 s  baro 开始连续失败（100 ms 一轮，之后再没恢复过）
     *   ~15.6 s   去抖满足 → 请求恢复 → 闸门放行 → 复位成功 → 代数 1
     *   之后 ~1 s baro 正在重写配置、重读标定，这段时间它仍在报失败——
     *             这些失败不许再触发第二次 reset。
     *   16.7 s 起 读数恢复正常。
     * 判据：整条线上总线只被 reset 了一次。 */
    pk_i2c0_detector_t d; det_baro(&d);
    pk_i2c0_gate_t     g; gate_std(&g);

    const int64_t t0 = 13619000LL;
    int64_t recovered_at = -1;
    int     resets = 0;
    for (int i = 0; i < 100; i++) {         /* 13.6 → 23.6 s */
        const int64_t t = t0 + i * 100000LL;
        /* 总线救回来之后器件还要约 1 s 重新初始化，期间照样失败。 */
        const bool ok = (recovered_at >= 0) && (t >= recovered_at + SEC(1));
        if (!pk_i2c0_detector_report(&d, ok, t)) continue;
        if (pk_i2c0_gate_begin(&g, t) != PK_I2C0_GATE_GO) continue;
        /* 一轮恢复 = bus_reset + 两次探活，算 100 ms。 */
        pk_i2c0_gate_finish(&g, true, t + 100000LL);
        recovered_at = t + 100000LL;
        resets++;
    }
    CHECK(resets == 1);          /* 15.619 s 那一次，之后再没有第二次 */
    CHECK(g.generation == 1);
}

int main(void)
{
    test_single_glitch_never_asks();
    test_count_threshold_alone_is_not_enough();
    test_span_threshold_alone_is_not_enough();
    test_real_incident_timing();
    test_success_clears_the_streak();
    test_asking_rearms_the_debounce_window();
    test_imu_params_two_stalls();
    test_zero_count_is_clamped();

    test_first_request_always_passes();
    test_not_reentrant();
    test_finish_always_releases_even_on_failure();
    test_cooldown_after_success();
    test_backoff_grows_then_caps();
    test_success_resets_backoff();
    test_generation_counts_only_successes();

    test_incident_end_to_end();

    if (g_fail == 0) printf("test_pk_i2c0_policy: ALL PASS\n");
    else             printf("test_pk_i2c0_policy: %d FAILED\n", g_fail);
    return g_fail != 0;
}
