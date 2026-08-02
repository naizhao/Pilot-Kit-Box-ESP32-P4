/* pk_flight_phase.c — 相位状态机实现。设计说明见 pk_flight_phase.h。
 *
 * 复用 geo.c 的 geo_dist_brg（已是纯 C、host 可测，见 geo.h 顶部注释），
 * 不重新发明大圆距离公式。
 */
#include "pk_flight_phase.h"

#include "geo.h"

#include <math.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

/* ------------------------------------------------------------ 阈值常量 */

/* 60 s 位移窗口，判"动没动"的主判据（UC1/UC2/UC3，不用瞬时地速）。 */
#define PK_PHASE_WINDOW_MS        60000ULL
#define PK_PHASE_MOVE_THRESHOLD_M 20.0   /* > 此值：算移动（UC1 推出 1-3kt） */
#define PK_PHASE_STOP_THRESHOLD_M 10.0   /* < 此值：算静止（UC2 GPS 抖动） */

/* IMU 振动强度阈值：<= 此值视为"没有活动迹象"，用来把静止判定的置信
 * 窗口从 STOP_THRESHOLD_M 扩到 MOVE_THRESHOLD_M（UC2 关键：振动为零 +
 * 位移落在抖动带 → 仍判 parked）。vib_level==0 是"不可用"哨兵，不参与
 * 判定（既不确认也不否认静止）。 */
#define PK_PHASE_VIB_LOW_MAX 20u

#define PK_PHASE_VS_CLIMB_FPM    300   /* 持续爬升视为"在飞" */
#define PK_PHASE_VS_DESCEND_FPM  -300  /* 持续下降视为"在降" */
#define PK_PHASE_HELI_LEVEL_VS_FPM 150 /* 直升机悬停/触地：vs 落在此带内视为水平 */
#define PK_PHASE_HELI_GROUND_KT  10    /* 直升机地速低于此值视为已经落地 */

/* ------------------------------------------------------------ 机型阈值表 */

typedef struct {
    float taxi_min_kt, taxi_max_kt; /* 0,0 = 该机型不用固定滑行速度带（滑翔机/直升机 "—"） */
    float rotate_kt;                /* 抬轮/离地地速阈值 */
    bool  takeoff_speed_only;       /* 滑翔机：只看速度 */
    bool  takeoff_vs_only;          /* 直升机：只看垂直速度 */
} pk_ac_thresholds_t;

static const pk_ac_thresholds_t PK_AC_THRESHOLDS[PK_AC_CAT_COUNT] = {
    [PK_AC_CAT_UNKNOWN]           = { 5.0f, 20.0f, 55.0f,  false, false }, /* 兜底同轻型活塞 */
    [PK_AC_CAT_GLIDER_ULTRALIGHT] = { 0.0f,  0.0f, 30.0f,  true,  false },
    [PK_AC_CAT_HELICOPTER]        = { 0.0f,  0.0f,  0.0f,  false, true  },
    [PK_AC_CAT_PISTON_LIGHT]      = { 5.0f, 20.0f, 55.0f,  false, false },
    [PK_AC_CAT_TURBOPROP_BIZ]     = {10.0f, 25.0f,100.0f,  false, false },
    [PK_AC_CAT_JET_TRANSPORT]     = {10.0f, 30.0f,150.0f,  false, false },
};

static const pk_ac_thresholds_t *thresholds_for(pk_ac_category_t cat)
{
    if ((unsigned)cat >= PK_AC_CAT_COUNT) cat = PK_AC_CAT_UNKNOWN;
    return &PK_AC_THRESHOLDS[cat];
}

/* ------------------------------------------------------------ 60s 位移窗口 */

static void ring_push(pk_flight_phase_state_t *st, uint64_t ts_ms, int32_t lat_e7, int32_t lon_e7)
{
    st->ring[st->ring_head].ts_ms  = ts_ms;
    st->ring[st->ring_head].lat_e7 = lat_e7;
    st->ring[st->ring_head].lon_e7 = lon_e7;
    st->ring_head = (st->ring_head + 1u) % PK_FLIGHT_PHASE_RING_CAP;
    if (st->ring_count < PK_FLIGHT_PHASE_RING_CAP) st->ring_count++;
}

/* 净位移：窗口内最旧样本 → 当前点的大圆距离（米）。不是路径累计长度——
 * 净位移能天然抗 GPS 抖动来回摆动，且能抓住 1-3 kt 的持续推出。 */
