/* test_pk_flight_phase.c — host proof for pk_flight_phase（相位状态机）。
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main -o /tmp/test_phase \
 *      firmware/test/test_pk_flight_phase.c -lm && /tmp/test_phase
 *
 *   ASan/UBSan：
 *   cc -std=c11 -Wall -Wextra -Werror -O0 -g -fsanitize=address,undefined \
 *      -I firmware/main -o /tmp/test_phase_asan \
 *      firmware/test/test_pk_flight_phase.c -lm && /tmp/test_phase_asan
 *
 *   leaks（macOS）：
 *   cc -std=c11 -Wall -Wextra -Werror -O0 -g -I firmware/main \
 *      -o /tmp/test_phase_leaks firmware/test/test_pk_flight_phase.c -lm && \
 *      leaks --atExit -- /tmp/test_phase_leaks
 *
 * 同 test_pk_pmtiles.c 惯例：把被测 .c（连同它依赖的 geo.c）直接
 * #include 进同一 TU。
 *
 * 逐场景覆盖设计文档「相位标记」一节的用户场景表 UC1-UC9：每个场景喂一段
 * 时间序列，断言相位在关键 tick 上的迁移（不是只看终态，中间态错了同样
 * 是 bug——比如 UC1 若中途被误判成别的相位，说明位移窗口判据被瞬时速度
 * 污染了）。
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../main/geo.c"
#include "../main/pk_flight_phase.c"

static int g_fail = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_fail++; \
    } \
} while (0)

#define CHECK_PHASE(got, want) do { \
    if ((got) != (want)) { \
        fprintf(stderr, "FAIL %s:%d: phase=%d want=%d\n", __FILE__, __LINE__, (int)(got), (int)(want)); \
        g_fail++; \
    } \
} while (0)

/* ---------------------------------------------------- 地理位移 helper */

#define BASE_LAT_E7 225000000  /* 22.5 度 */
#define BASE_LON_E7 1140000000 /* 114.0 度 */
#define M_PER_LAT_DEG 111320.0

/* 沿纬线（南北方向）挪动 meters 米对应的 lat_e7 偏移——1 度纬度恒
 * ~111.32 km，不受经度影响，比沿经线挪动更好算。 */
static int32_t lat_off_e7(double meters)
{
    return (int32_t)llround(meters * (1e7 / M_PER_LAT_DEG));
}

static pk_flight_phase_input_t mk_input(uint64_t ts_ms, double disp_m_from_base,
                                         uint16_t gs_kt, bool baro_valid, int16_t vs_fpm,
                                         uint8_t vib_level, pk_ac_category_t cat)
{
    pk_flight_phase_input_t in;
    memset(&in, 0, sizeof(in));
    in.ts_ms = ts_ms;
    in.gps_valid = true;
    in.lat_e7 = BASE_LAT_E7 + lat_off_e7(disp_m_from_base);
    in.lon_e7 = BASE_LON_E7;
    in.gs_kt = gs_kt;
    in.baro_valid = baro_valid;
    in.vs_fpm = vs_fpm;
    in.vib_level = vib_level;
    in.bound_valid = false;
    in.bound_on_ground = false;
    in.near_airport = false;
    in.ac_category = cat;
    return in;
}

/* ================================================================ UC1 */
/* 正常推出（1-3 kt，倒退）：只看瞬时速度会判成静止，整段丢；60 s 窗口
 * 位移 >20 m 即算移动——本测试故意把 gs_kt 全程钉在 2（典型推出速度），
 * 只靠位置漂移触发 TAXI。 */
