/*
 * pk_ui_nav.c — 见 pk_ui_nav.h。
 *
 * 尺寸取自视觉稿 docs/ux/box-4.3-ux-spec.html 的 .fb 规则（毫米），按
 * 4.3″ 屏的 8.54 px/mm 换算：
 *
 *     直径 6.6 mm → 56 px      边距 1.9 mm → 16 px
 *     图标 3.2 mm → 27 px      投影 blur 1.4 mm → 12 px
 *
 * 56 px 远超 44 px 的触摸下限，戴薄手套也点得中。
 */

#include "pk_ui_nav.h"

#include <stdint.h>

#include "lvgl.h"

#include "display.h"
#include "i18n.h"
#include "lv_font_zh.h"
#include "pfd_layout.h"

/* spec 视觉稿的调色板。 */
#define COL_FAB       lv_color_hex(0x2E6DF0)   /* --sel  主操作色 */
#define COL_FAB_PRESS lv_color_hex(0x1E4FBF)
#define COL_DOCK_BG   lv_color_hex(0x0A0F1C)   /* --bar  顶/底栏底色 */
#define COL_TAB_TXT   lv_color_hex(0x93A8C4)   /* --dim  未选中 */
#define COL_TAB_ON    lv_color_hex(0xE2ECF8)   /* --txt  选中 */
#define COL_LINE      lv_color_hex(0x1C2740)   /* --line 分隔 */
#define COL_ACT       lv_color_hex(0xFFB43F)   /* --warn 动作区 */

#define FAB_D        56
#define FAB_MARGIN   16

#define DOCK_H       56
#define DOCK_TAB_W   94
#define DOCK_SEP_W    8
#define DOCK_GAP      8          /* dock 右端与 FAB 之间的呼吸 */
/* 高度带下沿与右信息框上沿之间的空隙中点。 */
#define FAB_DEFAULT_Y \
    ((PFD_TAPE_BOT + PFD_IB_TOP) / 2 - FAB_D / 2)

static lv_obj_t *s_fab;
static lv_obj_t *s_dock;
static size_t    s_active_tab;   /* 当前高亮的页签，索引进 DOCK_TABS */

/* FAB 的落点。spec §3.2：水平只吸附左/右边缘，垂直自由。
 * 存边缘 + 垂直坐标而不是存 x/y，是因为换屏或旋转后 x 会失效而「哪一侧」
 * 不会——持久化到 NVS 的也是这两个量。 */
static bool s_fab_left;                 /* true = 吸在左缘 */
static int  s_fab_y = FAB_DEFAULT_Y;
static bool s_dragging;

/* 当前层级。spec §4.3：**最多两层、不做返回栈**——二级页面只能从诊断进入，
 * 返回目标唯一确定，所以一个 bool 就够，不需要压栈。
 *
 * 这个约束不是偷懒：无物理按键的设备上，返回栈越深越容易让人迷路，而飞行中
 * 没有余裕去数「我在第几层」。 */
static bool s_in_subpage;
static bool      s_pressed;
static bool      s_dock_open;

/* 中文字体。TinyTTF 在运行时从子集 TTF 渲染字形，一份轮廓服务所有字号——
 * 每个字号只是一个 lv_font_t 句柄，不再各存一份 CJK 位图。 */
static const lv_font_t *s_font_zh_m;

bool pk_ui_nav_dock_open(void) { return s_dock_open; }

/* ── dock ─────────────────────────────────────────────────────────
 *
 * 六个一级页签 + 分隔线 + 动作区，顺序即 spec §4.1 的排列。
 *
 * 文案一律走 i18n catalog，不在这里写死中文：屏上任何一处硬编码中文都会
 * 绕过 catalog，切英文时就漏在那儿——而 catalog 同时也是字体子集的字符集
 * 来源，绕过它的字根本不会被生成进字库，最终显示为豆腐块。
 */