static double ring_disp_m(const pk_flight_phase_state_t *st, uint64_t now_ms,
                           double cur_lat_deg, double cur_lon_deg)
{
    if (st->ring_count == 0) return 0.0;

    size_t oldest_idx = (st->ring_head + PK_FLIGHT_PHASE_RING_CAP - st->ring_count)
                         % PK_FLIGHT_PHASE_RING_CAP;
    for (size_t i = 0; i < st->ring_count; i++) {
        size_t idx = (oldest_idx + i) % PK_FLIGHT_PHASE_RING_CAP;
        uint64_t age_ms = now_ms - st->ring[idx].ts_ms; /* ts_ms 单调不减前提下恒 >=0 */
        if (age_ms <= PK_PHASE_WINDOW_MS) {
            double dist_nm = 0.0, brg = 0.0;
            geo_dist_brg(st->ring[idx].lat_e7 / 1e7, st->ring[idx].lon_e7 / 1e7,
                         cur_lat_deg, cur_lon_deg, &dist_nm, &brg);
            (void)brg;
            return dist_nm * 1852.0;
        }
    }
    return 0.0; /* 窗口内全是超龄样本，理论上刚 push 过当前点不会走到这里 */
}

/* ------------------------------------------------------------ reset/update */

void pk_flight_phase_reset(pk_flight_phase_state_t *st, pk_ac_category_t category)
{
    memset(st, 0, sizeof(*st));
    st->phase = PK_PHASE_UNKNOWN;
    st->ac_category = category;
}