static void test_uc1_pushback(void)
{
    pk_flight_phase_state_t st;
    pk_flight_phase_reset(&st, PK_AC_CAT_PISTON_LIGHT);

    /* t=0：冷启动第一条样本，位移窗口为空 → disp=0 → 判 GROUND_STOPPED。 */
    pk_flight_phase_input_t in0 = mk_input(0, 0.0, 2, false, 0, 0, PK_AC_CAT_PISTON_LIGHT);
    pk_flight_phase_t p = pk_flight_phase_update(&st, &in0, NULL);
    CHECK_PHASE(p, PK_PHASE_GROUND_STOPPED);

    /* 0.5 m/s 持续倒退，gs_kt 全程钉在 2 kt（不触发任何"速度阈值"分支，
     * 因为地面阶段判定压根不看 gs_kt）。t=39s 净位移 19.5 m，仍 < 20 m。 */
    for (int t = 1; t <= 39; t++) {
        pk_flight_phase_input_t in = mk_input((uint64_t)t * 1000, 0.5 * t, 2, false, 0, 0,
                                               PK_AC_CAT_PISTON_LIGHT);
        p = pk_flight_phase_update(&st, &in, NULL);
        CHECK_PHASE(p, PK_PHASE_GROUND_STOPPED);
    }

    /* t=41s：净位移 20.5 m > 20 m 阈值 → 必须已经识别成"推出中"（TAXI），
     * 尽管全程瞬时速度只有 2 kt。 */
    pk_flight_phase_input_t in41 = mk_input(41000, 0.5 * 41, 2, false, 0, 0,
                                             PK_AC_CAT_PISTON_LIGHT);
    p = pk_flight_phase_update(&st, &in41, NULL);
    CHECK_PHASE(p, PK_PHASE_TAXI);
}

/* ================================================================ UC2 */
/* 停机位静止、GPS 抖动 1-2 kt：振动为零 + 60 s 位移 <10 m → 保持
 * parked，不产生假轨迹。本测试把净位移故意钉在抖动带（10-20 m 之间的
 * 15 m，比纯位移阈值更难的情形），靠 vib_level 低位确认"没有活动"。 */
static void test_uc2_gps_jitter_at_parked(void)
{
    pk_flight_phase_state_t st;
    pk_flight_phase_reset(&st, PK_AC_CAT_PISTON_LIGHT);

    pk_flight_phase_input_t in0 = mk_input(0, 0.0, 1, false, 0, 5, PK_AC_CAT_PISTON_LIGHT);
    pk_flight_phase_t p = pk_flight_phase_update(&st, &in0, NULL);
    CHECK_PHASE(p, PK_PHASE_GROUND_STOPPED);

    /* 之后 90 s，GPS 噪声把位置钉在偏离起点 15 m 处不动，vib_level=5
     * （<=20，"没有活动迹象"）——必须全程保持 GROUND_STOPPED。
     * 前 60 s，位移窗口的参照点还是 t=0 那次跳变（0 → 15 m），净位移
     * 落在纯位移阈值本身判不出的抖动带（10-20 m 之间），必须靠 vib
     * 才能确认 parked；60 s 之后窗口完全滑过跳变点，参照点与当前点
     * 都停在 15 m 处，净位移收敛到 0——同一个"没动"结论，两段窗口给
     * 出的证据形态不同，都要断言到。 */
    for (int t = 1; t <= 90; t++) {
        pk_flight_phase_input_t in = mk_input((uint64_t)t * 1000, 15.0, 1, false, 0, 5,
                                               PK_AC_CAT_PISTON_LIGHT);
        pk_flight_phase_debug_t dbg;
        p = pk_flight_phase_update(&st, &in, &dbg);
        CHECK_PHASE(p, PK_PHASE_GROUND_STOPPED);
        if (t <= 60) {
            CHECK(dbg.disp_m_60s >= 10.0 && dbg.disp_m_60s <= 20.0); /* 抖动带 */
        } else {
            CHECK(dbg.disp_m_60s < 10.0); /* 窗口已滑过跳变点，位移收敛到 0 */
        }
    }
}

/* ================================================================ UC3 */
/* 拖车推行、发动机未开：同 UC1 的位移判据，但显式验证"不依赖振动"——
 * vib_level=0（不可用/未就绪），仍然只靠位移识别出"动了"。 */
