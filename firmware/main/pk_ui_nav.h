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

/* 设定 FAB 吸附在哪一侧。宿主开机时用它恢复 NVS 里的 fab_side。 */
void pk_ui_nav_set_fab_side(bool left);
bool pk_ui_nav_dock_open(void);

/* FAB 是否处于按下态（供模拟器核对命中区）。 */
bool pk_ui_nav_fab_pressed(void);

/* ── 由导航层回调出去的动作 ──────────────────────────────────────
 *
 * 导航层只管「点了哪个」，不管「切页要做什么」——后者在固件里是
 * pk_ui_set_mode()，在模拟器里只是打印一行。弱符号默认实现放在
 * pk_ui_nav.c，各宿主按需覆盖，这样这个文件不必知道宿主是谁。 */
void pk_ui_nav_on_tab(int tr_id);
void pk_ui_nav_on_level(void);

/* FAB 被拖到新位置后回调，供宿主持久化（固件写 NVS 的 fab_side / fab_y）。
 * y_pct 是 0~100 的相对位置——换屏或旋转后像素值会失效，比例不会。 */
void pk_ui_nav_on_fab_moved(bool left, int y_pct);
