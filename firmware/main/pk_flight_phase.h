/*
 * pk_flight_phase.h — 本机飞行相位状态机（开机即录，只标记不做写入闸门）。
 *
 * 设计依据 docs/internal/2026-08-02-adsb-data-persistence-design-zh_CN.md
 * （v5）「相位标记」「机型分类阈值」两节。**本文件的状态枚举与转移图严格
 * 按 spec「相位标记」一节给出的六态图**：
 *
 *   0 unknown → 1 ground_stopped → 2 taxi → 3 takeoff_roll → 4 airborne
 *     → 5 landing_rollout → 2 taxi → 1 ground_stopped
 *
 * 与本模块的任务书（8 态：parked/pushback/taxi/takeoff_roll/airborne/
 * landing_rollout/taxi_in）不同——按任务书原话「若 spec 与此有出入以 spec
 * 为准」，这里以 spec 的六态为准。推出（pushback）不是独立状态：spec 的
 * 用户场景 1/3（推出、拖车推行）都是靠 60 s 位移窗口把"动了"这件事识别
 * 出来，落在 taxi 态里，不单独建态。
 *
 * 本文件**不依赖任何 IDF 头**，host 与固件共用（同 pk_rec_format.c 的
 * 翻译单元惯例，测试文件直接 #include 这份 .c）。
 *
 * 相位只是标记，判错可重算——因此内部逻辑刻意保守：
 *   - GPS 无效（UC9 廊桥/机库）：整段跳过，phase 原样返回，不做任何转移；
 *   - 60 s 位移窗口是判"动没动"的主判据，不用瞬时地速（UC1/UC2/UC3）；
 *   - 绑定机 ADS-B on_ground 位仅在与自身 GPS/速度不矛盾时采信（UC6）；
 *   - 触地复飞（UC8）在 landing_rollout 态内直接弹回 takeoff_roll/airborne，
 *     不经过 ground_stopped/taxi，天然不切段；
 *   - 机场范围内（near_airport）时不把 taxi 降级到 ground_stopped（UC7）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ 相位枚举 */

typedef enum {
    PK_PHASE_UNKNOWN         = 0,
    PK_PHASE_GROUND_STOPPED  = 1,
    PK_PHASE_TAXI            = 2,
    PK_PHASE_TAKEOFF_ROLL    = 3,
    PK_PHASE_AIRBORNE        = 4,
    PK_PHASE_LANDING_ROLLOUT = 5,
} pk_flight_phase_t;

/* ------------------------------------------------------------ 机型分类 */

/* 「机型分类阈值」表，罩哥拍板：设置页存 u8，默认 PK_AC_CAT_PISTON_LIGHT。
 * 0 保留为 unknown（own.trk ac_category 字段的约定），枚举从 1 开始。 */
typedef enum {
    PK_AC_CAT_UNKNOWN            = 0,
    PK_AC_CAT_GLIDER_ULTRALIGHT  = 1, /* 滑翔机/超轻：起飞判据只看速度 */
    PK_AC_CAT_HELICOPTER         = 2, /* 直升机：起飞/降落只看垂直速度 */
    PK_AC_CAT_PISTON_LIGHT       = 3, /* 轻型活塞（默认） */
    PK_AC_CAT_TURBOPROP_BIZ      = 4, /* 涡桨/公务机 */
    PK_AC_CAT_JET_TRANSPORT      = 5, /* 喷气运输 */
    PK_AC_CAT_COUNT               = 6,
} pk_ac_category_t;

/* ------------------------------------------------------------ 输入 */

/* 每 tick（设计上 1 Hz，但状态机本身不强制固定周期——用 ts_ms 算窗口）
 * 喂给状态机的一份传感器快照。 */
typedef struct {
    uint64_t ts_ms;

    bool     gps_valid;
    int32_t  lat_e7;
    int32_t  lon_e7;
    uint16_t gs_kt;          /* GPS 地速；gps_valid=false 时忽略 */

    bool     baro_valid;
    int16_t  vs_fpm;         /* 气压垂直速度；baro_valid=false 时忽略 */

    uint8_t  vib_level;      /* IMU 振动强度 0-255；0 = 不可用（见
                                 own.trk 字段注释，同一约定） */

    /* 绑定机（本机）的 ADS-B on_ground 位。bound_valid=false 表示未绑定
     * 或该架飞机暂无新鲜的 ADS-B 数据。 */
    bool     bound_valid;
    bool     bound_on_ground;

    /* 在机场范围内（跑道/滑行道/停机坪）——由航空数据库最近机场判定，
     * 本模块不做地理查询，由调用方喂入。缺省 false 是安全值：不会错误
     * 地压制该封的段，只是可能少享受 UC7 的"不封段"优待。 */
    bool     near_airport;

    /* 当前设置页选定的机型分类（决定滑行/抬轮/巡航阈值）。 */
    pk_ac_category_t ac_category;
} pk_flight_phase_input_t;

/* 每 tick 输出的诊断量，供调用方落进 own.trk 的 disp_m_60s 字段
 * （spec：「存证据，不只存结论」，回放端可用更好算法重新推导相位）。 */
typedef struct {
    double disp_m_60s;     /* 60 s 窗口净位移，米；GPS 无效时保持上次值 */
    bool   bound_trusted;  /* 本 tick 是否采信了绑定机 ADS-B on_ground 位 */
} pk_flight_phase_debug_t;

/* ------------------------------------------------------------ 状态 */

/* 60 s @ 1Hz 位移窗口的环形缓冲容量。给 4 s 余量，容忍采样器偶尔抖动
 * （不是严格 1Hz 也不会立刻把窗口冲垮）。 */
#define PK_FLIGHT_PHASE_RING_CAP 64u

typedef struct {
    uint64_t ts_ms;
    int32_t  lat_e7;
    int32_t  lon_e7;
} pk_flight_phase_ring_slot_t;

typedef struct {
    pk_flight_phase_t phase;
    pk_ac_category_t  ac_category;

    /* 60 s 位移窗口。 */
    pk_flight_phase_ring_slot_t ring[PK_FLIGHT_PHASE_RING_CAP];
    size_t   ring_head;   /* 下一次写入的位置 */
    size_t   ring_count;  /* 已有多少条有效样本（<= CAP） */

    double   last_disp_m_60s; /* GPS 丢失时（UC9）原样保留，供回放端参考 */
} pk_flight_phase_state_t;

/* 显式状态版本（reentrant）：调用方持有自己的 state，可以并行跑多份
 * （单测按 UC 场景各开一份，互不干扰）。 */
void pk_flight_phase_reset(pk_flight_phase_state_t *st, pk_ac_category_t category);

pk_flight_phase_t pk_flight_phase_update(pk_flight_phase_state_t *st,
                                          const pk_flight_phase_input_t *in,
                                          pk_flight_phase_debug_t *out_debug /* 可为 NULL */);

/* ------------------------------------------------------------ 单例便捷 API */

/* 固件侧真正接管写入管线时（本阶段刻意不做）会用这套单例：内部状态是
 * 「大静态数据」（60 槽环形缓冲），按内存红线一律 EXT_RAM_BSS_ATTR——
 * 该宏在 host 编译时退化为空，见 pk_flight_phase.c 顶部 #if。 */
void pk_flight_phase_g_reset(pk_ac_category_t category);

pk_flight_phase_t pk_flight_phase_g_update(const pk_flight_phase_input_t *in,
                                            pk_flight_phase_debug_t *out_debug);

#ifdef __cplusplus
}
#endif