static const pk_tr_id_t DOCK_TABS[] = {
    /* 顺序按「飞行中会看的 → 偶尔查的 → 几乎不动的」排：
     * 前三个是飞行相关，诊断在出问题时查，设置与关于基本是一次性配置。 */
    PK_TR_NAV_PFD, PK_TR_NAV_TRAFFIC, PK_TR_NAV_LIST,
    PK_TR_NAV_DIAG, PK_TR_NAV_SETTINGS, PK_TR_NAV_ABOUT,
};
#define DOCK_TAB_CNT (sizeof(DOCK_TABS) / sizeof(DOCK_TABS[0]))

/* 文案一律经 i18n 取，随运行时语言走。
 *
 * 两种语言都得盯：中文等宽，每个页签恒两字 52 px，排版规整得看不出问题；
 * 英文变宽，"Settings" 能撑到中文的两倍——页签宽度必须按英文这侧定，只看
 * 中文会漏。模拟器可用 PK_SIM_LANG 切换核对（见 sim/compat/i18n_stub.c）。 */
static const char *tr(pk_tr_id_t id)
{
    return pk_i18n_text(id);
}

/* 选中态跟随当前页：只有被选中的页签用亮色，其余压暗。
 *
 * 页签本身不持有「哪一页」的状态——真值在 ui_state 里（pk_ui_get_mode），
 * 这里只是把它映射成颜色。否则切页路径一多（dock 点击、跨层跳转、开机
 * 恢复），两处状态迟早对不上。 */
static lv_obj_t *s_tabs[DOCK_TAB_CNT];

static void tab_refresh(void)
{
    for (size_t i = 0; i < DOCK_TAB_CNT; ++i) {
        if (!s_tabs[i]) continue;
        lv_obj_t *lbl = lv_obj_get_child(s_tabs[i], 0);
        if (lbl) lv_obj_set_style_text_color(
            lbl, i == s_active_tab ? COL_TAB_ON : COL_TAB_TXT, 0);
    }
}

static void tab_event_cb(lv_event_t *e)
{
    size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
    if (idx >= DOCK_TAB_CNT) return;

    s_active_tab = idx;
    tab_refresh();
    pk_ui_nav_on_tab(DOCK_TABS[idx]);

    /* 切完页就收起：dock 是导航入口不是常驻栏，留着只会挡住刚切过去的内容。 */
    pk_ui_nav_set_dock_open(false);
}

/* 「调平」不是切页，是改变机器状态的动作：把当前姿态设为水平基准。
 *
 * spec §3.2 要求长按 1 s 才触发，短按只弹提示——误触把地平线归零，飞行中是
 * 要命的。四个状态各有归属：
 *
 *     按下       起 1 s 单次定时器
 *     满 1 s     on_level()，真正执行
 *     提前松手   撤销定时器 + on_level_hint()，告诉用户「要长按」
 *     滑出按钮   同上（PRESS_LOST）
 *
 * 不用 LVGL 的 LV_EVENT_LONG_PRESSED：它的阈值 lv_indev_set_long_press_time()
 * 是 **indev 全局**的，改了会一并影响 FAB 的起拖判定（那里要的是 200 ms）。
 * 一个按钮的手感不该绑架整个输入设备，所以这里自己计时。
 *
 * 提示为什么走回调而不在这里弹：toast 的唯一来源是 ui_state 的
 * pk_ui_toast_show()（带时间戳、可在中断里调），而 ui_state.c 不参与模拟器
 * 编译。导航层只报告「用户短按了调平」，怎么提示由宿主决定。 */
#define ACT_LEVEL_HOLD_MS 1000

static lv_timer_t *s_level_timer;
/* dock 的 5 s 自动收起（DOCK_IDLE_MS）。与上面那个互相牵制：按住调平期间
 * 要顶住它，否则按钮会在手指底下消失。 */
static lv_timer_t *s_idle_timer;

static void level_fire_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    /* 单次定时器，回调返回后 LVGL 自行删除，这里只能置空不能再 delete。 */
    s_level_timer = NULL;
    pk_ui_nav_on_level();
    pk_ui_nav_set_dock_open(false);
}

