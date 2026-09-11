/* vbus_sense.c — 见 vbus_sense.h。纯换算 + 一个最近值缓存。 */
#include "vbus_sense.h"

#ifndef PK_HOST_TEST
#include "freertos/FreeRTOS.h"
#endif

/* 理想分压倍率 ×100（整数算，避免在 P4 上为一次诊断读数引入浮点）。
 * V4 = 30k/10k → 4.00     V3 = 10k/10k → 2.00      —— PLAN.md F6 */
static const int k_ratio_x100[PK_BOARD_PROFILE_COUNT] = {
    [PK_BOARD_PROFILE_V3] = 200,
    [PK_BOARD_PROFILE_V4] = 400,
};

/*
 * 判档门限，直接定在**中点电压**上（单位 mV）。
 *
 * 为什么不拿换算后的 VBUS 去比 7000 mV：换算用的是理想倍率，系统性偏低 7%，
 * 拿它比门限等于把偏差带进判据。这里的数来自 PLAN.md F6 按 ADC Zin ≥100k
 * 算出的**实际**中点电压，门限取两档的中点：
 *
 *   V4：5V→1163 mV，9V→2093 mV      → 门限 1628 mV（余量 ±465 mV）
 *   V3：倍率 2.0，同法 5V→2326 mV… 但 9V 时中点已 4186 mV **超过 3.3V IOVDD**，
 *       ADC 会饱和。所以 V3 只能判"有没有 5V"，9V 档判不了——如实反映为
 *       高于上限即 UNKNOWN，而不是硬报一个 9V。
 */
typedef struct {
    int absent_below;   /* 低于此值判为没插 */
    int mid_5v_9v;      /* 5V/9V 分界；<=0 表示本板型判不了 9V */
    int over_above;     /* 高于此值判 UNKNOWN（接近/超过 ADC 满量程） */
} vbus_thresh_t;

static const vbus_thresh_t k_thresh[PK_BOARD_PROFILE_COUNT] = {
    /* V3：倍率 2.0。5V→2326 mV；9V 会把中点顶到 4186 mV > 3.3V，判不了。 */
    [PK_BOARD_PROFILE_V3] = { .absent_below = 600, .mid_5v_9v = 0,
                              .over_above = 3000 },
    /* V4：倍率 4.0。5V→1163 mV，9V→2093 mV，门限取中点 1628 mV。 */
    [PK_BOARD_PROFILE_V4] = { .absent_below = 400, .mid_5v_9v = 1628,
                              .over_above = 3000 },
};

static bool profile_ok(pk_board_profile_t p)
{
    return (int)p >= 0 && (int)p < PK_BOARD_PROFILE_COUNT;
}

int pk_vbus_mv_uncal(pk_board_profile_t profile, int node_mv)
{
    if (node_mv < 0 || !profile_ok(profile)) return -1;
    return node_mv * k_ratio_x100[profile] / 100;
}

pk_vbus_class_t pk_vbus_class(pk_board_profile_t profile, int node_mv)
{
    if (node_mv < 0 || !profile_ok(profile)) return PK_VBUS_UNKNOWN;
    const vbus_thresh_t *t = &k_thresh[profile];

    if (node_mv < t->absent_below) return PK_VBUS_ABSENT;
    if (node_mv > t->over_above)   return PK_VBUS_UNKNOWN;
    if (t->mid_5v_9v <= 0) {
        /* 本板型判不了 9V（中点会超 ADC 量程）——只回答"有输入"，
         * 不硬凑一个档位。 */
        return PK_VBUS_5V;
    }
    return (node_mv < t->mid_5v_9v) ? PK_VBUS_5V : PK_VBUS_9V;
}

const char *pk_vbus_class_name(pk_vbus_class_t c)
{
    switch (c) {
    case PK_VBUS_ABSENT: return "absent";
    case PK_VBUS_5V:     return "5V";
    case PK_VBUS_9V:     return "9V";
    default:             return "unknown";
    }
}

/* ── 最近值 ──
 * 1 Hz 更新、诊断页偶尔读，不值得为它上锁：单个 int 的读写在 RV32 上是
 * 原子的，且 have 标志只会从 false 变 true。volatile 防编译器缓存。 */
static volatile int  s_node_mv = -1;
static volatile bool s_have;

void pk_vbus_sense_update(int node_mv)
{
    s_node_mv = node_mv;
    s_have = true;
}

bool pk_vbus_sense_get(int *node_mv, int *vbus_mv_uncal, pk_vbus_class_t *cls)
{
    if (!s_have) return false;
    const int n = s_node_mv;
    const pk_board_profile_t p = pk_board_profile();
    if (node_mv)      *node_mv = n;
    if (vbus_mv_uncal) *vbus_mv_uncal = pk_vbus_mv_uncal(p, n);
    if (cls)          *cls = pk_vbus_class(p, n);
    return true;
}