static void test_uc3_tow_engine_off(void)
{
    pk_flight_phase_state_t st;
    pk_flight_phase_reset(&st, PK_AC_CAT_PISTON_LIGHT);

    pk_flight_phase_input_t in0 = mk_input(0, 0.0, 0, false, 0, 0, PK_AC_CAT_PISTON_LIGHT);
    pk_flight_phase_t p = pk_flight_phase_update(&st, &in0, NULL);
    CHECK_PHASE(p, PK_PHASE_GROUND_STOPPED);

    /* 1 m/s 拖行，vib_level 全程 0（发动机未开、IMU 振动量不可用）。 */
    for (int t = 1; t <= 25; t++) {
        pk_flight_phase_input_t in = mk_input((uint64_t)t * 1000, 1.0 * t, 0, false, 0, 0,
                                               PK_AC_CAT_PISTON_LIGHT);
        p = pk_flight_phase_update(&st, &in, NULL);
    }
    /* t=25s 净位移 25 m > 20 m，必须已经是 TAXI——没有 vib 数据也不妨碍。 */
    CHECK_PHASE(p, PK_PHASE_TAXI);

    /* 拖到位后停下（近似同一地点），且不在机场范围内 → 应该能正常回到
     * GROUND_STOPPED（证明"不依赖振动"是双向的：进/出 taxi 都不需要它）。 */
    for (int t = 26; t <= 95; t++) {
        pk_flight_phase_input_t in = mk_input((uint64_t)t * 1000, 25.0, 0, false, 0, 0,
                                               PK_AC_CAT_PISTON_LIGHT);
        p = pk_flight_phase_update(&st, &in, NULL);
    }
    CHECK_PHASE(p, PK_PHASE_GROUND_STOPPED);
}

/* ================================================================ UC4 */
/* 直升机起飞：地速近零就升空，纯地速阈值永远判不出；靠气压垂直速度 +
 * GPS 高度爬升（本模块只拿 vs_fpm，不单独存高度，用 vs 即可）。 */
static void test_uc4_helicopter_vertical_takeoff(void)
{
    pk_flight_phase_state_t st;
    pk_flight_phase_reset(&st, PK_AC_CAT_HELICOPTER);

    pk_flight_phase_input_t in0 = mk_input(0, 0.0, 0, true, 0, 0, PK_AC_CAT_HELICOPTER);
    pk_flight_phase_t p = pk_flight_phase_update(&st, &in0, NULL);
    CHECK_PHASE(p, PK_PHASE_GROUND_STOPPED);

    /* 垂直爬升，地速全程钉在 5 kt（"近零"）——纯速度判据在这架机型上
     * 永远不会触发，必须靠 vs_fpm 直接从 GROUND_STOPPED 跳到 AIRBORNE，
     * 中间不经过 taxi/takeoff_roll（直升机没有滑跑段）。 */
    pk_flight_phase_input_t in1 = mk_input(1000, 0.0, 5, true, 800, 0, PK_AC_CAT_HELICOPTER);
    p = pk_flight_phase_update(&st, &in1, NULL);
    CHECK_PHASE(p, PK_PHASE_AIRBORNE);

    /* 巡航下降途中，地速仍然很低（直升机可以近乎垂直下降），vs 明显
     * 负值——不能因为地速低就误判落地。 */
    for (int t = 2; t <= 10; t++) {
        pk_flight_phase_input_t in = mk_input((uint64_t)t * 1000, 0.0, 5, true, -500, 0,
                                               PK_AC_CAT_HELICOPTER);
        p = pk_flight_phase_update(&st, &in, NULL);
        CHECK_PHASE(p, PK_PHASE_AIRBORNE);
    }

    /* 最终悬停触地：vs 回到水平带、地速趋零 → 直接落地，跳过滑跑段
     * （直升机没有 landing_rollout）。 */
    pk_flight_phase_input_t in_land = mk_input(11000, 0.0, 2, true, 0, 0, PK_AC_CAT_HELICOPTER);
    p = pk_flight_phase_update(&st, &in_land, NULL);
    CHECK_PHASE(p, PK_PHASE_GROUND_STOPPED);
}

/* ================================================================ UC5 */
/* 滑翔机绞车/拖曳：加速极快，"地速阈值 + 持续 3 s"的旧算法会被踩空——
 * 本状态机单 tick 越过阈值即触发，不要求持续时长，且用滑翔机专属的
 * 30 kt 抬轮阈值（不是活塞机的 55 kt）。 */
