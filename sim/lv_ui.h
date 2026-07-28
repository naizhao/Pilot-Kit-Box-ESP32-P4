/*
 * lv_ui.h — 叠在 PFD canvas 之上的 LVGL 控件层。
 *
 * 这是 spec §3.2 导航骨架的起点：FAB 是唯一常驻的交互入口，一级页面点它
 * 展开 dock，二级页面它变成返回键。
 *
 * 眼下先只做 FAB 本身，目的不止是「有个按钮」——它同时是**图层混合的实证**。
 * PFD 画在 lv_canvas 上，控件画在其上，两层要经 LVGL 的 alpha 混合合成；
 * 而本项目的像素是大端 RGB565（见 lv_backend.h），混合路径与常规主机序
 * 不同。圆角 + 半透明 + 投影恰好把这条路径全用上：只要边缘没有毛刺、
 * 底下的 PFD 透得出来，就说明 LV_COLOR_FORMAT_RGB565_SWAPPED 配对了。
 */
#pragma once

#include <stdbool.h>

/* 在当前活动屏幕上创建 FAB。canvas 必须已存在（它是背景层）。 */
void pk_sim_ui_init(void);

/* FAB 是否处于按下态 —— 供模拟器打印状态，验证点击命中区。 */
bool pk_sim_ui_fab_pressed(void);

/* M 档中文字体（26 px ≈ spec §2 的 3.0 mm）。dock 与各页面共用。 */
struct _lv_font_t;
const struct _lv_font_t *pk_sim_ui_font_zh(void);

/* 展开 / 收起 dock。截图模式下用它拿到展开态的画面。 */
void pk_sim_ui_set_dock_open(bool open);
