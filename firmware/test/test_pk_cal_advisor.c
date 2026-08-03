/* test_pk_cal_advisor.c — host proof for pk_cal_advisor（罗盘校准提示判定状态机）。
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main -o /tmp/test_cal_advisor \
 *      firmware/test/test_pk_cal_advisor.c -lm && /tmp/test_cal_advisor
 *
 *   ASan/UBSan：
 *   cc -std=c11 -Wall -Wextra -Werror -O0 -g -fsanitize=address,undefined \
 *      -I firmware/main -o /tmp/test_cal_advisor_asan \
 *      firmware/test/test_pk_cal_advisor.c -lm && /tmp/test_cal_advisor_asan
 *
 *   leaks（macOS）：
 *   cc -std=c11 -Wall -Wextra -Werror -O0 -g -I firmware/main \
 *      -o /tmp/test_cal_advisor_leaks firmware/test/test_pk_cal_advisor.c -lm && \
 *      leaks --atExit -- /tmp/test_cal_advisor_leaks
 *
 * 同 test_pk_flight_phase.c 惯例：把被测 .c 直接 #include 进同一 TU。本模块只
 * 用到 pk_flight_phase.h 里的相位枚举（纯 enum），不需要连带编译
 * pk_flight_phase.c / geo.c。
 *
 * 逐场景覆盖 IMPLEMENTATION_PLAN.md「阶段 C1」的 SC1-SC8。每个场景喂一段时间
 * 序列，断言的是**每一拍**的 advice，不是只看终态——这个模块的 bug 恰恰长在
 * 中间态上（旧实现单帧抖到 acc≥2 就把闸门开了，终态看不出来）。
 */
#include <stdio.h>
#include <string.h>

#include "../main/pk_cal_advisor.c"

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

#define CHECK_ADVICE(got, want) do { \
    if ((got) != (want)) { \
        fprintf(stderr, "FAIL %s:%d: advice=%d want=%d\n", __FILE__, __LINE__, \
                (int)(got), (int)(want)); \
        g_fail++; \
    } \
} while (0)

/* 一拍 100 ms。真机是渲染线程每帧调（pfd.c:160，30+ FPS ≈ 33 ms），这里放宽到
 * 100 ms 纯粹是为了让 20 s / 30 s 这些窗口对应到整数拍数（200 / 300 拍）便于
 * 卡边界；状态机本身不假设固定周期，所有判据都是 (now - then) 的时间差。 */
#define TICK_MS 100u

#define ANY_ADVICE (-1) /* feed() 的 expect 传这个＝这一段不逐拍断言 */

/* 连喂 ticks 拍，每拍推进 TICK_MS，逐拍断言 advice（expect >= 0 时）。
 * *t_ms 原地推进到下一拍的时刻，便于调用方接着卡边界。
 * 用「拍数」而不是「结束时刻」控制循环，是为了 SC8 的 uint32 回绕场景也能照
 * 用——那里结束时刻的数值比起始时刻还小，写不出 t <= end 的循环条件。 */
static pk_cal_advice_t feed(pk_cal_advisor_t *st, uint32_t *t_ms, unsigned ticks,
                            bool imu_valid, uint8_t accuracy, pk_flight_phase_t phase,
                            int expect, const char *tag)
{
    pk_cal_advice_t adv = PK_CAL_ADVICE_NONE;
    for (unsigned i = 0; i < ticks; i++) {
        adv = pk_cal_advisor_update(st, *t_ms, imu_valid, accuracy, phase);
        if (expect >= 0 && (int)adv != expect) {
            fprintf(stderr, "FAIL [%s] tick %u @t=%u: advice=%d want=%d\n",
                    tag, i, *t_ms, (int)adv, expect);
            g_fail++;
        }
        *t_ms += TICK_MS;
    }
    return adv;
}

