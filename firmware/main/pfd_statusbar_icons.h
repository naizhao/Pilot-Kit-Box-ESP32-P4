/*
 * pfd_statusbar_icons.h — 状态栏图标绘制。
 *
 * 每个函数把图标画在 (x, y) 起始的 cell 内，返回**推进宽度**（含与后续
 * 文字的间隙），供状态栏按顺序排布。
 *
 * 颜色由调用方给出：图标与其数值同色，视觉上成为一体。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PK_BAR_ICON_NONE = 0,
    PK_BAR_ICON_REC,
    PK_BAR_ICON_BATT,
    PK_BAR_ICON_WARN,
    PK_BAR_ICON_TEMP,   /* 温度告警：温度计 + 感叹号，比通用三角贴切 */
    PK_BAR_ICON_SAT,
    PK_BAR_ICON_BLE,
    PK_BAR_ICON_SD,
    PK_BAR_ICON_ADSB,   /* ADS-B 目标数前缀，替代原来的 "ADSB" 四字母 */
} pk_bar_icon_t;

/* 电池图标要在七档刻度、低电告警、充电动画之间选形态，输入不止一个值，
 * 故打包传入而不是往 draw() 上再挂两个参数。 */
typedef struct {
    uint8_t  pct;
    bool     charging;
    uint32_t uptime_ms;     /* 充电动画相位；与帧率无关 */
} pk_bar_batt_t;

/* 布局需先测宽再排版，故测量与绘制分离。图标 cell 固定（字形表按目标
 * 字号预渲染），所有图标等宽。 */
int pk_bar_icon_width(pk_bar_icon_t kind);

/* 按类型分发绘制，返回推进宽度。batt 仅 PK_BAR_ICON_BATT 使用，其余可传 NULL。 */
int pk_bar_icon_draw(uint16_t *fb, int x, int y,
                     pk_bar_icon_t kind, const pk_bar_batt_t *batt,
                     uint16_t col);