static void test_uc5_glider_winch_launch(void)
{
    pk_flight_phase_state_t st;
    pk_flight_phase_reset(&st, PK_AC_CAT_GLIDER_ULTRALIGHT);

    pk_flight_phase_input_t in0 = mk_input(0, 0.0, 0, false, 0, 0, PK_AC_CAT_GLIDER_ULTRALIGHT);
    pk_flight_phase_t p = pk_flight_phase_update(&st, &in0, NULL);
    CHECK_PHASE(p, PK_PHASE_GROUND_STOPPED);

    pk_flight_phase_input_t in1 = mk_input(1000, 5.0, 10, false, 0, 0, PK_AC_CAT_GLIDER_ULTRALIGHT);
    p = pk_flight_phase_update(&st, &in1, NULL);
    CHECK_PHASE(p, PK_PHASE_GROUND_STOPPED); /* 净位移仅 5 m，还没到 20 m */

    pk_flight_phase_input_t in2 = mk_input(2000, 25.0, 25, false, 0, 0, PK_AC_CAT_GLIDER_ULTRALIGHT);
    p = pk_flight_phase_update(&st, &in2, NULL);
    CHECK_PHASE(p, PK_PHASE_TAXI); /* 净位移 25 m > 20 m，判"动了" */

    pk_flight_phase_input_t in3 = mk_input(3000, 55.0, 35, false, 0, 0, PK_AC_CAT_GLIDER_ULTRALIGHT);
    p = pk_flight_phase_update(&st, &in3, NULL);
    CHECK_PHASE(p, PK_PHASE_TAKEOFF_ROLL); /* 35 kt >= 滑翔机 30 kt 抬轮阈值，单 tick 即触发 */

    pk_flight_phase_input_t in4 = mk_input(4000, 90.0, 40, false, 0, 0, PK_AC_CAT_GLIDER_ULTRALIGHT);
    p = pk_flight_phase_update(&st, &in4, NULL);
    CHECK_PHASE(p, PK_PHASE_AIRBORNE); /* 滑翔机只看速度，不要求 vs 数据 */
}

/* ================================================================ UC6 */
/* 绑错了别的飞机：ADS-B 说 300 kt 空中、GPS 说停着——矛盾时不信 ADS-B，
 * 退回自主传感器（不能被错误地拽去 airborne）。同时验证"不矛盾时才
 * 采信"的正向路径：AIRBORNE 阶段绑定机 on_ground 位与自身传感器一致时，
 * 直接触发降落（最权威信号）。 */
static void test_uc6_bound_mismatch(void)
{
    pk_flight_phase_state_t st;
    pk_flight_phase_reset(&st, PK_AC_CAT_PISTON_LIGHT);

    pk_flight_phase_input_t in0 = mk_input(0, 0.0, 0, false, 0, 0, PK_AC_CAT_PISTON_LIGHT);
    pk_flight_phase_t p = pk_flight_phase_update(&st, &in0, NULL);
    CHECK_PHASE(p, PK_PHASE_GROUND_STOPPED);

    /* 绑定机（其实是别的飞机）报告 300 kt 空中，但自身 GPS 明确说
     * "停着"（位移 0、地速 0）——矛盾，必须不被拽去 airborne。 */
    for (int t = 1; t <= 5; t++) {
        pk_flight_phase_input_t in = mk_input((uint64_t)t * 1000, 0.0, 0, false, 0, 0,
                                               PK_AC_CAT_PISTON_LIGHT);
        in.bound_valid = true;
        in.bound_on_ground = false; /* ADS-B 说空中 */
        pk_flight_phase_debug_t dbg;
        p = pk_flight_phase_update(&st, &in, &dbg);
        CHECK_PHASE(p, PK_PHASE_GROUND_STOPPED); /* 没被拽走 */
        CHECK(dbg.bound_trusted == false);        /* 且明确标记"不采信" */
    }

    /* 不矛盾的情况（绑定机也说在地面，与 GPS 一致）——应当被采信。 */
    pk_flight_phase_input_t in_ok = mk_input(6000, 0.0, 0, false, 0, 0, PK_AC_CAT_PISTON_LIGHT);
    in_ok.bound_valid = true;
    in_ok.bound_on_ground = true;
    pk_flight_phase_debug_t dbg_ok;
    p = pk_flight_phase_update(&st, &in_ok, &dbg_ok);
    CHECK(dbg_ok.bound_trusted == true);
}

