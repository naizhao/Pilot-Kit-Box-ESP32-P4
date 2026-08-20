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
#include "pfd_statusbar.h"   /* DEMO 徽标的槽位几何，与顶栏共用同一份 */

/* spec 视觉稿的调色板。全屏导航网格（nav_grid_page.c）逐值照抄同一组色号，
 * 两处的视觉才不会在切换形态时跳一下。 */
#define COL_FAB       lv_color_hex(0x2E6DF0)   /* --sel  主操作色 */
#define COL_FAB_PRESS lv_color_hex(0x1E4FBF)
#define COL_BAR_BG    lv_color_hex(0x0A0F1C)   /* --bar  顶/底栏底色 */
#define COL_TXT       lv_color_hex(0xE2ECF8)   /* --txt  主要文字 */

#define FAB_D        56
#define FAB_MARGIN   16
/* 高度带下沿与右信息框上沿之间的空隙中点。 */
#define FAB_DEFAULT_Y \
    ((PFD_TAPE_BOT + PFD_IB_TOP) / 2 - FAB_D / 2)

static lv_obj_t *s_fab;

/* FAB 的落点。spec §3.2：水平只吸附左/右边缘，垂直自由。
 * 存边缘 + 垂直坐标而不是存 x/y，是因为换屏或旋转后 x 会失效而「哪一侧」
 * 不会——持久化到 NVS 的也是这两个量。 */
static bool s_fab_left;                 /* true = 吸在左缘 */
static int  s_fab_y = FAB_DEFAULT_Y;
static bool s_dragging;
/* 本次按压过程中是否发生过拖动。
 *
 * s_dragging 在 RELEASED 里就被清掉了，而 CLICKED 紧随其后才到，那时已经
 * 分辨不出这一下是点击还是拖动的收尾——于是拖完 FAB 一松手，菜单就弹出来了。
 * 这个标志跨过 RELEASED 活到 CLICKED，由 CLICKED 消费。 */
static bool s_drag_happened;

/* 当前层级。spec §4.3：**最多两层、不做返回栈**——二级页面只能从诊断进入，
 * 返回目标唯一确定，所以一个 bool 就够，不需要压栈。
 *
 * 这个约束不是偷懒：无物理按键的设备上，返回栈越深越容易让人迷路，而飞行中
 * 没有余裕去数「我在第几层」。 */
static bool s_in_subpage;
/* 2026-08-01：曾有一个 s_pressed 跟踪 FAB 按下态，只喂 pk_ui_nav_fab_pressed()
 * 这一个「供模拟器核对命中区」的取值器，而模拟器从没调过。按下态的视觉由
 * LVGL 的 LV_STATE_PRESSED 样式（COL_FAB_PRESS）负责，与它无关。 */

/* 中文字体。TinyTTF 在运行时从子集 TTF 渲染字形，一份轮廓服务所有字号——
 * 每个字号只是一个 lv_font_t 句柄，不再各存一份 CJK 位图。 */
static const lv_font_t *s_font_zh_m;
/* 二级页 backbar 专用的一档，视觉高度对齐 framebuffer 那侧的页面标题
 * （PK_UI_TITLE_SIZE = PK_AA_M）。具体尺寸与量法见 pk_ui_nav_init()。
 *
 * 不能复用 s_font_zh_m：它是 26 px、且拉丁回退到 Montserrat 28，于是二级页顶
 * 上那条「← DIAGNOSTICS」比总览页自己的「DIAGNOSTICS」大了将近一半——评审说的
 * 「点进子页面后字号比外面还大」就是这里，不是子系统名（那个一直是 M）。 */
static const lv_font_t *s_font_zh_title;

/* FAB 自身的落点，随吸附边缘。 */
static int32_t fab_x(void)
{
    return s_fab_left ? FAB_MARGIN : PK_DISPLAY_W - FAB_MARGIN - FAB_D;
}

/* 供渲染/触摸线程做浮层避让——它们与 LVGL 任务并发,这里只读纯静态量
 * (s_fab_left/s_fab_y/s_fab_hidden 由 LVGL 任务维护),绝不触碰 lv_obj_*。 */
