/*
 * mock_runtime.c — 见 mock_runtime.h。
 *
 * 提供 pfd_hsi_traffic.c 所需的四个数据源桩：IMU 采样、本机解算、气压、
 * 目标表快照。数值是合成的，但**结构与真实接口完全一致**，所以被测的
 * 渲染代码走的是与固件相同的分支。
 *
 * 2026-08-01：合成数据本身搬去了 firmware/main/demo_data.c，因为真机上的
 * 「演示模式」要用同一批目标（迎头 / 扎堆 / 降级 …）。本文件从此只剩两件事：
 *   1. 把真实 getter 的符号顶掉（PC 上没有 IMU / GPS / 融合表）；
 *   2. 叠上模拟器专有的 PK_SIM_* 缺数据开关。
 * 一句合成公式都不该再留在这里——留了就又是两份真源。
 */

#include "mock_runtime.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "aircraft_state.h"
#include "baro.h"
#include "demo_data.h"
#include "imu_task.h"
#include "own_ship.h"

static float s_yaw_deg   = 0.0f;
static int   s_own_alt   = 23000;
static int64_t s_now_us  = 0;
static bool  s_own_ok    = true;
static bool  s_traffic_ok = true;

void pk_mock_update(float yaw_deg, int own_alt_ft, int64_t now_us)
{
    s_yaw_deg = yaw_deg;
    s_own_alt = own_alt_ft;
    s_now_us  = now_us;
}

void pk_mock_set_enabled(bool own_valid, bool traffic_valid)
{
    s_own_ok     = own_valid;
    s_traffic_ok = traffic_valid;
}

/* 见 mock_runtime.h。PK_SIM_EMPTY=1 等价于把每个单项开关都打开。 */
bool pk_sim_flag(const char *key)
{
    const char *e = getenv("PK_SIM_EMPTY");
    if (e && e[0] == '1') return true;
    e = getenv(key);
    return e && e[0] == '1';
}

/* ── IMU ──────────────────────────────────────────────────────────── */
bool pk_imu_sample_get(pk_imu_sample_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    /* PK_SIM_NO_IMU=1 → 模拟 BNO085 没接/无应答（真机串口上那句
     * "BNO085 no response"）。姿态、航向、调平三条路径同时失效，是**盒子
     * 单卖、用户还没装 IMU** 时的常态，不是罕见故障。 */
    if (pk_sim_flag("PK_SIM_NO_IMU")) return false;
    if (!pk_demo_imu_sample(s_now_us, out)) return false;
    out->valid = s_own_ok;
    /* PK_SIM_HDG=<deg> 把航向钉住。动画驱动的 yaw 每帧都在变，截不出
     * 「正好 60°」那一帧，而验证「正北朝上时本机符号跟着航向转」恰恰需要
     * 一个确定的角度。 */
    {
        const char *e = getenv("PK_SIM_HDG");
        out->yaw_deg = e ? (float)atof(e) : s_yaw_deg;
    }
    return s_own_ok;
}

/* ── 本机 ─────────────────────────────────────────────────────────── */
bool pk_own_ship_resolve(int64_t now_us, int64_t max_age_us,
                         aircraft_t *out, pk_own_src_t *src)
{
    (void)now_us; (void)max_age_us;
    /* PK_SIM_NO_OWN=1 → 既没绑定 ADS-B 本机、GPS 也没定位（真机 gps fix=0）。
     * 这是**开机后到拿到首次定位之间**的必经状态，不是异常：室内、冷启动、
     * 天线遮挡都会长时间停在这里。此时雷达上任何目标的距离/方位都算不出来。 */
    if (pk_sim_flag("PK_SIM_NO_OWN")) { if (src) *src = PK_OWN_SRC_NONE; return false; }
    if (!s_own_ok) return false;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->icao24        = 0x780ABC;
        out->have_position = true;
        pk_demo_own_pos(&out->lat, &out->lon);
        out->have_altitude = true;
        out->altitude_ft   = s_own_alt;
        out->have_velocity = true;
        out->heading_deg   = (int)s_yaw_deg;
    }
    if (src) *src = PK_OWN_SRC_BOUND_ADSB;
    return true;
}

/* ── 气压 ─────────────────────────────────────────────────────────── */
bool pk_baro_get(pk_baro_state_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    /* PK_SIM_NO_BARO=1 → 模拟 BMP388 没焊/I²C 无应答（真机那句
     * "BMP388 not found"）。返回 false 且 valid=0，与 baro_task 未初始化时
     * 一致——调用方多数只看返回值，也有只看 valid 的，两边都得给对。 */
    if (pk_sim_flag("PK_SIM_NO_BARO")) return false;
    if (!pk_demo_baro(s_now_us, out)) return false;
    /* 本机高度由模拟器动画驱动（PK_SIM_* 之外还有键盘手动步进），与 demo_data
     * 自己那条曲线可能不同步，气压高度得跟着屏上那个数走。 */
    out->alt_ft = s_own_alt - 120;
    return true;
}

/* ── 目标表 ───────────────────────────────────────────────────────── */
size_t aircraft_state_snapshot(aircraft_t *out, size_t cap, int64_t now_us,
                               int64_t max_age_us)
{
    (void)max_age_us;
    if (!out || cap == 0 || !s_traffic_ok) return 0;
    /* PK_SIM_NO_TRAFFIC=1 → 空快照。空列表是必须验证的一种状态：留白屏会被
     * 当成页面没画出来，得有明确的「当前无目标」文案。 */
    if (pk_sim_flag("PK_SIM_NO_TRAFFIC")) return 0;

    /* PK_SIM_TFC_FAR=1 → 目标全部推到量程外（+40 NM，超过最大档 20 NM）。
     * 这是「有数据但屏上一架都画不出」的那一种空：顶栏计数不为 0，雷达却是
     * 空的，用户会以为渲染坏了。单独一个开关是因为它与「一架都没收到」的
     * 观感相同、成因完全不同，两者的文案不能是同一句。
     *
     * PK_SIM_TFC_BARE=1 → 目标只有位置，呼号/高度/速度全缺。真实空域里
     * 只发 DF17 位置报文、不发识别与速度的飞机相当常见（老应答机、刚上电），
     * 看板上那几列会同时退化成 ---。 */
    const bool far  = pk_sim_flag("PK_SIM_TFC_FAR");
    const bool bare = pk_sim_flag("PK_SIM_TFC_BARE");

    /* 传本机的**动画**航向与高度，而不是让 demo_data 自己算：PK_SIM_HDG 把
     * 航向钉住时，目标的方位换算必须跟着钉住的那个值走，否则屏上的航向和
     * 目标分布对不上。now_us 仍走动画时间，好让 --shot 定格可复现。 */
    return pk_demo_traffic(out, cap, now_us, /*anim_us=*/s_now_us,
                           s_yaw_deg, s_own_alt,
                           far ? 40.0f : 0.0f, bare);
}