static void test_uc6_bound_authoritative_landing(void)
{
    pk_flight_phase_state_t st;
    pk_flight_phase_reset(&st, PK_AC_CAT_PISTON_LIGHT);

    pk_flight_phase_input_t in0 = mk_input(0, 0.0, 0, false, 0, 0, PK_AC_CAT_PISTON_LIGHT);
    pk_flight_phase_update(&st, &in0, NULL);

    /* 起飞到空中：先 taxi 到抬轮速度（vs 仍平，尚未拉起），再拉起爬升
     * （vs 越过 300 fpm 才算真正起飞判据成立——本状态机的活塞机起飞
     * 判据是"速度 + 垂速"，不是速度单独）。 */
    pk_flight_phase_input_t in1 = mk_input(1000, 30.0, 20, true, 50, 0, PK_AC_CAT_PISTON_LIGHT);
    pk_flight_phase_t p = pk_flight_phase_update(&st, &in1, NULL);
    CHECK_PHASE(p, PK_PHASE_TAXI);

    pk_flight_phase_input_t in2 = mk_input(2000, 80.0, 60, true, 100, 0, PK_AC_CAT_PISTON_LIGHT);
    p = pk_flight_phase_update(&st, &in2, NULL);
    CHECK_PHASE(p, PK_PHASE_TAXI); /* 已达抬轮速度，但 vs 还没爬升，尚未拉起 */

    pk_flight_phase_input_t in3 = mk_input(3000, 150.0, 65, true, 500, 0, PK_AC_CAT_PISTON_LIGHT);
    p = pk_flight_phase_update(&st, &in3, NULL);
    CHECK_PHASE(p, PK_PHASE_TAKEOFF_ROLL); /* 速度 + 垂速同时满足 → 拉起 */

    pk_flight_phase_input_t in4 = mk_input(4000, 230.0, 68, true, 600, 0, PK_AC_CAT_PISTON_LIGHT);
    p = pk_flight_phase_update(&st, &in4, NULL);
    CHECK_PHASE(p, PK_PHASE_AIRBORNE);

    /* 巡航中，绑定机 ADS-B 突然报告 on_ground=true，且与自身传感器不
     * 矛盾（地速 65 kt 远没到"明显空中"的 1.3x 抬轮阈值 71.5 kt）——
     * 最权威信号，直接判降落，不等 vs 掉头向下。 */
    pk_flight_phase_input_t in5 = mk_input(5000, 260.0, 65, true, 100, 0, PK_AC_CAT_PISTON_LIGHT);
    in5.bound_valid = true;
    in5.bound_on_ground = true;
    pk_flight_phase_debug_t dbg;
    p = pk_flight_phase_update(&st, &in5, &dbg);
    CHECK_PHASE(p, PK_PHASE_LANDING_ROLLOUT);
    CHECK(dbg.bound_trusted == true);
}

/* ================================================================ UC7 */
/* 跑道口排队 10 分钟：停止判据取长，且"在机场范围内"时不封段——不能
 * 因为等待就把这段判成结束。 */
static void test_uc7_runway_queue(void)
{
    pk_flight_phase_state_t st;
    pk_flight_phase_reset(&st, PK_AC_CAT_PISTON_LIGHT);

    pk_flight_phase_input_t in0 = mk_input(0, 0.0, 0, false, 0, 0, PK_AC_CAT_PISTON_LIGHT);
    pk_flight_phase_update(&st, &in0, NULL);

    /* 先滑到 TAXI。 */
    pk_flight_phase_input_t in1 = mk_input(1000, 30.0, 8, false, 0, 0, PK_AC_CAT_PISTON_LIGHT);
    pk_flight_phase_t p = pk_flight_phase_update(&st, &in1, NULL);
    CHECK_PHASE(p, PK_PHASE_TAXI);

    /* 跑道口排队 10 分钟（600 s），位置基本不动，near_airport=true——
     * 全程必须留在 TAXI，不能降级到 GROUND_STOPPED。 */
    for (int t = 2; t <= 602; t++) {
        pk_flight_phase_input_t in = mk_input((uint64_t)t * 1000, 30.0, 0, false, 0, 0,
                                               PK_AC_CAT_PISTON_LIGHT);
        in.near_airport = true;
        p = pk_flight_phase_update(&st, &in, NULL);
        CHECK_PHASE(p, PK_PHASE_TAXI);
    }

    /* 对照组：同样静止，但 near_airport=false——必须能正常降级，证明
     * 上面的"不封段"确实是 near_airport 生效，不是逻辑失效。 */
    pk_flight_phase_input_t in_no_apt = mk_input(603000, 30.0, 0, false, 0, 0,
                                                  PK_AC_CAT_PISTON_LIGHT);
    in_no_apt.near_airport = false;
    p = pk_flight_phase_update(&st, &in_no_apt, NULL);
    CHECK_PHASE(p, PK_PHASE_GROUND_STOPPED);
}