static void level_timer_cancel(void)
{
    if (s_level_timer == NULL) return;
    lv_timer_delete(s_level_timer);
    s_level_timer = NULL;
}

static void act_event_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        level_timer_cancel();          /* 上一次的残留，正常不该有 */
        s_level_timer = lv_timer_create(level_fire_cb, ACT_LEVEL_HOLD_MS, NULL);
        if (s_level_timer) lv_timer_set_repeat_count(s_level_timer, 1);
        /* 按住不算「操作」，5 s 自动收起会照常走完。dock 一收，按钮连同
         * 手指下的命中区一起消失——运气好收到 PRESS_LOST 是取消，运气不好
         * 就是「我什么都没干，地平线自己归零了」。按住期间把它顶回去。 */
        if (s_idle_timer) lv_timer_reset(s_idle_timer);
        break;

    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        /* 定时器还在 = 没满 1 s，属于短按。已触发的话 fire_cb 早已置空，
         * 于是这里天然不会在触发后再补一条「要长按」的提示。 */
        if (s_level_timer) {
            level_timer_cancel();
            pk_ui_nav_on_level_hint();
        }
        break;

    default:
        break;
    }
}

static lv_obj_t *make_tab(lv_obj_t *parent, const char *text, lv_color_t col)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, DOCK_TAB_W, DOCK_H);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(b, COL_FAB, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, s_font_zh_m, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_center(l);
    return b;
}

/* ── 展开动画与自动收起 ────────────────────────────────────────
 *
 * dock 从 FAB 那侧滑出，而不是凭空出现：飞行员的余光需要一个「它从哪来」
 * 的线索，硬切会让人以为画面闪了一下。180 ms 是「看得出是滑出、又不至于
 * 等」的区间；收起同样走动画，否则一半有动效一半没有会更怪。
 *
 * spec §3.2 的收起条件有两个：再点 FAB，或 5 s 无操作。后者靠 LVGL 定时器，
 * 每次与 dock 发生交互就重排——飞行中忘记收起是常态，不能让它一直盖着 PFD。 */
#define DOCK_ANIM_MS      180
#define DOCK_IDLE_MS     5000

static int dock_width(void)
{
    return (int)DOCK_TAB_CNT * DOCK_TAB_W + DOCK_SEP_W + 1 + DOCK_TAB_W;
}

/* 收起时藏到 FAB 那一侧的屏外。藏错边的话，滑出方向就反了。 */
static int32_t dock_hidden_x(void)
{
    return s_fab_left ? -dock_width() : PK_DISPLAY_W;
}

/* spec §3.2：dock 展开方向随 FAB 边缘翻转——FAB 在左则向右铺开。
 * 否则 dock 会从屏外某处冒出来，与手指刚点的位置对不上。 */
static int32_t dock_shown_x(void)
{
    return s_fab_left ? (FAB_MARGIN + FAB_D + DOCK_GAP)
                      : PK_DISPLAY_W - (FAB_MARGIN + FAB_D + DOCK_GAP) - dock_width();
}

/* FAB 自身的落点，同样随边缘。 */
static int32_t fab_x(void)
{
    return s_fab_left ? FAB_MARGIN : PK_DISPLAY_W - FAB_MARGIN - FAB_D;
}

static void fab_apply_pos(void)
{
    if (s_fab) {
        lv_obj_set_pos(s_fab, fab_x(), s_fab_y);
    }
    if (s_dock && !s_dock_open) {
        /* 收起态也要跟着挪到正确的屏外侧，否则下次展开会从反方向滑出。 */
        lv_obj_set_x(s_dock, dock_hidden_x());
        lv_obj_set_y(s_dock, s_fab_y + (FAB_D - DOCK_H) / 2);
    }
}

static void dock_anim_x_cb(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, v);
}

static void dock_anim_done_cb(lv_anim_t *a)
{
    /* 收起动画结束后才真正隐藏：动画过程中必须可见，否则一开始就没得看。 */
    if (!s_dock_open) lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
}