static bool s_fab_hidden_mirror;

bool pk_ui_nav_fab_rect(int *out_x, int *out_y, int *out_w, int *out_h)
{
    if (s_fab == NULL || s_fab_hidden_mirror) return false;
    if (out_x) *out_x = (int)fab_x();
    if (out_y) *out_y = (int)s_fab_y;
    if (out_w) *out_w = FAB_D;
    if (out_h) *out_h = FAB_D;
    return true;
}

static void fab_apply_pos(void)
{
    if (s_fab) {
        lv_obj_set_pos(s_fab, fab_x(), s_fab_y);
    }
}

/* ── 二级页面 ──────────────────────────────────────────────────
 *
 * spec §4.2 开宗明义：**无物理按键 = 没有系统级返回**，二级页面若不设计返回，
 * 用户进去即出不来。因此三条退路必须同时可用：
 *
 *   ① 顶栏「← 诊断」按钮   ② FAB（此时变 ←）   ③ 右滑手势
 *
 * 三条都实现在这里，任何一条失效都还剩两条。二级页面里 FAB 是返回键而不是
 * 菜单键——一级页面之间的切换入口在子页里给出来，只会让人以为能直接跳走。
 */
#define BACKBAR_H   PK_UI_BACKBAR_H   /* 单一来源在 pk_ui_nav.h */

/* 胶囊底板允许越过内容列 8 px，但**文字**必须落在 PK_UI_PAD_L 上：
 * x = PK_UI_PAD_L - 8，左右内边距 = 8，两个 8 抵消，字正好从 16 开始。
 * 这与列表页行高亮从 PAD_L-8 起画是同一套「底板可越线、文字不可」的规矩。
 *
 * 原来是 x=8 + 内边距 14，字落在 22，于是详情页的键列被迫也写成 24 去迁就它
 * ——一个 LVGL 控件的内边距不该反过来决定 framebuffer 那侧的版面。 */
#define BACKBAR_BLEED   8

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
    lv_obj_align(s_backbar, LV_ALIGN_TOP_LEFT,
                 PK_UI_PAD_L - BACKBAR_BLEED, PK_UI_BACKBAR_TOP);
    lv_obj_set_style_bg_color(s_backbar, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(s_backbar, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_backbar, 0, 0);
    lv_obj_set_style_radius(s_backbar, 8, 0);
    lv_obj_set_style_pad_hor(s_backbar, BACKBAR_BLEED, 0);
    lv_obj_set_style_pad_ver(s_backbar, 0, 0);
    lv_obj_remove_flag(s_backbar, LV_OBJ_FLAG_SCROLLABLE);

    s_backbar_label = lv_label_create(s_backbar);
    lv_obj_set_style_text_font(s_backbar_label, s_font_zh_title, 0);
    lv_obj_set_style_text_color(s_backbar_label, COL_TXT, 0);
    lv_obj_center(s_backbar_label);

    lv_obj_add_event_cb(s_backbar, back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_backbar, LV_OBJ_FLAG_HIDDEN);
}

