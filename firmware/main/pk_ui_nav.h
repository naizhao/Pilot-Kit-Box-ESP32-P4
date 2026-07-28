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

/* 设定 FAB 的垂直位置，与 pk_ui_nav_on_fab_moved() 回出去的 y_pct 同一单位。
 * 侧与高度是两个正交的量，分开设——只想换边时不必先把高度读出来再写回去。 */
void pk_ui_nav_set_fab_y_pct(int y_pct);
bool pk_ui_nav_dock_open(void);

/* FAB 是否处于按下态（供模拟器核对命中区）。 */
bool pk_ui_nav_fab_pressed(void);

/* 把控件标脏，供每帧全屏重画 PFD 的宿主调用。只标控件，不标整屏。 */
void pk_ui_nav_refresh(void);

/*
 * 显示 / 收起居中的瞬时提示。msg 为 NULL 或空串即收起。
 *
 * 只管「显示什么」，不管「显示多久」——过期由 ui_state 的 pk_ui_toast_get()
 * 判定（它带时间戳且线程安全，中断里也能 show）。调用方每帧同步一次即可，
 * 在这里再实现一套计时只会多一处不一致。
 */
void pk_ui_nav_toast(const char *msg, bool is_error);

/* ── 由导航层回调出去的动作 ──────────────────────────────────────
 *
 * 导航层只管「点了哪个」，不管「切页要做什么」——后者在固件里是
 * pk_ui_set_mode()，在模拟器里只是打印一行。弱符号默认实现放在
 * pk_ui_nav.c，各宿主按需覆盖，这样这个文件不必知道宿主是谁。 */
void pk_ui_nav_on_tab(int tr_id);

/* 「调平」长按满 1 s，真正执行（固件里是 pk_imu_tare_persist()）。 */
void pk_ui_nav_on_level(void);

/* 「调平」被短按——用户多半以为点一下就行。宿主应提示「需长按 1 秒」
 * （固件走 pk_ui_toast_show(PK_TR_ACT_LEVEL_HINT)）。
 *
 * 提示不由导航层自己弹：toast 的唯一来源是 ui_state，而它不参与模拟器编译。 */
void pk_ui_nav_on_level_hint(void);

/* FAB 被拖到新位置后回调，供宿主持久化（固件写 NVS 的 fab_side / fab_y）。
 * y_pct 是 0~100 的相对位置——换屏或旋转后像素值会失效，比例不会。 */
void pk_ui_nav_on_fab_moved(bool left, int y_pct);

/* ── 二级页面（spec §4.2）───────────────────────────────────────
 *
 * 进入 / 退出全屏子页。on=true 时：顶栏出现「← <parent_title>」、FAB 图标
 * 变 ←、dock 收起且不再可展开。三条退路（顶栏按钮 / FAB / 右滑）同时可用——
 * 无物理按键的设备上，任何一条失效都不能让用户困在里面。
 *
 * 层级最多两层、不做返回栈：子页只能从诊断进入，返回目标唯一确定。
 */
void pk_ui_nav_set_subpage(bool on, const char *parent_title);
bool pk_ui_nav_in_subpage(void);

/* 用户请求返回上一级时回调（三条退路共用）。 */
void pk_ui_nav_on_back(void);