/* ================================================================ UC8 */
/* 触地复飞：不能切成两段飞行——落地滑跑中重新拉起，必须直接弹回
 * takeoff_roll/airborne，不经过 ground_stopped/taxi。 */
static void test_uc8_touch_and_go(void)
{
    pk_flight_phase_state_t st;
    pk_flight_phase_reset(&st, PK_AC_CAT_PISTON_LIGHT);

    pk_flight_phase_input_t in0 = mk_input(0, 0.0, 0, false, 0, 0, PK_AC_CAT_PISTON_LIGHT);
    pk_flight_phase_update(&st, &in0, NULL);

    pk_flight_phase_input_t in1 = mk_input(1000, 30.0, 20, true, 50, 0, PK_AC_CAT_PISTON_LIGHT);
    pk_flight_phase_t p = pk_flight_phase_update(&st, &in1, NULL);
    CHECK_PHASE(p, PK_PHASE_TAXI);

    pk_flight_phase_input_t in2 = mk_input(2000, 80.0, 60, true, 100, 0, PK_AC_CAT_PISTON_LIGHT);
    p = pk_flight_phase_update(&st, &in2, NULL);
    CHECK_PHASE(p, PK_PHASE_TAXI); /* 抬轮速度已到，vs 还平，仍在跑道滑跑 */

    pk_flight_phase_input_t in3 = mk_input(3000, 150.0, 65, true, 500, 0, PK_AC_CAT_PISTON_LIGHT);
    p = pk_flight_phase_update(&st, &in3, NULL);
    CHECK_PHASE(p, PK_PHASE_TAKEOFF_ROLL);

    pk_flight_phase_input_t in4 = mk_input(4000, 230.0, 68, true, 600, 0, PK_AC_CAT_PISTON_LIGHT);
    p = pk_flight_phase_update(&st, &in4, NULL);
    CHECK_PHASE(p, PK_PHASE_AIRBORNE);

    /* 下降进近：vs 明显下降 + 地速低于抬轮阈值 → 判进跑道滑跑。 */
    pk_flight_phase_input_t in5 = mk_input(5000, 300.0, 40, true, -600, 0, PK_AC_CAT_PISTON_LIGHT);
    p = pk_flight_phase_update(&st, &in5, NULL);
    CHECK_PHASE(p, PK_PHASE_LANDING_ROLLOUT);

    /* 触地后没减到底就重新拉起：vs 转正、速度重新爬升——必须直接弹回
     * takeoff_roll，绝不能先经过 ground_stopped 或 taxi（那样就是把
     * 一次起降切成了两段独立飞行）。 */
    pk_flight_phase_input_t in6 = mk_input(6000, 330.0, 55, true, 400, 0, PK_AC_CAT_PISTON_LIGHT);
    p = pk_flight_phase_update(&st, &in6, NULL);
    CHECK_PHASE(p, PK_PHASE_TAKEOFF_ROLL);
    CHECK(p != PK_PHASE_GROUND_STOPPED);
    CHECK(p != PK_PHASE_TAXI);

    pk_flight_phase_input_t in7 = mk_input(7000, 400.0, 65, true, 600, 0, PK_AC_CAT_PISTON_LIGHT);
    p = pk_flight_phase_update(&st, &in7, NULL);
    CHECK_PHASE(p, PK_PHASE_AIRBORNE);
}

/* ================================================================ UC9 */
/* 廊桥/机库 GPS 全丢：状态跳 unknown 是错的——必须保持上一状态，且把
 * 数据质量的判断留给调用方（flags 位），本模块只负责"冻结"。 */