/* ================================================================== SC1 */
/* 骚扰循环回归（当前线上 bug 的红灯）。
 *
 * 老逻辑（ui_state.c:277-282）：只要**单帧** acc≥2 就 s_cal_auto_suppressed
 * = false，而 tick 是每帧调的。于是 acc 在机坪抖到 2 一帧 → 用户刚按下的
 * 「稍后再说」当场作废 → 掉回 0 熬满窗口又被强拽回校准页，约 13 s 一轮。
 *
 * 新逻辑：闸门只在 acc≥2 **连续保持 PK_CAL_REARM_MS** 后才解除，单帧抖动作
 * 废不了用户的意图。断言全程只降级成 HINT（状态栏图标），永不再抢页面。
 *
 * 关键：这里只制造**一次** 0→2→0（＝2 次跨阈跳变，< PK_CAL_JAM_CROSSINGS），
 * 并显式断言 is_jammed()==false——否则「没弹页」可能是被干扰识别兜住的，
 * 这条回归就白测了（测试通过的理由必须是滞回，不是别的分支）。 */
static void test_sc1_single_frame_blip_does_not_rearm(void)
{
    pk_cal_advisor_t st;
    pk_cal_advisor_reset(&st);
    uint32_t t = 0;

    /* 累计窗口内（t=0..19900，200 拍）不给任何提示。 */
    feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_NONE, "SC1 累计中");

    /* t=20000 整：acc=0 连续满 PK_CAL_ENTER_MS，地面静止 → 建议弹整页。 */
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);

    /* 用户按「稍后再说」。 */
    pk_cal_advisor_dismiss(&st, t);
    t += TICK_MS;

    /* 之后一律降级成状态栏图标：信息还在，但不再抢页面。 */
    feed(&st, &t, 10, true, 0, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_HINT, "SC1 dismiss 后");

    /* 单帧抖到 acc=2（BNO085 在 GPU 电源车旁边就是这么抖的）。这一拍本身
     * 不给提示：acc≥2 把低精度计时器清零了。 */
    adv = pk_cal_advisor_update(&st, t, true, 2, PK_PHASE_GROUND_STOPPED);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_NONE);
    t += TICK_MS;

    /* 掉回 0：低精度重新起算，前 20 s 仍是 NONE。 */
    feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_NONE, "SC1 blip 后重新累计");

    /* 再熬 40 s（两倍于 ENTER 窗口）。老逻辑在这里会一路把页面抢回去；
     * 新逻辑必须全程 HINT。 */
    feed(&st, &t, 400, true, 0, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_HINT, "SC1 闸门仍关着");

    /* 没弹页的理由必须是滞回，不是干扰识别。 */
    CHECK(pk_cal_advisor_is_jammed(&st) == false);
}

/* ================================================================== SC2 */
/* 真的校准好了：acc≥2 连续 PK_CAL_REARM_MS → 闸门重新武装；此后掉回 0 满
 * PK_CAL_ENTER_MS → 允许再次弹整页。此时的提示是有信息量的——设备确实完成
 * 过一次校准，再退化说明换了环境。
 *
 * 同时用一份「只保持 29.9 s」的对照 advisor 卡住 REARM 边界：差一拍都不解除，
 * 证明 SC1 的不弹页不是因为闸门根本解除不了。 */
