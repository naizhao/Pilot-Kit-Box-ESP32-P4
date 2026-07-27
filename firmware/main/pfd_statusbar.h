/*
 * pfd_statusbar.h — top strip of the G1000-style PFD.
 *
 * Layout (spec §3): y ∈ [0, 18), full 320 wide. Renders the current
 * heading on the left and the ADS-B aircraft count on the right.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    /* ── 固定两端，永不隐藏 ── */
    bool    imu_valid;
    float   yaw_deg;            /* 左端：航向 */
    size_t  aircraft_count;     /* 右端：ADS-B 目标数 */

    /* ── 中段状态位，空间不足时按优先级降级（见 pfd_statusbar.c）── */
    bool    gps_have_fix;
    uint8_t gps_sats;
    bool    rec_active;         /* 正在写记录（TF 卡 / LittleFS）*/
    bool    ble_connected;      /* GDL90 已连上 App              */
    bool    batt_valid;
    uint8_t batt_pct;
    bool    batt_charging;      /* 外部供电中，图标改播充电动画   */
    bool    temp_warn;          /* 芯片超温告警，优先级高于电量   */
    int     temp_c;

    /* 单调时钟。动效（充电动画等）的相位由它算，而不是数渲染帧数——
     * 固件与模拟器帧率不同，数帧会让同一段动画快慢不一。 */
    uint32_t uptime_ms;
} pk_pfd_status_t;

void pk_pfd_statusbar_render(uint16_t *fb, const pk_pfd_status_t *s);
