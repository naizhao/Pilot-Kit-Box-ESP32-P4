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
        /* 首项按「当前在 PFD 页」高亮。 */
        make_tab(s_dock, tr(DOCK_TABS[i]), i == 0 ? COL_TAB_ON : COL_TAB_TXT);
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

    make_tab(s_dock, tr(PK_TR_ACT_LEVEL), COL_ACT);

    /* dock 右端贴着 FAB 左侧，垂直与 FAB 同心——展开时像是从钮里推出来的。 */
    lv_obj_align(s_dock, LV_ALIGN_TOP_RIGHT,
                 -(FAB_MARGIN + FAB_D + DOCK_GAP),
                 FAB_DEFAULT_Y + (FAB_D - DOCK_H) / 2);
    lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
}

static void fab_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED)  s_pressed = true;
    if (code == LV_EVENT_RELEASED) s_pressed = false;
    if (code == LV_EVENT_CLICKED) pk_ui_nav_set_dock_open(!s_dock_open);
}

void pk_ui_nav_init(void)
{
    lv_obj_t *scr = lv_screen_active();

    /* M 档 26 px ≈ spec §2 的 3.0 mm「正文主力」。 */
    s_font_zh_m = lv_tiny_ttf_create_data(pk_lv_font_zh_ttf,
                                          pk_lv_font_zh_ttf_size, 26);

    /* 先建 dock 再建 FAB：同层内后建的在上，FAB 必须压在 dock 之上。 */
    dock_build(scr);

    s_fab = lv_button_create(scr);
    lv_obj_set_size(s_fab, FAB_D, FAB_D);
    /* 默认落点不取视觉稿的右下角：那里在本实现里被三行信息框占了，FAB 会把
     * 「7444m」「-640」两行常驻读数压掉一半。spec §3.2 规定 FAB 垂直位置自由、
     * 只水平吸附边缘，所以默认值可以挑一个谁都不挡的——高度带下沿(298)与右
     * 信息框上沿(382)之间恰好空着 84 px，56 px 的 FAB 居中放进去，上不碰
     * tape、下不碰信息框、左不碰罗盘(顶沿 365)。
     *
     * 用户仍可拖动改位置，届时由 NVS 记住（阶段 3）。 */
    lv_obj_align(s_fab, LV_ALIGN_TOP_RIGHT, -FAB_MARGIN, FAB_DEFAULT_Y);

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

void pk_ui_nav_set_dock_open(bool open)
{
    s_dock_open = open;
    if (open) lv_obj_remove_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
}