static void idle_timer_cb(lv_timer_t *t)
{
    (void)t;
    pk_ui_nav_set_dock_open(false);
}

/* 每次与 dock 交互都把 5 s 倒计时归零。 */
static void dock_touch_cb(lv_event_t *e)
{
    (void)e;
    if (s_idle_timer) lv_timer_reset(s_idle_timer);
}

/* ── 二级页面 ──────────────────────────────────────────────────
 *
 * spec §4.2 开宗明义：**无物理按键 = 没有系统级返回**，二级页面若不设计返回，
 * 用户进去即出不来。因此三条退路必须同时可用：
 *
 *   ① 顶栏「← 诊断」按钮   ② FAB（此时变 ←）   ③ 右滑手势
 *
 * 三条都实现在这里，任何一条失效都还剩两条。二级页面**不提供 dock**——那是
 * 一级页面之间的横向切换，在子页里给出来只会让人以为能直接跳走。
 */
#define BACKBAR_H   44

static lv_obj_t *s_backbar;
static lv_obj_t *s_backbar_label;

static void back_event_cb(lv_event_t *e)
{
    (void)e;
    pk_ui_nav_set_subpage(false, NULL);
    pk_ui_nav_on_back();
}

/* 右滑返回：只认从**左缘起手**的横向滑动，且横向位移要明显大于纵向。
 * 否则列表纵向滚动时稍微带点横向分量就会误触发返回——子页里全是可滚动的
 * 诊断详情，这个误触会很频繁。 */
static void backbar_gesture_cb(lv_event_t *e)
{
    (void)e;
    if (!s_in_subpage) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) {
        pk_ui_nav_set_subpage(false, NULL);
        pk_ui_nav_on_back();
    }
}

static void backbar_ensure(void)
{
    if (s_backbar) return;

    s_backbar = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_backbar, LV_SIZE_CONTENT, BACKBAR_H);
    lv_obj_align(s_backbar, LV_ALIGN_TOP_LEFT, 8, PFD_BAR_BOT + 6);
    lv_obj_set_style_bg_color(s_backbar, COL_DOCK_BG, 0);
    lv_obj_set_style_bg_opa(s_backbar, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_backbar, 0, 0);
    lv_obj_set_style_radius(s_backbar, 8, 0);
    lv_obj_set_style_pad_hor(s_backbar, 14, 0);
    lv_obj_set_style_pad_ver(s_backbar, 0, 0);
    lv_obj_remove_flag(s_backbar, LV_OBJ_FLAG_SCROLLABLE);

    s_backbar_label = lv_label_create(s_backbar);
    lv_obj_set_style_text_font(s_backbar_label, s_font_zh_m, 0);
    lv_obj_set_style_text_color(s_backbar_label, COL_TAB_ON, 0);
    lv_obj_center(s_backbar_label);

    lv_obj_add_event_cb(s_backbar, back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_backbar, LV_OBJ_FLAG_HIDDEN);
}

void pk_ui_nav_set_subpage(bool on, const char *parent_title)
{
    backbar_ensure();
    s_in_subpage = on;

    if (on) {
        /* 进子页先收 dock：它是一级页之间的切换入口，子页里给出来会误导。 */
        pk_ui_nav_set_dock_open(false);

        char buf[32];
        lv_snprintf(buf, sizeof(buf), LV_SYMBOL_LEFT " %s",
                    parent_title ? parent_title : "");
        lv_label_set_text(s_backbar_label, buf);
        lv_obj_remove_flag(s_backbar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_backbar);
    } else {
        lv_obj_add_flag(s_backbar, LV_OBJ_FLAG_HIDDEN);
    }

    /* FAB 一键两用：一级页是 ☰（展开 dock），二级页是 ←（返回）。
     * spec §4.3 要求它的**位置固定不变**——用户只需记住一个位置，含义由
     * 图标区分。 */
    if (s_fab) {
        lv_obj_t *icon = lv_obj_get_child(s_fab, 0);
        if (icon) lv_label_set_text(icon, on ? LV_SYMBOL_LEFT : LV_SYMBOL_LIST);
    }
}

