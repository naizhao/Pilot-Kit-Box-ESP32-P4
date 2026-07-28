/*
 * lv_ui.c — 见 lv_ui.h。
 *
 * 尺寸取自视觉稿 docs/ux/box-4.3-ux-spec.html 的 .fb 规则（毫米），按
 * 4.3″ 屏的 8.54 px/mm 换算：
 *
 *     直径 6.6 mm → 56 px      边距 1.9 mm → 16 px
 *     图标 3.2 mm → 27 px      投影 blur 1.4 mm → 12 px
 *
 * 56 px 远超 44 px 的触摸下限，戴薄手套也点得中。
 */

#include "lv_ui.h"

#include "lvgl.h"

#include "display.h"
#include "lv_font_zh.h"
#include "pfd_layout.h"

/* spec 视觉稿的 --sel（选中/主操作色）。 */
#define COL_FAB       lv_color_hex(0x2E6DF0)
#define COL_FAB_PRESS lv_color_hex(0x1E4FBF)

#define FAB_D        56
#define FAB_MARGIN   16
/* 高度带下沿与右信息框上沿之间的空隙中点。 */
#define FAB_DEFAULT_Y \
    ((PFD_TAPE_BOT + PFD_IB_TOP) / 2 - FAB_D / 2)

static lv_obj_t *s_fab;
static bool      s_pressed;

/* 中文字体。TinyTTF 在运行时从子集 TTF 渲染字形，一份轮廓服务所有字号——
 * 每个字号只是一个 lv_font_t 句柄，不再各存一份 CJK 位图。 */
static const lv_font_t *s_font_zh_m;

const lv_font_t *pk_sim_ui_font_zh(void) { return s_font_zh_m; }

static void fab_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED)  s_pressed = true;
    if (code == LV_EVENT_RELEASED) s_pressed = false;
    if (code == LV_EVENT_CLICKED) {
        /* 展开 dock 是下一步的事；先确认事件通路是通的。 */
        LV_LOG_USER("FAB clicked");
    }
}

void pk_sim_ui_init(void)
{
    lv_obj_t *scr = lv_screen_active();

    /* M 档 26 px ≈ spec §2 的 3.0 mm「正文主力」。 */
    s_font_zh_m = lv_tiny_ttf_create_data(pk_lv_font_zh_ttf,
                                          pk_lv_font_zh_ttf_size, 26);

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

    /* 临时探针：验证 TinyTTF 能把 catalog 里的中文渲染出来。
     * dock 搭起来之后由真正的页签取代（阶段 3）。 */
    lv_obj_t *probe = lv_label_create(scr);
    lv_label_set_text(probe, "设置 · 语言 · 校准 · 版本");
    lv_obj_set_style_text_font(probe, s_font_zh_m, 0);
    lv_obj_set_style_text_color(probe, lv_color_white(), 0);
    lv_obj_set_style_bg_color(probe, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(probe, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(probe, 6, 0);
    lv_obj_set_style_radius(probe, 6, 0);
    lv_obj_align(probe, LV_ALIGN_TOP_MID, 0, PFD_BAR_BOT + 12);
}

bool pk_sim_ui_fab_pressed(void) { return s_pressed; }