static void test_sc2_sustained_good_accuracy_rearms_gate(void)
{
    pk_cal_advisor_t st;
    pk_cal_advisor_reset(&st);
    uint32_t t = 0;

    feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_NONE, "SC2 累计中");
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);
    pk_cal_advisor_dismiss(&st, t);
    t += TICK_MS;
    feed(&st, &t, 10, true, 0, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_HINT, "SC2 dismiss 后");

    /* 用户真的画了 8 字：acc≥2 连续 300 拍（29.9 s，最后一拍差 100 ms 到
     * PK_CAL_REARM_MS）。这一段全程 NONE——精度够了本来就没什么好提示的。 */
    uint32_t high_since = t;
    feed(&st, &t, 300, true, 2, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_NONE, "SC2 高精度保持");
    CHECK(t == high_since + PK_CAL_REARM_MS); /* 下一拍正好是 30 s 整 */

    /* 第 30 s 整这一拍：闸门解除。 */
    adv = pk_cal_advisor_update(&st, t, true, 2, PK_PHASE_GROUND_STOPPED);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_NONE);
    t += TICK_MS;

    /* 掉回 acc=0（换了个磁环境/贴近了干扰源）：熬满 ENTER 窗口后应当重新
     * 允许弹整页。 */
    feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_NONE, "SC2 二次退化累计");
    adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);

    /* 全程只有 0→2 与 2→0 两次跨阈，不该被判成干扰环境。 */
    CHECK(pk_cal_advisor_is_jammed(&st) == false);

    /* ---- 对照组：同一段序列，acc≥2 只保持 29.9 s（差一拍） ---- */
    pk_cal_advisor_t st_short;
    pk_cal_advisor_reset(&st_short);
    uint32_t t2 = 0;
    feed(&st_short, &t2, 200, true, 0, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_NONE, "SC2b 累计中");
    adv = pk_cal_advisor_update(&st_short, t2, true, 0, PK_PHASE_GROUND_STOPPED);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);
    pk_cal_advisor_dismiss(&st_short, t2);
    t2 += TICK_MS;

    feed(&st_short, &t2, 299, true, 2, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_NONE, "SC2b 高精度差一拍");
    /* 立刻掉回 0：REARM 没走满，闸门仍关着 → 之后再久也只能是 HINT。 */
    feed(&st_short, &t2, 200, true, 0, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_NONE, "SC2b 二次累计");
    feed(&st_short, &t2, 100, true, 0, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_HINT, "SC2b 闸门仍关着");
}

/* ================================================================== SC3 */
/* 相位门控：滑行 / 起飞滑跑 / 空中 / 落地滑跑，一律只给状态栏图标，永不抢
 * 页面。飞行中把 PFD 换成校准向导是安全问题，不是骚扰问题。 */
static void test_sc3_in_motion_phases_never_take_the_page(void)
{
    static const pk_flight_phase_t IN_MOTION[] = {
        PK_PHASE_TAXI, PK_PHASE_TAKEOFF_ROLL, PK_PHASE_AIRBORNE, PK_PHASE_LANDING_ROLLOUT,
    };

    for (size_t i = 0; i < sizeof(IN_MOTION) / sizeof(IN_MOTION[0]); i++) {
        pk_cal_advisor_t st;
        pk_cal_advisor_reset(&st);
        uint32_t t = 0;

        feed(&st, &t, 200, true, 0, IN_MOTION[i], PK_CAL_ADVICE_NONE, "SC3 累计中");
        /* 熬满窗口后再跑 60 s：全程 HINT，一次 WIZARD 都不能有。 */
        feed(&st, &t, 600, true, 0, IN_MOTION[i], PK_CAL_ADVICE_HINT, "SC3 飞行中只给 HINT");
        CHECK(pk_cal_advisor_is_jammed(&st) == false);
    }
}

/* ================================================================== SC4 */
/* UNKNOWN 放行：它覆盖「刚开机、GPS 还没定位」这个最该提示的场景
 * （pk_flight_phase.h 明确 GPS 无效时整段跳过、相位原样返回，开机初值就是
 * UNKNOWN）。机库/廊桥同样是 UNKNOWN，那种场合交给 jammed 兜（见 SC5）。 */
static void test_sc4_unknown_phase_allows_wizard(void)
{
    pk_cal_advisor_t st;
    pk_cal_advisor_reset(&st);
    uint32_t t = 0;

    feed(&st, &t, 200, true, 0, PK_PHASE_UNKNOWN, PK_CAL_ADVICE_NONE, "SC4 累计中");
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_UNKNOWN);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);
}

/* ---------------------------------------------- SC5/SC6 共用前置 */
/* 制造 4 次跨阈跳变（0→2→0→2→0，每次间隔 1 s，全在 60 s 窗口内），把 advisor
 * 打进 jammed。返回最后一次跳变的时刻——jam 解除的 30 s 从它起算。 */