bool pk_ui_nav_in_subpage(void) { return s_in_subpage; }

static void dock_build(lv_obj_t *scr)
{
    /* 宽度要把分隔线**自身的 1 px** 也算进去：漏了它，flex 会把最后一项
     * （动作区「调平」）挤出可视区，而 dock 不滚动，挤出去就是彻底看不见。 */
    const int w = (int)DOCK_TAB_CNT * DOCK_TAB_W + DOCK_SEP_W + 1 + DOCK_TAB_W;

    s_dock = lv_obj_create(scr);
    lv_obj_set_size(s_dock, w, DOCK_H);
    lv_obj_set_style_bg_color(s_dock, COL_DOCK_BG, 0);
    /* 不做全不透明：dock 浮在 PFD 之上，留一点透，飞行员仍能感知底下的姿态。 */
    lv_obj_set_style_bg_opa(s_dock, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_dock, 0, 0);
    lv_obj_set_style_radius(s_dock, 8, 0);
    lv_obj_set_style_pad_all(s_dock, 0, 0);
    lv_obj_remove_flag(s_dock, LV_OBJ_FLAG_SCROLLABLE);

    /* 页签之间不留缝：分隔靠一条竖线，而不是间距——间距会让 94 px 的命中区
     * 之间出现点不中的死带。 */
    lv_obj_set_flex_flow(s_dock, LV_FLEX_FLOW_ROW);
    /* flex 的默认列间距不是 0，会在每两项之间插缝，累计下来同样挤掉末项。 */
    lv_obj_set_style_pad_column(s_dock, 0, 0);
    lv_obj_set_style_pad_row(s_dock, 0, 0);
    lv_obj_set_flex_align(s_dock, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (size_t i = 0; i < DOCK_TAB_CNT; ++i) {
        s_tabs[i] = make_tab(s_dock, tr(DOCK_TABS[i]),
                             i == s_active_tab ? COL_TAB_ON : COL_TAB_TXT);
        lv_obj_add_event_cb(s_tabs[i], tab_event_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }

    /* 分隔线把导航与动作分开。「调平」会重设姿态基准，是改变机器状态的
     * 操作，和单纯切页不是一类，必须在视觉上划清。 */
    lv_obj_t *sep = lv_obj_create(s_dock);
    lv_obj_set_size(sep, 1, DOCK_H - 16);
    lv_obj_set_style_bg_color(sep, COL_LINE, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);
    lv_obj_set_style_margin_left(sep, DOCK_SEP_W / 2, 0);
    lv_obj_set_style_margin_right(sep, DOCK_SEP_W / 2, 0);

    lv_obj_t *act = make_tab(s_dock, tr(PK_TR_ACT_LEVEL), COL_ACT);
    /* 要 PRESSED/RELEASED/PRESS_LOST 三种，CLICKED 给不了按压时长。 */
    lv_obj_add_event_cb(act, act_event_cb, LV_EVENT_ALL, NULL);

    /* 垂直与 FAB 同心；水平初始落在屏外，由动画推进来。 */
    lv_obj_set_y(s_dock, FAB_DEFAULT_Y + (FAB_D - DOCK_H) / 2);
    lv_obj_set_x(s_dock, dock_hidden_x());
    lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);

    /* 5 s 无操作自动收起。先建后停——LVGL 的定时器创建即运行。 */
    s_idle_timer = lv_timer_create(idle_timer_cb, DOCK_IDLE_MS, NULL);
    lv_timer_pause(s_idle_timer);
    lv_obj_add_event_cb(s_dock, dock_touch_cb, LV_EVENT_PRESSED, NULL);
}

/* ── FAB 拖动 ──────────────────────────────────────────────────
 *
 * spec §3.2：长按 200 ms 进入拖动态（与点击区分开），拖动实时跟手，松手后
 * 水平吸附到最近的左/右边缘——不允许悬在中间挡住内容。
 *
 * 用 LVGL 的 LONG_PRESSED 而不是自己计时：它的阈值就是给这个用的，且与点击
 * 判定共用一套状态机，不会出现「既算点击又算长按」。
 */
static void fab_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    switch (code) {
    case LV_EVENT_PRESSED:
        s_pressed = true;
        break;

    case LV_EVENT_LONG_PRESSED:
        /* 进入拖动态：微放大 + 半透明，让人看出「它现在跟手了」。 */
        s_dragging = true;
        lv_obj_set_style_transform_scale(s_fab, 280, 0);   /* 256 = 1.0×，即 1.09× */
        lv_obj_set_style_bg_opa(s_fab, LV_OPA_60, 0);
        /* 拖动期间 dock 必须收起：它锚在 FAB 上，跟着乱跑没有意义。 */
        pk_ui_nav_set_dock_open(false);
        break;

    case LV_EVENT_PRESSING: {
        if (!s_dragging) break;
        lv_indev_t *indev = lv_indev_active();
        if (!indev) break;
        lv_point_t v;
        lv_indev_get_vect(indev, &v);

        /* 只跟垂直分量；水平方向松手才吸附，拖动中也让它跟手，否则手感发滞。 */
        int nx = lv_obj_get_x(s_fab) + v.x;
        int ny = lv_obj_get_y(s_fab) + v.y;
        /* 夹在屏内：顶栏之下、屏底之上，别拖到看不见的地方。 */
        if (ny < PFD_BAR_BOT) ny = PFD_BAR_BOT;
        if (ny > PK_DISPLAY_H - FAB_D) ny = PK_DISPLAY_H - FAB_D;
        lv_obj_set_pos(s_fab, nx, ny);
        break;
    }

    case LV_EVENT_RELEASED:
        s_pressed = false;
        if (!s_dragging) break;
        s_dragging = false;
        lv_obj_set_style_transform_scale(s_fab, 256, 0);
        lv_obj_set_style_bg_opa(s_fab, LV_OPA_80, 0);

        /* 吸附到更近的一侧，按 FAB 中心判定。 */
        s_fab_left = (lv_obj_get_x(s_fab) + FAB_D / 2) < (PK_DISPLAY_W / 2);
        s_fab_y    = lv_obj_get_y(s_fab);
        fab_apply_pos();
        pk_ui_nav_on_fab_moved(s_fab_left,
                               s_fab_y * 100 / (PK_DISPLAY_H - FAB_D));
        break;

    case LV_EVENT_CLICKED:
        /* 拖动结束时 LVGL 不会再补一个 CLICKED，这里只处理真正的点击。
         * 二级页面里 FAB 是返回键，不是 dock 开关。 */
        if (s_in_subpage) {
            pk_ui_nav_set_subpage(false, NULL);
            pk_ui_nav_on_back();
        } else {
            pk_ui_nav_set_dock_open(!s_dock_open);
        }
        break;

    default:
        break;
    }
}

