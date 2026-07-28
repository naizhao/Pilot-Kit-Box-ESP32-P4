/*
 * pk_ui_nav.h — 触摸导航层：FAB + dock。
 *
 * 与 PFD 绘制模块同样的定位：**平台无关**，只依赖 LVGL 与 i18n，因此固件与
 * 模拟器编译同一份源码。平台相关的部分（显示器初始化、flush 去向）分别在
 * firmware/main/lv_port.c 与 sim/lv_backend.c。
 *
 * 之所以不留在模拟器里，是因为一旦两边各写一份，改一处忘一处是必然的——
 * PFD 那批模块当初就是按这条规矩落的。
 */
#pragma once

#include <stdbool.h>

/* 在当前活动屏幕上创建 FAB 与 dock。须在 LVGL 初始化、canvas 建立之后调用。 */
void pk_ui_nav_init(void);

/* 展开 / 收起 dock。 */
void pk_ui_nav_set_dock_open(bool open);
bool pk_ui_nav_dock_open(void);

/* FAB 是否处于按下态（供模拟器核对命中区）。 */
bool pk_ui_nav_fab_pressed(void);
