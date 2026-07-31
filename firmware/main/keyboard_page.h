/*
 * keyboard_page.h — 全屏**受限 ASCII 编辑器**（4×10 自绘键盘）。
 *
 * 为什么自绘而不是用 LVGL 的 keyboard
 * -----------------------------------
 * firmware/sdkconfig 里 CONFIG_LV_USE_BUTTONMATRIX / _KEYBOARD / _TEXTAREA
 * 三个开关全是 "is not set"（当年为 app 分区余量关掉的）。要用 LVGL 键盘就得
 * 把这三样一并打开，等于为一个「一辈子用一两次」的功能常驻一堆控件代码。
 * 自绘的这一套只依赖已有的 pfd_draw + pfd_aa_text，与 settings_draw.c 同源。
 *
 * 为什么只有 A-Z 0-9 - _
 * ----------------------
 *   - 航空语境下用户真正要填的是 N123AB / B-1234 / 机位号，全在这个集合里；
 *   - 全大写 ⇒ 不需要 Shift 键，一屏就是全部字符，没有第二层；
 *   - 中文**画不出来**：CJK 字形是 i18n catalog 驱动的子集（gen_pfd_aa_font.py
 *     只把 catalog 里出现过的字做进字模），任意汉字这台盒子没有字形。
 *
 * 与 settings_draw.c 同一套触摸约定：render 每帧把几何留在 s_hit[] 里，
 * 触摸回调查表。几何只有绘制那一刻才知道，分开各算一次迟早会飘。
 *
 * 模态性
 * ------
 * 打开期间 FAB 被隐藏（pk_ui_nav_set_fab_hidden），页面吃掉标题栏以下的全部
 * 触摸。出口有两个且都在屏上写着字：键盘右下角的「确定」与页首的「取消」。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "i18n_catalog.h"    /* pk_tr_id_t — 标题按翻译条目 id 传 */

/* 编辑缓冲上限（不含 NUL）。调用方给的 max_len 会被夹到这个数以内。
 * 24 不是需求，是「输入框一行装得下的上限」：M 档 15 px/字符 × 24 = 360 px，
 * 加上右侧计数器仍在 768 px 的框内。 */
#define PK_KBD_TEXT_MAX  24

/* 打开编辑器。initial 为 NULL 或空串即从空开始；max_len 是允许的字符数。 */
void pk_keyboard_page_open(pk_tr_id_t title, const char *initial, int max_len);

/* 当前是否处于编辑态。渲染与触摸的分派靠它——键盘不是独立的 pk_ui_mode_t，
 * 它是设置页之上的模态层，模式循环里不该出现一个「键盘页」。 */
bool pk_keyboard_page_active(void);

void pk_keyboard_page_render(uint16_t *fb);

/* 触摸：约定同 adsb_list / diag / settings。没有 drag —— 键盘不滚动。 */
bool pk_keyboard_page_touch(int x, int y);
void pk_keyboard_page_touch_up(void);
void pk_keyboard_page_touch_cancel(void);

/* ── 结果回调（弱符号，照 pk_ui_nav.c 的做法）──────────────────────
 *
 * 编辑器不知道自己在编什么：落 NVS、重开 BLE 广播都是宿主的事。固件侧由
 * settings_page.c 提供强符号，模拟器不提供、链接照样通过。 */
void pk_keyboard_page_on_commit(const char *text);
void pk_keyboard_page_on_cancel(void);
