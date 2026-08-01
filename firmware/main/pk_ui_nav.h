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

#include "pfd_layout.h"

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

/*
 * 整个藏掉 / 放出 FAB。
 *
 * 给**模态**页面用（当前只有键盘编辑器）。与 set_subpage 不同：那个只是把
 * 图标换成「←」，FAB 本身还在，仍然压在页面之上；模态编辑器铺满全屏且自绘
 * 命中区排在 LVGL 之前（见 touch_gt911.c），FAB 留着的结果是它自己点不动、
 * 又盖住底下的键。藏起来的同时收掉 dock —— dock 锚在 FAB 上，FAB 没了它就
 * 会浮在半空。
 */
void pk_ui_nav_set_fab_hidden(bool hidden);

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
/* backbar 的几何，供二级页排版避让——它是 LVGL 控件、画在 framebuffer 之上，
 * 子页把内容画到这块区域里就会被盖住。导出而不是让各页各抄一份数字：抄的那
 * 份不会跟着这里变（已经错过一次，把 44 抄成了 36）。 */
/*
 * backbar 占**第一行**，就落在其他页画标题栏的那条带（0..PFD_BAR_BOT=48）里。
 *
 * 之前它排在 PFD_BAR_BOT + 6，等于把最顶上那 48 px 让给了子页自己画的子系统
 * 名，于是屏幕从上到下读出来是「IMU / ← DIAGNOSTICS / 内容」——先看到"我在
 * 哪"再看到"怎么回去"，层级是倒的。返回入口必须是视线落点的第一个东西。
 *
 * 44 高塞进 48 的带里，上下各留 2 px，正好与其他页的标题栏同高同位，
 * 换页时顶部这条带不会跳。
 */
#define PK_UI_BACKBAR_TOP   2
#define PK_UI_BACKBAR_H     44
#define PK_UI_BACKBAR_BOT   (PK_UI_BACKBAR_TOP + PK_UI_BACKBAR_H)

void pk_ui_nav_set_subpage(bool on, const char *parent_title);

/* 用户请求返回上一级时回调（三条退路共用）。 */
void pk_ui_nav_on_back(void);

/* ── 演示模式常驻标识（安全件，见 config_demo.h）──────────────────
 *
 * 开着就必须看得见，而且用户没有任何办法「只要假数据、不要标识」——所以它不
 * 是一个可选的装饰，而是与演示模式同生共死的一块 UI。
 *
 * 做在导航层而不是各页面里，是因为这里是**唯一**压在所有页面之上的图层：
 * PFD / 交通 / 看板 / 诊断 / 设置 / 关于 / 键盘 / 调平向导 各自往 framebuffer
 * 里画，谁都不知道别人的存在，逐页去加就必然漏（漏掉的那一页恰恰是用户盯着
 * 看最久的那一页的概率并不低）。
 *
 * 两件东西一起出现，缺一不可：
 *   - 顶栏右侧的红底 DEMO 徽标（几何见 pfd_statusbar.h 的 PK_UI_DEMO_BADGE_*）；
 *   - 整屏一圈红框。徽标是"看得见"，红框是"扫一眼就知道这一屏不正常"——
 *     座舱里多数时候只有余光扫过，一枚 96 px 的 chip 是会被漏掉的。
 *
 * 宿主每帧同步一次（固件在 pfd.c，模拟器在 sim/main.c），与 toast 同一个套路：
 * 状态的真源在 config_demo，这里只负责显示。
 */
void pk_ui_nav_set_demo(bool on);
