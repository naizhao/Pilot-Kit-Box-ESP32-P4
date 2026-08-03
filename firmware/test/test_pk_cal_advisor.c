/* test_pk_cal_advisor.c — host proof for pk_cal_advisor（罗盘校准提示判定状态机）。
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main -o /tmp/test_cal_advisor \
 *      firmware/test/test_pk_cal_advisor.c -lm && /tmp/test_cal_advisor
 *
 *   ASan/UBSan：
 *   cc -std=c11 -Wall -Wextra -Werror -O0 -g -fsanitize=address,undefined \
 *      -I firmware/main -o /tmp/test_cal_advisor_asan \
 *      firmware/test/test_pk_cal_advisor.c -lm && /tmp/test_cal_advisor_asan
 *
 * 同 test_pk_flight_phase.c 惯例：把被测 .c 直接 #include 进同一 TU。本模块只
 * 用到 pk_flight_phase.h 里的相位枚举（纯 enum），不需要连带编译
 * pk_flight_phase.c / geo.c。
 *
 * 逐场景覆盖：SC1-SC8（存量场景）+ SC9-SC15（D2/D3 新增）。每个场景喂一段时间
 * 序列，断言的是**每一拍**的 advice，不是只看终态。
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

#define TICK_MS 100u

/* 实测对照值（pk_vib.c 的 accel 模长 RMS），静止门控阈值 PK_CAL_VIB_STILL_THRESH=20
 * 照这组对照定。 */
#define VIB_STILL   4u    /* 桌上静止 */
#define VIB_TOUCH  13u    /* 轻触，仍算静止 */
#define VIB_MOVE  125u    /* 拿起来转动 */
#define VIB_NA     0u     /* 不可用（窗口未满 / have_accel=false） */

static pk_cal_advice_t feed(pk_cal_advisor_t *st, uint32_t *t_ms, unsigned ticks,
                            bool imu_valid, uint8_t accuracy, pk_flight_phase_t phase,
                            uint8_t vib_level, int expect, const char *tag)
{
    pk_cal_advice_t adv = PK_CAL_ADVICE_NONE;
    for (unsigned i = 0; i < ticks; i++) {
        adv = pk_cal_advisor_update(st, *t_ms, imu_valid, accuracy, phase, vib_level);
        if (expect >= 0 && (int)adv != expect) {
            fprintf(stderr, "FAIL [%s] tick %u @t=%u: advice=%d want=%d\n",
                    tag, i, *t_ms, (int)adv, expect);
            g_fail++;
        }
        *t_ms += TICK_MS;
    }
    return adv;
}

/* ===================== SC1: 骚扰循环回归（滞回） ===================== */
static void test_sc1_single_frame_blip_does_not_rearm(void)
{
    pk_cal_advisor_t st;
    pk_cal_advisor_reset(&st);
    uint32_t t = 0;

    /* 预置 ever_converged → 走 20 s 快通道（SC1 语义：此前已收敛过）。
     * 直接戳结构体而非走 update(acc=2) 预热：后者会额外制造一次 2→0 跨阈
     * 跳变，叠加后面的 0→2→0 凑成 3 次 ≥ JAM_CROSSINGS，把 SC1 打进 jammed
     * ——而 SC1 的回归断言显式要求 is_jammed==false（不弹页的理由必须是滞回，
     * 不是被干扰识别兜住）。 */
    st.ever_converged = true;

    feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC1 累计中");
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);

    pk_cal_advisor_dismiss(&st, t);
    t += TICK_MS;
    feed(&st, &t, 10, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_HINT, "SC1 dismiss 后");

    /* 单帧抖到 acc=2：不弹页（acc>=2 清零低精度计时器）。 */
    adv = pk_cal_advisor_update(&st, t, true, 2, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_NONE);
    t += TICK_MS;

    feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC1 blip 后");
    /* 再熬 40 s：全程 HINT，不弹页。 */
    feed(&st, &t, 400, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_HINT, "SC1 闸门仍关着");
    CHECK(pk_cal_advisor_is_jammed(&st) == false);
}