static uint32_t drive_into_jam(pk_cal_advisor_t *st, uint32_t base_ms)
{
    /* 首拍只是播种：要有「上一拍的 acc」才谈得上跨阈，第一条样本不算跳变。 */
    pk_cal_advisor_update(st, base_ms, true, 0, PK_PHASE_GROUND_STOPPED);
    CHECK(pk_cal_advisor_is_jammed(st) == false);

    pk_cal_advisor_update(st, base_ms + 1000u, true, 2, PK_PHASE_GROUND_STOPPED); /* 跨阈 1（上行） */
    pk_cal_advisor_update(st, base_ms + 2000u, true, 0, PK_PHASE_GROUND_STOPPED); /* 跨阈 2（下行） */
    CHECK(pk_cal_advisor_is_jammed(st) == false); /* 2 < PK_CAL_JAM_CROSSINGS，还不算 */

    pk_cal_advisor_update(st, base_ms + 3000u, true, 2, PK_PHASE_GROUND_STOPPED); /* 跨阈 3 */
    CHECK(pk_cal_advisor_is_jammed(st) == true);  /* 达到阈值，当拍置位 */

    pk_cal_advisor_update(st, base_ms + 4000u, true, 0, PK_PHASE_GROUND_STOPPED); /* 跨阈 4 */
    CHECK(pk_cal_advisor_is_jammed(st) == true);
    return base_ms + 4000u;
}

/* ================================================================== SC5 */
/* 强磁干扰环境：60 s 内 4 次跨阈跳变 → jammed，advice 强制 NONE。
 * 产品理由：这种环境下画 8 字物理上救不回来，提示是纯无效骚扰。
 * 注意最后那段——低精度计时器早就熬过 ENTER 窗口了（低精度从最后一次跳变
 * 起算，20 s 后满窗），jammed 必须把它压住。 */
static void test_sc5_jam_detection_forces_none(void)
{
    pk_cal_advisor_t st;
    pk_cal_advisor_reset(&st);

    uint32_t last_cross = drive_into_jam(&st, 0);
    uint32_t t = last_cross + TICK_MS;

    /* 一路喂到 jam 解除前的最后一拍（last_cross + 29900）：全程 NONE，
     * 其中 last_cross+20000 那一拍低精度已满窗，正是 jammed 起作用的证据。 */
    feed(&st, &t, 299, true, 0, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_NONE, "SC5 干扰期间闭嘴");
    CHECK(t == last_cross + PK_CAL_JAM_CLEAR_MS);
    CHECK(pk_cal_advisor_is_jammed(&st) == true);
}

/* ================================================================== SC6 */
/* 干扰解除：跳变停止 PK_CAL_JAM_CLEAR_MS 后 jammed 落下，判定恢复正常——
 * 此时低精度计时器已经熬过 ENTER 窗口，地面静止 → 当拍就该给 WIZARD。 */
static void test_sc6_jam_clears_after_quiet_window(void)
{
    pk_cal_advisor_t st;
    pk_cal_advisor_reset(&st);

    uint32_t last_cross = drive_into_jam(&st, 0);
    uint32_t t = last_cross + TICK_MS;

    feed(&st, &t, 299, true, 0, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_NONE, "SC6 静默期");
    CHECK(pk_cal_advisor_is_jammed(&st) == true); /* 差一拍还不解除 */

    /* 正好静默满 30 s 这一拍：解除，并且被压住的提示立刻放出来。 */
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED);
    CHECK(pk_cal_advisor_is_jammed(&st) == false);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);
}

/* ================================================================== SC7 */
/* EXIT 语义不回归：acc≥PK_CAL_EXIT_ACCURACY 持续 PK_CAL_EXIT_MS → 向导该退出。
 * advisor 侧的表现是两条：整段不再给 WIZARD/HINT（精度够了没什么好提示的），
 * 且 should_exit_wizard() 在满 3 s 那一拍翻真。
 *
 * 末尾那段是 ui_state.c:246-250 那条真机结论的回归：用户从设置页主动进来时
 * 必须把高精度计时器清掉，否则设备本来就校准好（acc≥2 已经保持了几分钟），
 * 这一页当场闪一下就被弹回 PFD。 */
