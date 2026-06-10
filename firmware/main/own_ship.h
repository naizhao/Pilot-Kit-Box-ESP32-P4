#pragma once
#include <stdint.h>
#include "aircraft_state.h"

typedef enum {
    PK_OWN_SRC_NONE = 0,
    PK_OWN_SRC_BOUND_ADSB,   /* user manually bound an ADS-B aircraft */
    PK_OWN_SRC_GPS,          /* fallback: GPS fix */
} pk_own_src_t;

/* 本机航向(HDG)来源。pk_own_heading_resolve() 输出，消费方据此决定是否做
 * 磁偏修正：ADS-B / GPS track 是真北参考，IMU 是磁北参考。 */
typedef enum {
    PK_HDG_SRC_NONE = 0,
    PK_HDG_SRC_ADSB,         /* 绑定飞机 ADS-B 地面航迹(真北) */
    PK_HDG_SRC_IMU,          /* IMU 磁航向 yaw(磁北, 机头朝向) */
    PK_HDG_SRC_GPS,          /* GPS track 兜底(真北, 地速方向) */
} pk_hdg_src_t;

/* Resolve the effective own-ship.
   Priority: manual ADS-B binding ALWAYS wins; else GPS fix; else none.
   Fills *out and (if non-NULL) *src. Returns true if a usable own-ship exists. */
bool pk_own_ship_resolve(int64_t now_us, int64_t max_age_us,
                         aircraft_t *out, pk_own_src_t *src);

/* 统一解析本机有效机头朝向(HDG)的来源 —— PFD / traffic / list 共用，
 * 取代各处重复的内联优先级。4 级优先级：
 *   1. 绑定 ADS-B(且有速度) → own->heading_deg(飞机自报航迹，最准)
 *   2. IMU 有效              → imu_yaw_deg(磁航向 = 机头朝向)
 *   3. GPS 兜底且地速≥2kt    → own->heading_deg(GPS track；静止时是噪声故设门槛)
 *   4. 都没有                → 无航向
 * own/own_valid/own_src 来自 pk_own_ship_resolve；imu_valid/imu_yaw_deg 来自
 * pk_imu_sample_get(无 IMU 传 false/0)。写 *out_deg(按来源基准：ADS-B/GPS 真北、
 * IMU 磁北) 与(可选)*out_src，返回 true 表示有有效航向。 */
bool pk_own_heading_resolve(bool own_valid, pk_own_src_t own_src,
                            const aircraft_t *own,
                            bool imu_valid, float imu_yaw_deg,
                            float *out_deg, pk_hdg_src_t *out_src);