static void test_uc9_gps_loss_freeze(void)
{
    pk_flight_phase_state_t st;
    pk_flight_phase_reset(&st, PK_AC_CAT_PISTON_LIGHT);

    pk_flight_phase_input_t in0 = mk_input(0, 0.0, 0, false, 0, 0, PK_AC_CAT_PISTON_LIGHT);
    pk_flight_phase_update(&st, &in0, NULL);

    pk_flight_phase_input_t in1 = mk_input(1000, 30.0, 8, false, 0, 0, PK_AC_CAT_PISTON_LIGHT);
    pk_flight_phase_t p = pk_flight_phase_update(&st, &in1, NULL);
    CHECK_PHASE(p, PK_PHASE_TAXI);

    pk_flight_phase_debug_t dbg_before;
    pk_flight_phase_input_t in_probe = mk_input(1999, 30.0, 8, false, 0, 0, PK_AC_CAT_PISTON_LIGHT);
    /* 不实际推进状态，只是拿当前 disp 值做参照：直接读 st.last_disp_m_60s。 */
    (void)in_probe;
    dbg_before.disp_m_60s = st.last_disp_m_60s;

    /* GPS 丢失 120 s（典型廊桥/机库遮挡时长）：phase 必须原地不动，
     * 且返回的 disp_m_60s 保持上次已知值（不能悄悄归零，那会被回放端
     * 误读成"回到了起点"）。 */
    for (int t = 2; t <= 121; t++) {
        pk_flight_phase_input_t in;
        memset(&in, 0, sizeof(in));
        in.ts_ms = (uint64_t)t * 1000;
        in.gps_valid = false;
        in.ac_category = PK_AC_CAT_PISTON_LIGHT;
        pk_flight_phase_debug_t dbg;
        p = pk_flight_phase_update(&st, &in, &dbg);
        CHECK_PHASE(p, PK_PHASE_TAXI);
        CHECK(dbg.disp_m_60s == dbg_before.disp_m_60s);
        CHECK(dbg.bound_trusted == false);
    }

    /* GPS 恢复后应能继续正常演化（这里验证的是"没有被 GPS 丢失期间的
     * 冻结逻辑破坏"，用一个明确的静止场景验证降级正常发生）。 */
    for (int t = 122; t <= 200; t++) {
        pk_flight_phase_input_t in = mk_input((uint64_t)t * 1000, 30.0, 0, false, 0, 0,
                                               PK_AC_CAT_PISTON_LIGHT);
        p = pk_flight_phase_update(&st, &in, NULL);
    }
    CHECK_PHASE(p, PK_PHASE_GROUND_STOPPED);
}

/* ================================================================ 杂项 */

static void test_initial_state_is_unknown(void)
{
    pk_flight_phase_state_t st;
    pk_flight_phase_reset(&st, PK_AC_CAT_PISTON_LIGHT);
    CHECK(st.phase == PK_PHASE_UNKNOWN);
}

static void test_singleton_api_wraps_reentrant(void)
{
    pk_flight_phase_g_reset(PK_AC_CAT_PISTON_LIGHT);
    pk_flight_phase_input_t in0 = mk_input(0, 0.0, 0, false, 0, 0, PK_AC_CAT_PISTON_LIGHT);
    pk_flight_phase_t p = pk_flight_phase_g_update(&in0, NULL);
    CHECK_PHASE(p, PK_PHASE_GROUND_STOPPED);
}

int main(void)
{
    test_initial_state_is_unknown();
    test_uc1_pushback();
    test_uc2_gps_jitter_at_parked();
    test_uc3_tow_engine_off();
    test_uc4_helicopter_vertical_takeoff();
    test_uc5_glider_winch_launch();
    test_uc6_bound_mismatch();
    test_uc6_bound_authoritative_landing();
    test_uc7_runway_queue();
    test_uc8_touch_and_go();
    test_uc9_gps_loss_freeze();
    test_singleton_api_wraps_reentrant();

    if (g_fail == 0) {
        printf("PASS: all pk_flight_phase tests passed\n");
        return 0;
    }
    fprintf(stderr, "FAIL: %d assertion(s) failed\n", g_fail);
    return 1;
}
