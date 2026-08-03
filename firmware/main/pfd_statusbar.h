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
    /* 罗盘（磁力计）精度低，建议校准。数据源是 pk_ui_cal_hint_active()，判定
     * 全在 pk_cal_advisor。
     *
     * 放中段这一组而不是右段：右段（BLE/电量/SD）是"设备自身状态"常驻位，
     * 插一枚进去就等于永久占掉一个槽；而罗盘精度是**会自愈的瞬态**——转两圈
     * 或者走出干扰区它自己就没了，不该长期挤占常驻位。中段那套"装不下就按
     * 优先级丢"正好对得上它可有可无的分量。 */
    bool    cal_hint;
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
 * 顶栏「设备状态组」——中段(SAT/ADSB/REC/TEMP，按优先级降级) + 右段(BLE/
 * 电量/SD)，pk_pfd_statusbar_render 内部用的就是它，只是外面多包了一层
 * HDG 左段 + 整条背景。
 *
 * 供 PFD 之外的整屏页面（交通 / 地图 / 列表）直接调用，让四页头部的这几个
 * 图标画在**同一个 x**——本函数的左右边界都是与页面内容无关的固定公式
 * （见 pfd_statusbar.c 里 pk_ui_status_group_left_x 的注释），不随各页
 * 标题、量程文案等的宽度变化。调用方不必也不该给它传左边界。
 *
 * 不清背景：整屏页面早在自己 render 开头把全屏清过一遍，这里只管画图标
 * 与文字，叠在页面已有的底色之上。
 */
void pk_ui_topbar_status_render(uint16_t *fb, const pk_pfd_status_t *s);

/*
 * 页面自己那些右对齐部件（地图 Z10、交通量程+朝向、列表排序说明+RESET）
 * 的右界：在 pk_ui_topbar_right_limit（给 DEMO 徽标让位）的基础上，再退让
 * 出设备状态组的 worst-case 宽度——那一组现在紧贴 DEMO 徽标（或紧贴屏幕
 * 右缘）常驻，页面自己的部件必须整体让到它左边，否则会被状态组的图标压住。
 *
 * 用 worst-case 而不是当前实际宽度：SAT/ADSB 常驻、REC/TEMP 时有时无，
 * 用实际宽度算会让页面自己的部件跟着 REC/TEMP 的出现/消失左右跳动——
 * 这正是 BAR_RIGHT_W 那段注释警告过的"元素位置必须稳定"。
 */
int pk_ui_topbar_content_right_limit(int dflt);

/*
 * 汇总 pk_pfd_status_t 里"设备自身状态"那一组字段（GPS/SD/BLE/电量/温度/
 * 罗盘校准提示/时钟相位），供交通/地图/列表三页复用——这些取数逻辑与 pfd.c
 * 里 PFD 分支的写法完全一致（同一批数据源，一台设备只有一份真值），不该四页
 * 各抄一份、抄出四套容易分叉的实现。PFD 分支自己也调本函数，只在调用前填
 * imu_valid/yaw_deg/aircraft_count 那三个专属字段，因此四页口径天然一致。
 *
 * 不填 imu_valid/yaw_deg/aircraft_count/rec_active：
 *   - 前两个是 PFD 专属的左段数据，与本组无关；
 *   - aircraft_count 各页快照的时间窗/来源已经各自算好，这里再算一遍只会
 *     对不上，由调用方填。
 *   - rec_active 尚无数据源接入（同 pfd.c 的 TODO(P2-1)），本函数也留默认
 *     false，等 P2-1 落地后两处一起接。
 */
void pk_ui_topbar_status_collect(pk_pfd_status_t *st);

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