static void test_sc7_exit_after_sustained_good_accuracy(void)
{
    pk_cal_advisor_t st;
    pk_cal_advisor_reset(&st);
    uint32_t t = 0;

    for (unsigned i = 0; i < 30; i++) { /* t=0..2900，差一拍到 3 s */
        pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 2, PK_PHASE_GROUND_STOPPED);
        CHECK_ADVICE(adv, PK_CAL_ADVICE_NONE);
        CHECK(pk_cal_advisor_should_exit_wizard(&st) == false);
        t += TICK_MS;
    }

    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 2, PK_PHASE_GROUND_STOPPED);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_NONE);
    CHECK(pk_cal_advisor_should_exit_wizard(&st) == true);

    /* 用户此刻从设置页主动打开校准页：退出条件必须复位，给他至少 3 s 的
     * 窗口，不能刚点开就被弹走。 */
    pk_cal_advisor_user_open(&st, t);
    CHECK(pk_cal_advisor_should_exit_wizard(&st) == false);

    /* 高精度连续段从 user_open 之后的**第一拍**（t+100）重新起算，所以满 3 s
     * 落在 t+3100 而不是 t+3000——这里连喂 30 拍（到 t+3000 为止）仍不该退出。 */
    t += TICK_MS;
    feed(&st, &t, 30, true, 2, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_NONE, "SC7 user_open 后");
    CHECK(pk_cal_advisor_should_exit_wizard(&st) == false); /* 重新数 3 s */
    pk_cal_advisor_update(&st, t, true, 2, PK_PHASE_GROUND_STOPPED);
    CHECK(pk_cal_advisor_should_exit_wizard(&st) == true);
}

/* ================================================================== SC8 */
/* now_ms 是 uint32 毫秒，49.7 天回绕一次。盒子长期通电（机库里插着电）撞得上。
 * 所有时间比较必须是无符号差值 (now - then)，直接比大小会在回绕那一刻把
 * 「已经等了 20 s」算成「等了 -49 天」，提示永远出不来（或反过来当场就出来）。
 *
 * 这里把整个 ENTER 窗口和整个 jam 窗口都骑在回绕点上。 */
static void test_sc8_uint32_wraparound(void)
{
    /* ---- A. ENTER 窗口跨回绕 ---- */
    pk_cal_advisor_t st;
    pk_cal_advisor_reset(&st);

    /* 回绕前 10 s 起算：200 拍里第 100 拍正好落在 UINT32_MAX 上，第 101 拍溢出。 */
    uint32_t t = 0xFFFFFFFFu - 10000u;
    uint32_t low_since = t;
    feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_NONE, "SC8 跨回绕累计");
    CHECK(t == (uint32_t)(low_since + PK_CAL_ENTER_MS)); /* 已经绕过 0 了 */
    CHECK(t < low_since);                                /* 数值上更小——朴素比较必错 */

    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);

    /* ---- B. 干扰窗口 + 解除计时跨回绕 ---- */
    pk_cal_advisor_t st2;
    pk_cal_advisor_reset(&st2);

    /* 4 次跳变分别落在回绕点两侧（-1000 / 0 / +1000 / +2000）。 */
    uint32_t last_cross = drive_into_jam(&st2, 0xFFFFFFFFu - 2000u);
    CHECK(last_cross < 0xFFFFFFFFu - 2000u); /* 确认最后一次跳变已经绕过 0 */

    uint32_t t2 = last_cross + TICK_MS;
    feed(&st2, &t2, 299, true, 0, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_NONE, "SC8 跨回绕静默");
    CHECK(pk_cal_advisor_is_jammed(&st2) == true);

    adv = pk_cal_advisor_update(&st2, t2, true, 0, PK_PHASE_GROUND_STOPPED);
    CHECK(pk_cal_advisor_is_jammed(&st2) == false);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);
}

/* ================================================================== 杂项 */