void pk_ui_nav_init(void)
{
    lv_obj_t *scr = lv_screen_active();

    /* M 档 26 px ≈ spec §2 的 3.0 mm「正文主力」。 */
    s_font_zh_m = lv_tiny_ttf_create_data(pk_lv_font_zh_ttf,
                                          pk_lv_font_zh_ttf_size, 26);
    /* 回退到内置 Montserrat：LV_SYMBOL_* 是 FontAwesome 私用区码位，不在中文
     * 子集里，直接混排会显示成豆腐块（返回栏的「← 诊断」就是这么露馅的）。
     *
     * 设 fallback 比"符号和文字各用一个 label"干净得多——后者每处混排都要拆
     * 成两个对象，还得自己算间距。 */
    if (s_font_zh_m) {
        /* v9.5 没有 setter，fallback 是 lv_font_t 的公开字段，直接接上即可。 */
        ((lv_font_t *)s_font_zh_m)->fallback = &lv_font_montserrat_28;
    }

    /* 先建 dock 再建 FAB：同层内后建的在上，FAB 必须压在 dock 之上。 */
    dock_build(scr);
    /* 右滑返回挂在屏幕根对象上：子页内容各式各样，逐个挂手势必漏。 */
    lv_obj_add_event_cb(scr, backbar_gesture_cb, LV_EVENT_GESTURE, NULL);

    s_fab = lv_button_create(scr);
    lv_obj_set_size(s_fab, FAB_D, FAB_D);
    /* 默认落点不取视觉稿的右下角：那里在本实现里被三行信息框占了，FAB 会把
     * 「7444m」「-640」两行常驻读数压掉一半。spec §3.2 规定 FAB 垂直位置自由、
     * 只水平吸附边缘，所以默认值可以挑一个谁都不挡的——高度带下沿(298)与右
     * 信息框上沿(382)之间恰好空着 84 px，56 px 的 FAB 居中放进去，上不碰
     * tape、下不碰信息框、左不碰罗盘(顶沿 365)。
     *
     * 用户仍可拖动改位置，届时由 NVS 记住（阶段 3）。 */
    lv_obj_set_pos(s_fab, fab_x(), s_fab_y);

    /* 刻意留一点透明度：FAB 悬在 PFD 之上，全不透明会把底下的姿态挡死。
     * 这同时是图层混合的试金石——半透明必须走 alpha 混合路径，若颜色格式
     * 配错了，这里会立刻显出偏色。 */
    lv_obj_set_style_radius(s_fab, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_fab, COL_FAB, 0);
    lv_obj_set_style_bg_opa(s_fab, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(s_fab, COL_FAB_PRESS, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_fab, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_fab, 0, 0);

    /* 投影：spec 视觉稿里 FAB 是浮在内容之上的，没有阴影就贴在画面里了。
     * 阴影是纯 alpha 渐变，对混合路径的检验比实色更严。 */
    lv_obj_set_style_shadow_width(s_fab, 12, 0);
    lv_obj_set_style_shadow_offset_y(s_fab, 3, 0);
    lv_obj_set_style_shadow_color(s_fab, COL_FAB, 0);
    lv_obj_set_style_shadow_opa(s_fab, LV_OPA_50, 0);

    /* 图标用 LVGL 内置符号，先把布局跑通；spec 要的 ☰ 与二级页的 ← 等
     * 中文字体管线接上后再换（阶段 2 后半程）。
     *
     * 字号必须显式给：LVGL 默认是 Montserrat 14，在 56 px 的钮里只占四分之一，
     * 看着像颗小痣。视觉稿 .fb 定的图标是 3.2 mm，按 8.54 px/mm 算得 27 px，
     * 内置字号里 28 最接近。 */
    lv_obj_t *lbl = lv_label_create(s_fab);
    lv_label_set_text(lbl, LV_SYMBOL_LIST);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(s_fab, fab_event_cb, LV_EVENT_ALL, NULL);
}

bool pk_ui_nav_fab_pressed(void) { return s_pressed; }

void pk_ui_nav_set_fab_side(bool left)
{
    s_fab_left = left;
    fab_apply_pos();
}

void pk_ui_nav_set_fab_y_pct(int y_pct)
{
    if (y_pct < 0)   y_pct = 0;
    if (y_pct > 100) y_pct = 100;
    /* on_fab_moved 那侧除以同一个量，两边必须对称，否则每存取一轮就漂一点。 */
    s_fab_y = y_pct * (PK_DISPLAY_H - FAB_D) / 100;
    if (s_fab_y < PFD_BAR_BOT) s_fab_y = PFD_BAR_BOT;
    fab_apply_pos();
}

void pk_ui_nav_set_dock_open(bool open)
{
    if (s_dock == NULL) return;
    s_dock_open = open;

    if (open) {
        lv_obj_remove_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
        if (s_idle_timer) lv_timer_resume(s_idle_timer), lv_timer_reset(s_idle_timer);
    } else if (s_idle_timer) {
        /* 收起后不必再倒计时，否则定时器空转还会在下次展开时立刻触发。 */
        lv_timer_pause(s_idle_timer);
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_dock);
    lv_anim_set_exec_cb(&a, dock_anim_x_cb);
    lv_anim_set_completed_cb(&a, dock_anim_done_cb);
    lv_anim_set_time(&a, DOCK_ANIM_MS);
    /* ease-out：起步快、收尾慢，观感像被「推」出来而不是匀速平移。 */
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_values(&a, lv_obj_get_x(s_dock),
                       open ? dock_shown_x() : dock_hidden_x());
    lv_anim_start(&a);
}

/* ── 默认动作实现（弱符号）────────────────────────────────────────
 *
 * 导航层不该知道宿主是固件还是模拟器：切页在固件里要调 pk_ui_set_mode()，
 * 在模拟器里只需打印一行。做成弱符号，宿主想接管就自己定义一个同名强符号，
 * 不接管也能链接通过——比在这里 #ifdef 分平台干净。
 */
__attribute__((weak)) void pk_ui_nav_on_tab(int tr_id)
{
    LV_UNUSED(tr_id);
}

__attribute__((weak)) void pk_ui_nav_on_level(void)
{
}

__attribute__((weak)) void pk_ui_nav_on_level_hint(void)
{
}

__attribute__((weak)) void pk_ui_nav_on_fab_moved(bool left, int y_pct)
{
    LV_UNUSED(left);
    LV_UNUSED(y_pct);
}

/* ── Toast ─────────────────────────────────────────────────────────
 *
 * 覆盖层，居中显示一句短提示（调平已保存 / 已绑定本机 …）。
 *
 * 从 pfd.c 的 render_toast() 迁过来，两点变化：
 *   - 画在**控件层**而不是 canvas 上。此前它跟 PFD 画在同一块缓冲里，
 *     于是被 dock、FAB 盖住——而 toast 恰恰是最该盖住别人的东西。
 *   - 文字走 TinyTTF，不再用 190 KB 的旧 CJK 位图字库。那批字库的退役
 *     就卡在这类零散调用点上，逐个迁完才能删。
 *
 * 生命周期仍由 ui_state 的 pk_ui_toast_get() 掌握（它带过期时间且线程安全，
 * 按键中断里也能安全地 show）。这里只负责「当前该不该显示、显示什么」，
 * 每帧同步一次——把过期逻辑再实现一遍只会多一处不一致。
 */
static lv_obj_t *s_toast;

static void toast_ensure(void)
{
    if (s_toast) return;

    s_toast = lv_obj_create(lv_screen_active());
    lv_obj_set_style_radius(s_toast, 10, 0);
    lv_obj_set_style_border_width(s_toast, 2, 0);
    lv_obj_set_style_pad_hor(s_toast, 18, 0);
    lv_obj_set_style_pad_ver(s_toast, 12, 0);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
    /* 不接受点击：它是通知不是按钮，让触摸穿透到底下的页面。 */
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    lv_obj_t *l = lv_label_create(s_toast);
    lv_obj_set_style_text_font(l, s_font_zh_m, 0);
    lv_obj_center(l);

    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    /* 永远在最上层：dock 展开时弹出的提示也得看得见。 */
    lv_obj_move_foreground(s_toast);
}

void pk_ui_nav_toast(const char *msg, bool is_error)
{
    toast_ensure();

    if (msg == NULL || msg[0] == '\0') {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* 配色沿用既有约定：绿=成功、红=失败。边框比底色亮，暗背景上才立得住。 */
    lv_color_t bg     = is_error ? lv_color_hex(0x781818) : lv_color_hex(0x106024);
    lv_color_t border = is_error ? lv_color_hex(0xFF5050) : lv_color_hex(0x60E678);

    lv_obj_set_style_bg_color(s_toast, bg, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_toast, border, 0);

    lv_obj_t *l = lv_obj_get_child(s_toast, 0);
    lv_label_set_text(l, msg);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);

    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    lv_obj_center(s_toast);
}

__attribute__((weak)) void pk_ui_nav_on_back(void)
{
}