void pk_ui_nav_set_subpage(bool on, const char *parent_title)
{
    backbar_ensure();
    s_in_subpage = on;

    if (on) {
        char buf[32];
        lv_snprintf(buf, sizeof(buf), LV_SYMBOL_LEFT " %s",
                    parent_title ? parent_title : "");
        lv_label_set_text(s_backbar_label, buf);
        lv_obj_remove_flag(s_backbar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_backbar);
    } else {
        lv_obj_add_flag(s_backbar, LV_OBJ_FLAG_HIDDEN);
    }

    /* FAB 一键两用：一级页是 ☰（打开全屏导航网格），二级页是 ←（返回）。
     * spec §4.3 要求它的**位置固定不变**——用户只需记住一个位置，含义由
     * 图标区分。 */
    if (s_fab) {
        lv_obj_t *icon = lv_obj_get_child(s_fab, 0);
        if (icon) lv_label_set_text(icon, on ? LV_SYMBOL_LEFT : LV_SYMBOL_LIST);
    }
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
        s_drag_happened = false;    /* 新的一次按压，重新判定 */
        break;

    case LV_EVENT_LONG_PRESSED:
        /*
         * 进入拖动态：半透明 + 一圈亮边 + 阴影加重，让人看出「它现在跟手了」。
         *
         * **不能用 transform_scale 做微放大**，哪怕它是最自然的表达。踩坑记录
         * （2026-08-02 真机实测「一长按 FAB 就死机」）：
         *
         *   lv_obj_style.c:1090  transform_scale != 256 → LV_LAYER_TYPE_TRANSFORM
         *   TRANSFORM layer 必须**一次性**拿到整块 buffer，不像 SIMPLE layer 那样
         *   能受 LV_DRAW_LAYER_SIMPLE_BUF_SIZE 限制分块渲染；
         *   而本工程 LVGL 池只有 CONFIG_LV_MEM_SIZE_KILOBYTES=64，分配不出来。
         *   分配失败时 lv_draw.c:506 只是 "Try later" 返回 NULL，随后
         *   lv_draw.c:302 在 LV_OS_NONE 下是**裸忙等** `while(!dispatch_req);`——
         *   整个 LVGL 都跑在 pfd 这一个任务里，没有第二个线程能释放内存或发请求，
         *   于是永久占满 CPU0，IDLE0 饿死，看门狗每 15 s 报一次 `CPU 0: pfd`。
         *
         * 换句话说：这个架构（单线程 LVGL + 64 KB 池）下 TRANSFORM layer 一律
         * 不可用。要改这里，先确认 lv_obj_style.c 的 calculate_layer_type() 不会
         * 把新写法判成 TRANSFORM——bg_opa / border / shadow 都安全（只有
         * opa_layered、bitmap_mask、blend_mode 会退到可分块的 SIMPLE layer）。
         */
        s_dragging = true;
        s_drag_happened = true;
        lv_obj_set_style_border_width(s_fab, 3, 0);
        lv_obj_set_style_border_color(s_fab, lv_color_white(), 0);
        lv_obj_set_style_border_opa(s_fab, LV_OPA_90, 0);
        lv_obj_set_style_shadow_width(s_fab, 20, 0);
        lv_obj_set_style_bg_opa(s_fab, LV_OPA_60, 0);
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
        /* 夹在屏内：顶栏之下、屏底之上，别拖到看不见的地方。
         *
         * 横向同样要夹，且理由与纵向一样实在：手指按住 FAB 中心往边上拖，
         * FAB 与手指的相对偏移是保持的，手指到 x=0 时 FAB 左缘已经是 -28，
         * 一半圆钮滑出屏外。松手会吸附回来，但拖的过程中它是缺一块的。 */
        if (nx < 0) nx = 0;
        if (nx > PK_DISPLAY_W - FAB_D) nx = PK_DISPLAY_W - FAB_D;
        if (ny < PFD_BAR_BOT) ny = PFD_BAR_BOT;
        if (ny > PK_DISPLAY_H - FAB_D) ny = PK_DISPLAY_H - FAB_D;
        lv_obj_set_pos(s_fab, nx, ny);
        break;
    }

    case LV_EVENT_RELEASED:
        if (!s_dragging) break;
        s_dragging = false;
        /* 复原拖动态的三样，与 LONG_PRESSED 一一对应；数值取 pk_ui_nav_init()
         * 里建 FAB 时设的那一套，别在这里另起一份。 */
        lv_obj_set_style_border_width(s_fab, 0, 0);
        lv_obj_set_style_shadow_width(s_fab, 12, 0);
        lv_obj_set_style_bg_opa(s_fab, LV_OPA_80, 0);

        /* 吸附到更近的一侧，按 FAB 中心判定。 */
        s_fab_left = (lv_obj_get_x(s_fab) + FAB_D / 2) < (PK_DISPLAY_W / 2);
        s_fab_y    = lv_obj_get_y(s_fab);
        fab_apply_pos();
        pk_ui_nav_on_fab_moved(s_fab_left,
                               s_fab_y * 100 / (PK_DISPLAY_H - FAB_D));
        break;

    case LV_EVENT_CLICKED:
        /* 拖动的收尾也会走到这里——FAB 拖动时是跟着手指的，松手那一刻指针
         * 必然还落在它身上，LVGL 照样判定为点击。不挡掉的话，每拖一次
         * 位置，菜单就跟着弹出来一次。 */
        if (s_drag_happened) {
            s_drag_happened = false;
            break;
        }
        /* 二级页面里 FAB 是返回键，不是菜单键。 */
        if (s_in_subpage) {
            pk_ui_nav_set_subpage(false, NULL);
            pk_ui_nav_on_back();
        } else {
            /* 一级页面：打开主菜单 = 全屏导航网格（nav_grid_page.c）。
             *
             * 此前这里展开的是一条横向 dock：7 个页签 × 94 px + 分隔 + 动作区
             * 算出 761 px，而 FAB 两侧留白之后可用宽度只有 720，溢出 41 px，
             * 第 8 项根本排不进来。网格是自绘的 framebuffer 层，本文件（平台
             * 无关，模拟器编同一份）够不着，所以走弱符号回调交给宿主。 */
            pk_ui_nav_on_menu();
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

    /* backbar 档：22 px。backbar 的文字几乎全是拉丁页名，**回退档**才是这里真正
     * 决定视觉大小的那一档（sdkconfig.defaults 为此打开 CONFIG_LV_FONT_MONTSERRAT_22）。
     *
     * 为什么不是 18：上一版照抄了 PK_AA_M 的标称 18 px，结果 backbar 反而比外层
     * 标题细一圈——两侧字体不同，标称 px 不可比，只能量像素：
     *   PK_AA_M（B612 Mono 位图，标称 18）  大写字高 16 px / CJK 字身 18 px
     *   Montserrat 18                      大写字高 13 px  ← 细一圈的真凶
     *   Montserrat 22                      大写字高 16 px  ← 与外层齐平
     * TinyTTF 那侧同步取 22（CJK 字身量得 19 px），中西文混排才不会一高一矮。
     * 改这里之前先截图量，别只调数字。 */
    s_font_zh_title = lv_tiny_ttf_create_data(pk_lv_font_zh_ttf,
                                              pk_lv_font_zh_ttf_size, 22);
    if (s_font_zh_title) {
        ((lv_font_t *)s_font_zh_title)->fallback = &lv_font_montserrat_22;
    }

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

void pk_ui_nav_set_fab_hidden(bool hidden)
{
    if (s_fab == NULL) return;
    s_fab_hidden_mirror = hidden;
    if (hidden) {
        lv_obj_add_flag(s_fab, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_fab, LV_OBJ_FLAG_HIDDEN);
    }
}




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

/* ── 默认动作实现（弱符号）────────────────────────────────────────
 *
 * 导航层不该知道宿主是固件还是模拟器：打开菜单在固件里要调
 * pk_nav_grid_page_open()，在模拟器里只需打印一行。做成弱符号，宿主想接管
 * 就自己定义一个同名强符号，不接管也能链接通过——比在这里 #ifdef 分平台干净。
 */
__attribute__((weak)) void pk_ui_nav_on_menu(void)
{
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
 *     于是被浮在上面的控件（FAB 等）盖住——而 toast 恰恰是最该盖住别人的东西。
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
    /* 永远在最上层：菜单打开时弹出的提示（调平已保存…）也得看得见。 */
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

/* ── 演示模式常驻标识 ──────────────────────────────────────────────
 *
 * 见 pk_ui_nav.h 里那段说明。这里只补两个实现上的取舍：
 *
 *   1. **不做闪烁**。航电惯例里闪烁留给需要立刻处置的 warning（着火、失速），
 *      演示模式不是那类——它是一个持续存在的状态（caution），常亮红更贴切，
 *      而且闪烁的标识会有半个周期是「看不见」的，正好违背了"永远看得见"。
 *   2. 红框走 border 而不是四条 lv_obj。一个全屏透明底 + 3 px 边框的对象，
 *      LVGL 只重绘边框那四条窄带；拆成四个对象反而多三次 draw task。
 *
 * 两个对象都 remove CLICKABLE：它们铺在最上层，留着可点会把 FAB 以及各页
 * 自绘的命中区全部吞掉。LVGL 的命中测试跳过非 clickable 对象。
 */
static lv_obj_t *s_demo_badge;
static lv_obj_t *s_demo_frame;

#define COL_DEMO      lv_color_hex(0xD01820)   /* 与 toast 的错误红同族但更饱和 */
#define COL_DEMO_EDGE lv_color_hex(0xFF3A32)

static void demo_ensure(void)
{
    if (s_demo_badge) return;
    lv_obj_t *scr = lv_screen_active();

    s_demo_frame = lv_obj_create(scr);
    lv_obj_set_size(s_demo_frame, PK_DISPLAY_W, PK_DISPLAY_H);
    lv_obj_set_pos(s_demo_frame, 0, 0);
    lv_obj_set_style_bg_opa(s_demo_frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(s_demo_frame, 0, 0);
    lv_obj_set_style_pad_all(s_demo_frame, 0, 0);
    lv_obj_set_style_border_width(s_demo_frame, 3, 0);
    lv_obj_set_style_border_color(s_demo_frame, COL_DEMO_EDGE, 0);
    lv_obj_set_style_border_opa(s_demo_frame, LV_OPA_90, 0);
    lv_obj_remove_flag(s_demo_frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_demo_frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_demo_frame, LV_OBJ_FLAG_HIDDEN);

    s_demo_badge = lv_obj_create(scr);
    lv_obj_set_size(s_demo_badge, PK_UI_DEMO_BADGE_W, PK_UI_DEMO_BADGE_H);
    /* 槽位来自 pfd_statusbar —— 各页顶栏按同一个数让开宽度，两处不能各写各的。 */
    lv_obj_set_pos(s_demo_badge, PK_UI_DEMO_BADGE_X, PK_UI_DEMO_BADGE_Y);
    lv_obj_set_style_bg_color(s_demo_badge, COL_DEMO, 0);
    lv_obj_set_style_bg_opa(s_demo_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_demo_badge, 1, 0);
    lv_obj_set_style_border_color(s_demo_badge, COL_DEMO_EDGE, 0);
    lv_obj_set_style_radius(s_demo_badge, 6, 0);
    lv_obj_set_style_pad_all(s_demo_badge, 0, 0);
    lv_obj_remove_flag(s_demo_badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_demo_badge, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *l = lv_label_create(s_demo_badge);
    lv_obj_set_style_text_font(l, s_font_zh_title, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    lv_obj_center(l);

    lv_obj_add_flag(s_demo_badge, LV_OBJ_FLAG_HIDDEN);
}

void pk_ui_nav_set_demo(bool on)
{
    demo_ensure();

    if (!on) {
        lv_obj_add_flag(s_demo_badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_demo_frame, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* 文案每次都重设：语言可以在设置页当场切，而这枚徽标在切语言的那一瞬间
     * 也必须是对的——它是安全件，不能有"下一次进演示模式才更新"的窗口。
     * 前缀用 LV_SYMBOL_WARNING（经 Montserrat 回退取字形），符号本身跨语言。 */
    lv_obj_t *l = lv_obj_get_child(s_demo_badge, 0);
    if (l) {
        char buf[32];
        lv_snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING " %s",
                    pk_i18n_text(PK_TR_DEMO_BADGE));
        lv_label_set_text(l, buf);
    }
    lv_obj_remove_flag(s_demo_badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_demo_frame, LV_OBJ_FLAG_HIDDEN);
    /* 压在 FAB / toast 之上。toast 是瞬时的，被盖住一两秒无所谓；
     * 演示标识被盖住哪怕一帧都不行。 */
    lv_obj_move_foreground(s_demo_frame);
    lv_obj_move_foreground(s_demo_badge);
}