/* ===================== SC2: 真校准→重新武装 ===================== */
static void test_sc2_sustained_good_accuracy_rearms_gate(void)
{
    pk_cal_advisor_t st;
    pk_cal_advisor_reset(&st);
    uint32_t t = 0;

    feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC2 冷启动宽限中");

    uint32_t high_since = t;
    feed(&st, &t, 300, true, 2, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC2 高精度保持");
    CHECK(t == high_since + PK_CAL_REARM_MS);

    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 2, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_NONE);
    t += TICK_MS;

    /* 退化 → ever_converged 已置位 → 20 s 快通道。 */
    feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC2 二次退化累计");
    adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);
    CHECK(pk_cal_advisor_is_jammed(&st) == false);

    /* 对照：acc>=2 只保持 29.9 s → REARM 没满 → 闸门仍关。与 SC2 主场景同
     * 序列，只是高精度段差一拍不到 30 s；也得先走 WIZARD→dismiss 才有关得
     * 上的闸门可测。直接戳 ever_converged 避免预热制造跨阈。 */
    pk_cal_advisor_t st_short;
    pk_cal_advisor_reset(&st_short);
    st_short.ever_converged = true;
    uint32_t t2 = 0;
    feed(&st_short, &t2, 200, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC2b 累计中");
    pk_cal_advice_t adv2 = pk_cal_advisor_update(&st_short, t2, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK_ADVICE(adv2, PK_CAL_ADVICE_WIZARD);
    pk_cal_advisor_dismiss(&st_short, t2);
    t2 += TICK_MS;
    feed(&st_short, &t2, 299, true, 2, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC2b 高精度差一拍");
    /* 立刻掉回 0：REARM 没走满，闸门仍关着 → 之后再久也只能是 HINT。 */
    feed(&st_short, &t2, 200, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC2b 二次累计");
    feed(&st_short, &t2, 100, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_HINT, "SC2b 闸门仍关着");
}

/* ===================== SC3: 飞行相位门控 ===================== */
static void test_sc3_in_motion_phases_never_take_the_page(void)
{
    static const pk_flight_phase_t IN_MOTION[] = {
        PK_PHASE_TAXI, PK_PHASE_TAKEOFF_ROLL, PK_PHASE_AIRBORNE, PK_PHASE_LANDING_ROLLOUT,
    };
    for (size_t i = 0; i < sizeof(IN_MOTION) / sizeof(IN_MOTION[0]); i++) {
        pk_cal_advisor_t st;
        pk_cal_advisor_reset(&st);
        st.ever_converged = true; /* 直接戳：避免预热跨阈污染 */
        uint32_t t = 0;
        feed(&st, &t, 200, true, 0, IN_MOTION[i], VIB_MOVE, PK_CAL_ADVICE_NONE, "SC3 累计中");
        feed(&st, &t, 600, true, 0, IN_MOTION[i], VIB_MOVE, PK_CAL_ADVICE_HINT, "SC3 飞行中只给 HINT");
        CHECK(pk_cal_advisor_is_jammed(&st) == false);
    }
}

/* ===================== SC4: UNKNOWN 放行 ===================== */
static void test_sc4_unknown_phase_allows_wizard(void)
{
    pk_cal_advisor_t st;
    pk_cal_advisor_reset(&st);
    st.ever_converged = true; /* 直接戳：避免预热跨阈污染 */
    uint32_t t = 0;
    feed(&st, &t, 200, true, 0, PK_PHASE_UNKNOWN, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC4 累计中");
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_UNKNOWN, VIB_MOVE);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);
}

/* ===================== SC5/SC6 共用：打进 jammed ===================== */
static uint32_t drive_into_jam(pk_cal_advisor_t *st, uint32_t base_ms)
{
    pk_cal_advisor_update(st, base_ms, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK(pk_cal_advisor_is_jammed(st) == false);
    pk_cal_advisor_update(st, base_ms + 1000u, true, 2, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    pk_cal_advisor_update(st, base_ms + 2000u, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK(pk_cal_advisor_is_jammed(st) == false);
    pk_cal_advisor_update(st, base_ms + 3000u, true, 2, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK(pk_cal_advisor_is_jammed(st) == true);
    pk_cal_advisor_update(st, base_ms + 4000u, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK(pk_cal_advisor_is_jammed(st) == true);
    return base_ms + 4000u;
}

/* ===================== SC5: 干扰期间闭嘴 ===================== */
static void test_sc5_jam_detection_forces_none(void)
{
    pk_cal_advisor_t st;
    pk_cal_advisor_reset(&st);
    uint32_t last_cross = drive_into_jam(&st, 0);
    uint32_t t = last_cross + TICK_MS;
    feed(&st, &t, 299, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC5 干扰期间闭嘴");
    CHECK(t == last_cross + PK_CAL_JAM_CLEAR_MS);
    CHECK(pk_cal_advisor_is_jammed(&st) == true);
}

/* ===================== SC6: 干扰解除 ===================== */
static void test_sc6_jam_clears_after_quiet_window(void)
{
    pk_cal_advisor_t st;
    pk_cal_advisor_reset(&st);
    uint32_t last_cross = drive_into_jam(&st, 0);
    uint32_t t = last_cross + TICK_MS;
    feed(&st, &t, 299, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC6 静默期");
    CHECK(pk_cal_advisor_is_jammed(&st) == true);
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK(pk_cal_advisor_is_jammed(&st) == false);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);
}

/* ===================== SC7: EXIT 语义 + D3 主动进入不退出 ===================== */
static void test_sc7_exit_after_sustained_good_accuracy(void)
{
    pk_cal_advisor_t st;
    pk_cal_advisor_reset(&st);
    uint32_t t = 0;

    /* 自动路径：acc>=2 保持满 EXIT_MS → should_exit_wizard 翻真。 */
    for (unsigned i = 0; i < 30; i++) {
        pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 2, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
        CHECK_ADVICE(adv, PK_CAL_ADVICE_NONE);
        CHECK(pk_cal_advisor_should_exit_wizard(&st) == false);
        t += TICK_MS;
    }
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 2, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_NONE);
    CHECK(pk_cal_advisor_should_exit_wizard(&st) == true);

    /* D3：用户主动进入 → 永不自动退出。 */
    pk_cal_advisor_user_open(&st, t);
    CHECK(pk_cal_advisor_should_exit_wizard(&st) == false);
    t += TICK_MS;
    feed(&st, &t, 60, true, 2, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC7 user_open 后");
    CHECK(pk_cal_advisor_should_exit_wizard(&st) == false);
    pk_cal_advisor_update(&st, t, true, 2, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK(pk_cal_advisor_should_exit_wizard(&st) == false);
}

/* ===================== SC8: uint32 回绕 ===================== */
static void test_sc8_uint32_wraparound(void)
{
    /* A. ENTER 窗口跨回绕（预置 ever_converged 走快通道） */
    pk_cal_advisor_t st;
    pk_cal_advisor_reset(&st);
    st.ever_converged = true; /* 直接戳：避免预热跨阈污染 */

    uint32_t t = 0xFFFFFFFFu - 10000u;
    uint32_t low_since = t;
    feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC8 跨回绕累计");
    CHECK(t == (uint32_t)(low_since + PK_CAL_ENTER_MS));
    CHECK(t < low_since);
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);

    /* B. 干扰窗口跨回绕 */
    pk_cal_advisor_t st2;
    pk_cal_advisor_reset(&st2);
    uint32_t last_cross = drive_into_jam(&st2, 0xFFFFFFFFu - 2000u);
    CHECK(last_cross < 0xFFFFFFFFu - 2000u);
    uint32_t t2 = last_cross + TICK_MS;
    feed(&st2, &t2, 299, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC8 跨回绕静默");
    CHECK(pk_cal_advisor_is_jammed(&st2) == true);
    adv = pk_cal_advisor_update(&st2, t2, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK(pk_cal_advisor_is_jammed(&st2) == false);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);
}

/* ===================== SC9 (D2-a): 静止只给 HINT ===================== */
static void test_sc9_still_box_only_gets_hint(void)
{
    /* vib=4：桌面静止 → 只给 HINT */
    { pk_cal_advisor_t st; pk_cal_advisor_reset(&st); uint32_t t = 0;
      st.ever_converged = true; /* 预置走 20s 快通道 */
      feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, VIB_STILL, PK_CAL_ADVICE_NONE, "SC9 静止累计中");
      feed(&st, &t, 600, true, 0, PK_PHASE_GROUND_STOPPED, VIB_STILL, PK_CAL_ADVICE_HINT, "SC9 静止只给 HINT");
    }
    /* vib=13：轻触 → 仍算静止 → 只给 HINT */
    { pk_cal_advisor_t st; pk_cal_advisor_reset(&st); uint32_t t = 0;
      st.ever_converged = true; /* 预置走 20s 快通道 */
      feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, VIB_TOUCH, PK_CAL_ADVICE_NONE, "SC9 轻触累计中");
      feed(&st, &t, 600, true, 0, PK_PHASE_GROUND_STOPPED, VIB_TOUCH, PK_CAL_ADVICE_HINT, "SC9 轻触只给 HINT");
    }
    /* vib=125：转动 → WIZARD */
    { pk_cal_advisor_t st; pk_cal_advisor_reset(&st); uint32_t t = 0;
      st.ever_converged = true; /* 预置走 20s 快通道（直接写字段避免引入跨阈跳变） */
      feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC9 运动累计中");
      pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
      CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);
    }
}

/* ===================== SC10 (D2-a): 静止→运动切换 ===================== */
static void test_sc10_still_then_moving_unleashes_wizard(void)
{
    pk_cal_advisor_t st; pk_cal_advisor_reset(&st); uint32_t t = 0;
    st.ever_converged = true; /* 预置走 20s 快通道 */
    feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, VIB_STILL, PK_CAL_ADVICE_NONE, "SC10 静止累计");
    feed(&st, &t, 100, true, 0, PK_PHASE_GROUND_STOPPED, VIB_STILL, PK_CAL_ADVICE_HINT, "SC10 静止满窗 HINT");
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);
}

/* ===================== SC11 (D2-a): vib==0 不可用不抑制 ===================== */
static void test_sc11_vib_unavailable_does_not_suppress(void)
{
    pk_cal_advisor_t st; pk_cal_advisor_reset(&st); uint32_t t = 0;
    st.ever_converged = true; /* 预置走 20s 快通道 */
    feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, VIB_NA, PK_CAL_ADVICE_NONE, "SC11 vib=0 累计中");
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED, VIB_NA);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);
}

/* ===================== SC12 (D2-b): 冷启动 120 s 宽限 ===================== */
static void test_sc12_coldstart_grace_120s(void)
{
    pk_cal_advisor_t st; pk_cal_advisor_reset(&st); uint32_t t = 0;
    /* ever_converged=false → 走 120 s 宽限。前 20 s 必 NONE。 */
    feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC12 前 20s");
    /* 20~120 s：旧窗口满了但宽限没到 → NONE。feed 到 t=120000 整。 */
    feed(&st, &t, 1000, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC12 20s~120s 宽限中");
    CHECK(t == PK_CAL_COLDSTART_GRACE_MS);
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);
    CHECK(pk_cal_advisor_is_jammed(&st) == false);
}

/* ===================== SC13 (D2-b): 收敛过→退化走 20 s 快通道 ===================== */
static void test_sc13_converged_then_degrade_uses_fast_path(void)
{
    pk_cal_advisor_t st; pk_cal_advisor_reset(&st); uint32_t t = 0;
    feed(&st, &t, 600, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC13 冷启动宽限中");
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 2, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_NONE);
    t += TICK_MS;
    feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC13 快通道累计");
    adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);
}

