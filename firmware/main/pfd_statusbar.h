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

#include "pfd_layout.h"   /* PFD_BAR_BOT —— 徽标槽位的垂直居中要用 */

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
    bool    sd_mounted;         /* TF 卡已挂载：绿色 sd_card 常亮  */
    /* 未插卡 / 挂不上时红色闪烁的相位，由调用方（渲染侧）每帧算好传入——
     * 本模块只管画，不存状态（见 pfd.c 里 alert_blink_on 的同款做法）。 */
    bool    sd_alert_blink_on;

    /* 单调时钟。动效（充电动画等）的相位由它算，而不是数渲染帧数——
     * 固件与模拟器帧率不同，数帧会让同一段动画快慢不一。 */
    uint32_t uptime_ms;
} pk_pfd_status_t;

void pk_pfd_statusbar_render(uint16_t *fb, const pk_pfd_status_t *s);

/*
 * 演示模式徽标在顶栏里占的槽位。
 *
 * 位置取**顶栏最右端**，与页面标题隔着整条屏。理由是各页顶栏的内容密度天差
 * 地别：PFD 右端是蓝牙+电量、看板右端是排序说明+RESET、交通页右端是朝向+
 * 量程，而中段（目标计数）几乎每页都在同一个固定 x 上。徽标若插在中段与右
 * 段之间，各页要让出的就不是一块空地而是一条**夹缝**，英文文案一长就同时挤
 * 到左右两边（实测看板英文版会出现 "17SORT DIST↑" 粘在一起）。放到最右端，
 * 每页只需把自己右对齐的那一摊整体左移，中段不受影响。
 *
 * 高度取 32 而不是整条 48：徽标是浮在顶栏上的一枚 chip，铺满整条会读成
 * 「顶栏本身变红了」，反而弱化了它是一个附加告警的含义。
 */
#define PK_UI_DEMO_BADGE_W   96
#define PK_UI_DEMO_BADGE_H   32
#define PK_UI_DEMO_BADGE_Y   ((PFD_BAR_BOT - PK_UI_DEMO_BADGE_H) / 2)
#define PK_UI_DEMO_BADGE_X   (PK_DISPLAY_W - PFD_BAR_MARGIN_R - PK_UI_DEMO_BADGE_W)

/*
 * 顶栏里右对齐内容的**右界**：演示模式下退到徽标左边，否则原样返回 dflt。
 *
 * 六个整屏页面各画各的顶栏，PFD 是状态位、交通页是朝向+量程、看板是排序+
 * RESET……徽标画在 LVGL 控件层、压在它们全部之上，谁不让谁就被盖住。与其在
 * 每页里各写一个 "if (demo) x -= 112"，不如让它们把自己的右界报进来、由这里
 * 统一裁——徽标宽度或位置一改，所有页面自动跟着走。
 *
 * 只有**顶栏右侧真的有东西**的页面需要调用它（当前是 PFD / 交通 / 看板）；
 * 诊断、设置、关于那几页顶栏只有左上角一个标题，够不到徽标。
 */
int pk_ui_topbar_right_limit(int dflt);