/* reset 之后是「什么都没发生过」：没有累计、闸门开着、不算干扰。 */
static void test_reset_is_clean_slate(void)
{
    pk_cal_advisor_t st;
    memset(&st, 0xAA, sizeof(st)); /* 先塞满垃圾，证明 reset 真的全清 */
    pk_cal_advisor_reset(&st);

    CHECK(pk_cal_advisor_is_jammed(&st) == false);
    CHECK(pk_cal_advisor_should_exit_wizard(&st) == false);
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, 0, true, 0, PK_PHASE_GROUND_STOPPED);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_NONE);
}

/* acc==1（融合在途）与 imu_valid=false（取样断流）都不推进也不清零计时器
 * ——沿用 ui_state.c:283-286 的 else 分支行为。这里验证「不清零」这一半：
 * 低精度累计中途插进 acc=1 和无效样本，满窗时刻不会被推后。 */
static void test_acc1_and_invalid_do_not_reset_timers(void)
{
    pk_cal_advisor_t st;
    pk_cal_advisor_reset(&st);
    uint32_t t = 0;

    feed(&st, &t, 50, true, 0, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_NONE, "SC-misc acc0");
    feed(&st, &t, 50, true, 1, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_NONE, "SC-misc acc1");
    feed(&st, &t, 50, false, 0, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_NONE, "SC-misc invalid");
    feed(&st, &t, 50, true, 0, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_NONE, "SC-misc acc0 again");

    CHECK(t == PK_CAL_ENTER_MS); /* 200 拍整 */
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);

    /* acc 0→1 不算跨阈（阈值是 2），全程零跳变。 */
    CHECK(pk_cal_advisor_is_jammed(&st) == false);
}

/* imu_valid=false 的样本不参与跨阈统计：断流不是磁环境变化。
 * 构造：acc 在 0 与 2 之间来回，但每次变化都发生在无效样本上——真正的
 * 有效样本序列是 0,0,0...（无跳变），不该被判成干扰。 */
static void test_invalid_samples_do_not_count_as_crossings(void)
{
    pk_cal_advisor_t st;
    pk_cal_advisor_reset(&st);

    for (unsigned i = 0; i < 8; i++) {
        pk_cal_advisor_update(&st, i * 1000u,        true,  0, PK_PHASE_GROUND_STOPPED);
        pk_cal_advisor_update(&st, i * 1000u + 500u, false, 2, PK_PHASE_GROUND_STOPPED);
    }
    CHECK(pk_cal_advisor_is_jammed(&st) == false);
}

/* 设置页主动进入：闸门重新武装 + 两条计时器清零（沿用
 * pk_ui_cal_wizard_enter() 的语义）。用户此刻的动作说明他改主意了。 */
static void test_user_open_rearms_and_clears_timers(void)
{
    pk_cal_advisor_t st;
    pk_cal_advisor_reset(&st);
    uint32_t t = 0;

    feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_NONE, "SC-open 累计中");
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);
    pk_cal_advisor_dismiss(&st, t);
    t += TICK_MS;
    feed(&st, &t, 10, true, 0, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_HINT, "SC-open dismiss 后");

    /* 用户改主意，从设置页主动进来：低精度计时器清零 → 下一拍起重新累计。 */
    pk_cal_advisor_user_open(&st, t);
    feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, PK_CAL_ADVICE_NONE, "SC-open 重新累计");
    adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD); /* 闸门已重新武装 */
}

int main(void)
{
    test_reset_is_clean_slate();
    test_sc1_single_frame_blip_does_not_rearm();
    test_sc2_sustained_good_accuracy_rearms_gate();
    test_sc3_in_motion_phases_never_take_the_page();
    test_sc4_unknown_phase_allows_wizard();
    test_sc5_jam_detection_forces_none();
    test_sc6_jam_clears_after_quiet_window();
    test_sc7_exit_after_sustained_good_accuracy();
    test_sc8_uint32_wraparound();
    test_acc1_and_invalid_do_not_reset_timers();
    test_invalid_samples_do_not_count_as_crossings();
    test_user_open_rearms_and_clears_timers();

    if (g_fail == 0) {
        printf("PASS: all pk_cal_advisor tests passed\n");
        return 0;
    }
    fprintf(stderr, "FAIL: %d assertion(s) failed\n", g_fail);
    return 1;
}
