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


/* 编辑缓冲上限（不含 NUL）。调用方给的 max_len 会被夹到这个数以内。
 *
 * 26 是当前唯一调用者（设备名，PK_DEVNAME_MAX_LEN）的硬上限——那个数由 BLE
 * 广播包算出来，是真正的墙。版面在这里不是瓶颈：输入框内宽 768，文字从 x=32
 * 起、右侧计数器占到 713，M 档 15 px/字符一行画得下 40 多个字符。
 * 夹是**静默**的（见 pk_keyboard_page_open），所以 settings_page.c 里有一条
 * 编译期断言钉住「调用方上限 ≤ 这个数」，免得出现屏上敲得进、存下来被截断。 */
#define PK_KBD_TEXT_MAX  26

/* ── 结果回调 ──────────────────────────────────────────────────────
 *
 * 编辑器不知道自己在编什么：落 NVS、重开 BLE 广播、发起一次搜索，都是宿主
 * 的事。原先这两个是**全局弱符号**（照 pk_ui_nav.c 的做法），设置页提供强
 * 符号占住了它们——那在"全机只有一个调用者"时成立，多一个（搜索页）就散架：
 * 链接期只能有一个强符号，敲完 ZGGG 按确定会去改设备名。改成随 open 传入的
 * 函数指针，谁打开谁负责收结果。
 *
 * commit 收到的 text 是**栈上的临时缓冲**（见 pk_keyboard_page_touch_up），
 * 回调返回后即失效——要留就自己拷走。 */
typedef void (*pk_keyboard_commit_fn)(const char *text);
typedef void (*pk_keyboard_cancel_fn)(void);

/* 打开编辑器。initial 为 NULL 或空串即从空开始；max_len 是允许的字符数。
 * on_commit / on_cancel 可为 NULL（那就是"编完什么也不做"）。
 *
 * title 收的是**已经取好的串**（调用方自己 pk_i18n_text()），不是翻译条目
 * id：搜索页的标题是一条不进 catalog 的 ASCII 常量（见 search_page.h 顶部
 * 关于中文的那一段），拿不出 id 来。串必须活过整个编辑期——catalog 里的
 * 译文和 .rodata 字面量都满足，栈上的临时缓冲不满足。 */
void pk_keyboard_page_open(const char *title, const char *initial, int max_len,
                           pk_keyboard_commit_fn on_commit,
                           pk_keyboard_cancel_fn on_cancel);

/* 当前是否处于编辑态。渲染与触摸的分派靠它——键盘不是独立的 pk_ui_mode_t，
 * 它是设置页之上的模态层，模式循环里不该出现一个「键盘页」。 */
bool pk_keyboard_page_active(void);

void pk_keyboard_page_render(uint16_t *fb);

/* 触摸：约定同 adsb_list / diag / settings。没有 drag —— 键盘不滚动。 */
bool pk_keyboard_page_touch(int x, int y);
void pk_keyboard_page_touch_up(void);
void pk_keyboard_page_touch_cancel(void);