pk_flight_phase_t pk_flight_phase_update(pk_flight_phase_state_t *st,
                                          const pk_flight_phase_input_t *in,
                                          pk_flight_phase_debug_t *out_debug)
{
    st->ac_category = in->ac_category;
    const pk_ac_thresholds_t *th = thresholds_for(in->ac_category);

    /* UC9：GPS 全丢——保持上一状态，不做任何转移，只把上次的位移窗口值
     * 原样吐出去（数据质量位由调用方在 own.trk flags 里另行标记）。 */
    if (!in->gps_valid) {
        if (out_debug) {
            out_debug->disp_m_60s = st->last_disp_m_60s;
            out_debug->bound_trusted = false;
        }
        return st->phase;
    }

    double cur_lat = in->lat_e7 / 1e7;
    double cur_lon = in->lon_e7 / 1e7;
    double disp_m = ring_disp_m(st, in->ts_ms, cur_lat, cur_lon);
    ring_push(st, in->ts_ms, in->lat_e7, in->lon_e7);
    st->last_disp_m_60s = disp_m;

    bool vib_available = (in->vib_level != 0);
    bool vib_says_quiet = vib_available && in->vib_level <= PK_PHASE_VIB_LOW_MAX;

    /* "动了"：位移超过移动阈值——UC1/UC3 的核心判据，不掺瞬时速度。 */
    bool is_moving = disp_m > PK_PHASE_MOVE_THRESHOLD_M;
    /* "没动"：位移低于静止阈值；或位移落在两阈值之间的抖动带，但振动
     * 数据明确说"没有活动迹象"——UC2 关键：振动为零 + 位移小 → parked，
     * 哪怕 GPS 噪声把净位移推到抖动带里。 */
    bool is_stopped = (disp_m < PK_PHASE_STOP_THRESHOLD_M) ||
                       (vib_says_quiet && disp_m <= PK_PHASE_MOVE_THRESHOLD_M);

    bool vs_climb   = in->baro_valid && in->vs_fpm > PK_PHASE_VS_CLIMB_FPM;
    bool vs_descend = in->baro_valid && in->vs_fpm < PK_PHASE_VS_DESCEND_FPM;
    bool vs_level_ish = !in->baro_valid ||
                         (in->vs_fpm >= -PK_PHASE_HELI_LEVEL_VS_FPM &&
                          in->vs_fpm <= PK_PHASE_HELI_LEVEL_VS_FPM);

    bool is_rotate_speed = in->gs_kt >= (uint16_t)th->rotate_kt;

    /* UC6：绑定机 ADS-B on_ground 与自身 GPS/速度矛盾时不信 ADS-B。
     * "明显在地面"＝位移小 + 地速低；"明显在空中"＝地速远超抬轮阈值。
     * 矛盾则整段忽略 bound，只用自主传感器（GPS/baro/vib）。 */
    bool own_clearly_ground = is_stopped && in->gs_kt < 40u;
    bool own_clearly_airborne = th->rotate_kt > 0.0f &&
                                 in->gs_kt > (uint16_t)(th->rotate_kt * 1.3f);
    bool contradiction = in->bound_valid &&
                          ((in->bound_on_ground && own_clearly_airborne) ||
                           (!in->bound_on_ground && own_clearly_ground));
    bool trust_bound = in->bound_valid && !contradiction;

    pk_flight_phase_t phase = st->phase;

    switch (phase) {
    case PK_PHASE_UNKNOWN:
        phase = is_moving ? PK_PHASE_TAXI : PK_PHASE_GROUND_STOPPED;
        break;

    case PK_PHASE_GROUND_STOPPED:
        if (th->takeoff_vs_only) {
            /* 直升机：地面态可以直接垂直起飞，跳过 takeoff_roll（没有滑跑）。 */
            if (vs_climb) phase = PK_PHASE_AIRBORNE;
            else if (is_moving) phase = PK_PHASE_TAXI;
        } else if (is_moving) {
            phase = PK_PHASE_TAXI;
        }
        break;

    case PK_PHASE_TAXI: {
        bool takeoff_now;
        if (th->takeoff_vs_only) {
            takeoff_now = vs_climb;
        } else if (th->takeoff_speed_only) {
            takeoff_now = is_rotate_speed; /* 滑翔机绞车：加速极快，单 tick 单阈值即可捕捉 */
        } else {
            takeoff_now = is_rotate_speed && (vs_climb || !in->baro_valid);
        }

        if (th->takeoff_vs_only && vs_climb) {
            phase = PK_PHASE_AIRBORNE; /* 直升机没有滑跑段 */
        } else if (takeoff_now) {
            phase = PK_PHASE_TAKEOFF_ROLL;
        } else if (is_stopped) {
            /* UC7：机场范围内排队等待，停止判据不封段——留在 taxi。 */
            if (!in->near_airport) phase = PK_PHASE_GROUND_STOPPED;
        }
        break;
    }

    case PK_PHASE_TAKEOFF_ROLL:
        if (vs_climb || (th->takeoff_speed_only && is_rotate_speed && !vs_descend)) {
            phase = PK_PHASE_AIRBORNE;
        } else if (!is_rotate_speed && !vs_climb) {
            phase = PK_PHASE_TAXI; /* 中断起飞 */
        }
        break;

    case PK_PHASE_AIRBORNE:
        if (th->takeoff_vs_only) {
            /* 直升机：垂速回到水平带 + 地速趋近于零 → 已经落地，跳过滑跑段。 */
            if (vs_level_ish && in->gs_kt < PK_PHASE_HELI_GROUND_KT) {
                phase = is_moving ? PK_PHASE_TAXI : PK_PHASE_GROUND_STOPPED;
            }
        } else if (trust_bound && in->bound_on_ground) {
            phase = PK_PHASE_LANDING_ROLLOUT; /* 最权威信号，直接采信 */
        } else if (vs_descend && !is_rotate_speed) {
            phase = PK_PHASE_LANDING_ROLLOUT;
        }
        break;

    case PK_PHASE_LANDING_ROLLOUT:
        /* UC8 触地复飞：滑跑中重新爬升/提速 → 直接弹回 takeoff_roll/
         * airborne，不经过 ground_stopped/taxi，两段航迹天然合并。 */
        if (vs_climb || is_rotate_speed) {
            phase = PK_PHASE_TAKEOFF_ROLL;
        } else if (is_stopped) {
            phase = PK_PHASE_GROUND_STOPPED;
        } else if (is_moving) {
            phase = PK_PHASE_TAXI;
        }
        break;
    }

    st->phase = phase;

    if (out_debug) {
        out_debug->disp_m_60s = disp_m;
        out_debug->bound_trusted = trust_bound;
    }
    return phase;
}

/* ------------------------------------------------------------ 单例便捷 API */

static EXT_RAM_BSS_ATTR pk_flight_phase_state_t s_default_state;

void pk_flight_phase_g_reset(pk_ac_category_t category)
{
    pk_flight_phase_reset(&s_default_state, category);
}

pk_flight_phase_t pk_flight_phase_g_update(const pk_flight_phase_input_t *in,
                                            pk_flight_phase_debug_t *out_debug)
{
    return pk_flight_phase_update(&s_default_state, in, out_debug);
}