/* ===================== SC14 (D2-c): acc==1 现在会起算 ===================== */
static void test_sc14_acc1_now_starts_timer(void)
{
    pk_cal_advisor_t st; pk_cal_advisor_reset(&st); uint32_t t = 0;
    st.ever_converged = true; /* 预置走 20s 快通道（直接写字段避免引入跨阈跳变） */
    feed(&st, &t, 200, true, 1, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC14 acc=1 累计中");
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 1, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);
}

/* ===================== SC15 (D3): 主动→dismiss→自动仍退出 ===================== */
static void test_sc15_user_open_then_dismiss_then_auto_still_exits(void)
{
    pk_cal_advisor_t st; pk_cal_advisor_reset(&st); uint32_t t = 0;
    st.ever_converged = true; /* 预置走 20s 快通道 */

    /* 用户主动进入 → should_exit 恒 false。 */
    pk_cal_advisor_user_open(&st, t);
    CHECK(pk_cal_advisor_should_exit_wizard(&st) == false);
    t += TICK_MS;

    /* acc>=2 保持满 EXIT_MS：should_exit 仍 false（user_opened 护栏）。 */
    feed(&st, &t, 31, true, 2, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC15 主动进入不退出");
    CHECK(pk_cal_advisor_should_exit_wizard(&st) == false);

    /* 用户 dismiss → user_opened 清位，闸门关上。
     * 直接写字段重新武装闸门（绕过 REARM 等待），因为这里测的不是滞回。 */
    pk_cal_advisor_dismiss(&st, t);
    st.suppressed = false; /* 模拟闸门已重新武装 */
    t += TICK_MS;

    /* acc=0 退化满 20 s → WIZARD（闸门开着，自动弹出路径）。 */
    feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC15 退化累计");
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);

    /* should_exit_wizard 已恢复（user_opened=false）：acc>=2 满 EXIT_MS 翻真。 */
    t += TICK_MS;
    feed(&st, &t, 30, true, 2, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "SC15 自动退出恢复");
    CHECK(pk_cal_advisor_should_exit_wizard(&st) == false); /* 差一拍 */
    pk_cal_advisor_update(&st, t, true, 2, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK(pk_cal_advisor_should_exit_wizard(&st) == true);  /* 满 3 s → 该退 */
}

/* ===================== 杂项 ===================== */
static void test_reset_is_clean_slate(void)
{
    pk_cal_advisor_t st;
    memset(&st, 0xAA, sizeof(st));
    pk_cal_advisor_reset(&st);
    CHECK(pk_cal_advisor_is_jammed(&st) == false);
    CHECK(pk_cal_advisor_should_exit_wizard(&st) == false);
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, 0, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_NONE);
}

static void test_invalid_does_not_reset_timers(void)
{
    pk_cal_advisor_t st; pk_cal_advisor_reset(&st); uint32_t t = 0;
    st.ever_converged = true; /* 预置走 20s 快通道 */
    /* low_since 在第一拍（t=0）起算。中间插 invalid 不清零也不推进计时器，
     * 所以满 20 s（t=20000）就该给 WIZARD——invalid 那 5 s 只是「断流」，
     * 不是「精度变好」。 */
    feed(&st, &t, 100, true,  0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "misc acc0 前半");
    feed(&st, &t, 50,  false, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "misc invalid 不清零");
    feed(&st, &t, 50,  true,  0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "misc acc0 后半");
    CHECK(t == PK_CAL_ENTER_MS); /* 200 拍 = 20 s */
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);
    CHECK(pk_cal_advisor_is_jammed(&st) == false);
}

static void test_invalid_samples_do_not_count_as_crossings(void)
{
    pk_cal_advisor_t st; pk_cal_advisor_reset(&st);
    for (unsigned i = 0; i < 8; i++) {
        pk_cal_advisor_update(&st, i * 1000u,        true,  0, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
        pk_cal_advisor_update(&st, i * 1000u + 500u, false, 2, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    }
    CHECK(pk_cal_advisor_is_jammed(&st) == false);
}

static void test_user_open_rearms_and_clears_timers(void)
{
    pk_cal_advisor_t st; pk_cal_advisor_reset(&st); uint32_t t = 0;
    st.ever_converged = true; /* 预置走 20s 快通道（直接写字段避免引入跨阈跳变） */
    feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "open 累计中");
    pk_cal_advice_t adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);
    pk_cal_advisor_dismiss(&st, t); t += TICK_MS;
    feed(&st, &t, 10, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_HINT, "open dismiss 后");
    pk_cal_advisor_user_open(&st, t);
    feed(&st, &t, 200, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE, PK_CAL_ADVICE_NONE, "open 重新累计");
    adv = pk_cal_advisor_update(&st, t, true, 0, PK_PHASE_GROUND_STOPPED, VIB_MOVE);
    CHECK_ADVICE(adv, PK_CAL_ADVICE_WIZARD);
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
    test_sc9_still_box_only_gets_hint();
    test_sc10_still_then_moving_unleashes_wizard();
    test_sc11_vib_unavailable_does_not_suppress();
    test_sc12_coldstart_grace_120s();
    test_sc13_converged_then_degrade_uses_fast_path();
    test_sc14_acc1_now_starts_timer();
    test_sc15_user_open_then_dismiss_then_auto_still_exits();
    test_invalid_does_not_reset_timers();
    test_invalid_samples_do_not_count_as_crossings();
    test_user_open_rearms_and_clears_timers();

    if (g_fail == 0) {
        printf("PASS: all pk_cal_advisor tests passed\n");
        return 0;
    }
    fprintf(stderr, "FAIL: %d assertion(s) failed\n", g_fail);
    return 1;
}
