/*
 * keyboard_page.c — 见 keyboard_page.h。
 *
 * 版面（800×480）
 * ---------------
 *   y=0    ┌───────────────────────────────────────────────────────┐
 *          │ 设备名                        [ 清除 ]   [ 取消 ]      │ 76
 *   y=76   ├───────────────────────────────────────────────────────┤
 *          │ ┌───────────────────────────────────────────┐         │
 *          │ │ N123AB▌                            6/10   │         │ 输入框
 *          │ └───────────────────────────────────────────┘         │
 *          │ 仅限 A-Z 0-9 - _                                       │ 提示
 *   y=152  ├───┬───┬───┬───┬───┬───┬───┬───┬───┬───────────────────┤
 *          │ 1 │ 2 │ 3 │ 4 │ 5 │ 6 │ 7 │ 8 │ 9 │ 0 │               │
 *          │ Q │ W │ E │ R │ T │ Y │ U │ I │ O │ P │               │ 4 × 82
 *          │ A │ S │ D │ F │ G │ H │ J │ K │ L │删除│               │
 *          │ Z │ X │ C │ V │ B │ N │ M │ - │ _ │确定│               │
 *   y=480  └───┴───┴───┴───┴───┴───┴───┴───┴───┴───┘
 *
 * 触摸尺寸是这一页的第一约束，不是版面好看。屏 800 px ≈ 95 mm → 8.4 px/mm，
 * 通行下限 9 mm ≈ 76 px（口径同 settings_draw.c 的 SEG_MIN_TOUCH_W 那段注释）。
 * 于是先按 76 定尺寸再排版面：
 *
 *   键宽 800/10 = 80 px ≈ 9.5 mm      键高 (480-152)/4 = 82 px ≈ 9.8 mm
 *   页首两个按钮 144×76                 都不低于 76
 *
 * 键帽**视觉**比命中区各边窄 4 px（72×74）。缝隙是画出来的，命中区之间没有
 * 死带——两个 80 px 的按钮之间若真留 8 px 间距，那条缝上的点击会掉在地上。
 */
#include "keyboard_page.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "apt_detail_page.h"   /* pk_ui_fab_sync —— FAB 显隐的唯一入口 */
#include "display.h"
#include "i18n.h"
#include "pfd_aa_font.h"
#include "pfd_aa_text.h"
#include "pfd_draw.h"
#include "pfd_layout.h"
#include "pk_ui_nav.h"

/* ── 版面常量 ──────────────────────────────────────────────────── */

#define KBD_HDR_H       76                       /* 页首带高 = 触摸下限 */
#define KBD_EDIT_TOP    KBD_HDR_H
#define KBD_GRID_TOP    152
#define KBD_COLS        10
#define KBD_ROW_CNT     4
#define KBD_KEY_W       (PK_DISPLAY_W / KBD_COLS)                    /* 80 */
#define KBD_KEY_H       ((PK_DISPLAY_H - KBD_GRID_TOP) / KBD_ROW_CNT)/* 82 */
#define KBD_KEY_PAD     4                        /* 视觉内缩，命中区不缩 */

/* 触摸目标下限，与 settings_draw.c 的 SEG_MIN_TOUCH_W 同一个数、同一个理由。
 * 这里把它写成编译期断言而不是注释：键盘是全屏铺满的，改任何一个版面常量都
 * 会直接压到键的尺寸上，而屏上「小了一点」肉眼看不出来。 */
#define KBD_MIN_TOUCH   76
_Static_assert(KBD_KEY_W >= KBD_MIN_TOUCH, "键宽低于 9 mm 触摸下限");
_Static_assert(KBD_KEY_H >= KBD_MIN_TOUCH, "键高低于 9 mm 触摸下限");
_Static_assert(KBD_GRID_TOP + KBD_ROW_CNT * KBD_KEY_H == PK_DISPLAY_H,
               "键盘没有正好铺到屏底");

/* 页首两个按钮。右对齐排布，取消在最右——它是「退出」，与各页 FAB 的位置感
 * 一致（右下角是离开的方向）。 */
#define KBD_BTN_W       144
#define KBD_BTN_H       48
#define KBD_BTN_GAP     16
#define KBD_BTN_R       (PK_DISPLAY_W - PK_UI_PAD_L)     /* 784 */
#define KBD_CANCEL_X0   (KBD_BTN_R - KBD_BTN_W)          /* 640 */
#define KBD_CLEAR_X0    (KBD_CANCEL_X0 - KBD_BTN_GAP - KBD_BTN_W)  /* 480 */
#define KBD_BTN_Y0      ((KBD_HDR_H - KBD_BTN_H) / 2)

/* 输入框。高 52，M 档 26 px 上下各留 13。 */
#define KBD_BOX_X0      PK_UI_PAD_L
#define KBD_BOX_X1      (PK_DISPLAY_W - PK_UI_PAD_L)
#define KBD_BOX_Y0      (KBD_EDIT_TOP + 6)               /* 82  */
#define KBD_BOX_H       52
#define KBD_BOX_Y1      (KBD_BOX_Y0 + KBD_BOX_H)         /* 134 */
#define KBD_TEXT_X      (KBD_BOX_X0 + 16)
#define KBD_HINT_Y      (KBD_BOX_Y1 + 1)                 /* 135，XS 17 px */
_Static_assert(KBD_HINT_Y + PK_AA_XS_H <= KBD_GRID_TOP, "提示行压到键盘上了");

/* 键面上的两个特殊键，用控制字符当哨兵：它们不在允许的字符集里，永远不会
 * 与真正的按键混淆。 */
#define KBD_K_DEL   '\b'
#define KBD_K_OK    '\n'

static const char *const KBD_KEYS[KBD_ROW_CNT] = {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL\b",
    "ZXCVBNM-_\n",
};

/* ── 状态 ──────────────────────────────────────────────────────── */

static bool       s_active;
static const char *s_title = "";
static int        s_max_len = PK_KBD_TEXT_MAX;
static char       s_text[PK_KBD_TEXT_MAX + 1];
static int        s_len;

/* 本次编辑归谁（见 keyboard_page.h 的「结果回调」）。close_page() 不清它们：
 * 「先关页再回调」是有意的顺序（回调里会写 NVS / 起后台任务，那时渲染任务
 * 已经切回底下那一页），所以回调指针必须活过 close_page()。下一次 open()
 * 会整组覆盖。 */
static pk_keyboard_commit_fn s_on_commit;
static pk_keyboard_cancel_fn s_on_cancel;

/* 按下时的坐标，松手时才判定——与 settings/list/diag 三页同一套。
 * 键盘不滚动，所以没有 drag 与位移阈值：手指落在哪个键上，抬起就是哪个键。 */
static int  s_press_x, s_press_y;
static bool s_press_valid;

/* ── 配色 ──────────────────────────────────────────────────────── */

static uint16_t col_bg(void)      { return pk_rgb565(7, 10, 16); }
static uint16_t col_key(void)     { return pk_rgb565(28, 36, 48); }
static uint16_t col_key_txt(void) { return pk_rgb565(235, 240, 248); }
static uint16_t col_accent(void)  { return pk_rgb565(0, 110, 200); }
static uint16_t col_dim(void)     { return pk_rgb565(120, 130, 145); }

/* ── 绘制 ──────────────────────────────────────────────────────── */

/* 一枚按钮：圆角底 + 居中文字。返回值无用，写成 void 让调用点读起来是一串
 * 平铺的绘制语句。 */
static void draw_button(uint16_t *fb, int x0, int y0, int w, int h,
                        const char *label, uint16_t bg, uint16_t fg,
                        pk_aa_size_t size)
{
    pk_pfd_fill_round_rect(fb, x0, y0, x0 + w, y0 + h, 10, bg);
    /* 宽度必须过 pk_aa_text_width：中文是多字节，strlen 数的是字节，
     * 「取消」两个字用 strlen 算出来是 6 个字符宽，会把文字推出按钮左边。 */
    const int tw = pk_aa_text_width(label, size);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               x0 + (w - tw) / 2, y0 + (h - pk_aa_cell_h(size)) / 2,
               label, fg, size);
}

static void draw_input_box(uint16_t *fb)
{
    /* 边框 = 外层圆角矩形叠一层内缩的底色，pfd_draw 里没有描边接口，
     * 两次填充比新增一个只有这里用的原语划算。 */
    pk_pfd_fill_round_rect(fb, KBD_BOX_X0, KBD_BOX_Y0, KBD_BOX_X1, KBD_BOX_Y1,
                           10, col_accent());
    pk_pfd_fill_round_rect(fb, KBD_BOX_X0 + 2, KBD_BOX_Y0 + 2,
                           KBD_BOX_X1 - 2, KBD_BOX_Y1 - 2, 8, col_key());

    const int ty = KBD_BOX_Y0 + (KBD_BOX_H - PK_AA_M_H) / 2;
    int caret_x = KBD_TEXT_X;
    if (s_len > 0) {
        caret_x += pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                              KBD_TEXT_X, ty, s_text, col_key_txt(), PK_AA_M);
    }

    /*
     * 光标常亮，不闪。
     *
     * 渲染循环实测约 10 fps，闪烁周期只能摊到一两帧上——屏上看着不像光标在
     * 闪，像画面在掉帧。而这一页是模态的，用户手指就在键盘上，不需要靠闪烁
     * 提示「这里能输入」。
     */
    pk_pfd_fill_rect(fb, caret_x + 2, ty, caret_x + 5, ty + PK_AA_M_H,
                     col_accent());

    /* 右侧计数器。满了转成警示色——上限是硬的（广播包字节数），到顶时必须
     * 让用户看出来是「不让再输」，而不是「屏幕没反应」。 */
    char cnt[16];
    snprintf(cnt, sizeof(cnt), "%d/%d", s_len, s_max_len);
    const int cw = pk_aa_text_width(cnt, PK_AA_S);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               KBD_BOX_X1 - 16 - cw, KBD_BOX_Y0 + (KBD_BOX_H - PK_AA_S_H) / 2,
               cnt, s_len >= s_max_len ? pk_rgb565(255, 180, 60) : col_dim(),
               PK_AA_S);
}

/* 一枚键的几何。row/col 给命中区（整格），视觉再各内缩 KBD_KEY_PAD。 */
static void key_rect(int row, int col, int *x0, int *y0)
{
    *x0 = col * KBD_KEY_W;
    *y0 = KBD_GRID_TOP + row * KBD_KEY_H;
}

static void draw_grid(uint16_t *fb)
{
    for (int r = 0; r < KBD_ROW_CNT; ++r) {
        for (int c = 0; c < KBD_COLS; ++c) {
            int x0, y0;
            key_rect(r, c, &x0, &y0);
            const char k = KBD_KEYS[r][c];

            const char *label;
            char one[2] = { k, '\0' };
            pk_aa_size_t size = PK_AA_M;
            uint16_t bg = col_key();

            if (k == KBD_K_DEL) {
                label = pk_i18n_text(PK_TR_KBD_DELETE);
                size  = PK_AA_S;
                bg    = pk_rgb565(52, 40, 40);   /* 破坏性操作，暖色底 */
            } else if (k == KBD_K_OK) {
                label = pk_i18n_text(PK_TR_KBD_OK);
                size  = PK_AA_S;
                bg    = col_accent();            /* 主操作，与 FAB 同色 */
            } else {
                label = one;
            }

            draw_button(fb, x0 + KBD_KEY_PAD, y0 + KBD_KEY_PAD,
                        KBD_KEY_W - 2 * KBD_KEY_PAD,
                        KBD_KEY_H - 2 * KBD_KEY_PAD,
                        label, bg, col_key_txt(), size);
        }
    }
}

void pk_keyboard_page_render(uint16_t *fb)
{
    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, col_bg());

    /* 页首：标题走各页统一的标题字号/配色，切进来时顶部这条带不跳。 */
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, PK_UI_PAD_L,
               (KBD_HDR_H - PK_UI_TITLE_H) / 2, s_title,
               PK_UI_TITLE_COL, PK_UI_TITLE_SIZE);

    draw_button(fb, KBD_CLEAR_X0, KBD_BTN_Y0, KBD_BTN_W, KBD_BTN_H,
                pk_i18n_text(PK_TR_KBD_CLEAR), col_key(), col_key_txt(),
                PK_AA_S);
    draw_button(fb, KBD_CANCEL_X0, KBD_BTN_Y0, KBD_BTN_W, KBD_BTN_H,
                pk_i18n_text(PK_TR_KBD_CANCEL), pk_rgb565(52, 40, 40),
                col_key_txt(), PK_AA_S);

    draw_input_box(fb);

    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, KBD_TEXT_X, KBD_HINT_Y,
               pk_i18n_text(PK_TR_KBD_CHARSET_HINT), col_dim(), PK_AA_XS);

    draw_grid(fb);
}

/* ── 打开 / 关闭 ───────────────────────────────────────────────── */

static void close_page(void)
{
    s_active      = false;
    s_press_valid = false;
    /*
     * FAB 收回来：它在编辑期间是藏着的（见 open）。
     *
     * 2026-08-04：这里原来是无条件 set_fab_hidden(false)，在"设置页 → 键盘"
     * 那条唯一的老链路上是对的，键盘底下就是设置页。但键盘也能从**搜索页**
     * 的查询行打开，那时按「确定」/「取消」露出来的是搜索页——一枚点不动的
     * 悬浮球就浮在结果列表上。判据改由 pk_ui_fab_hidden_for 统一给
     * （apt_detail_page.h）。
     */
    pk_ui_fab_sync();
}

void pk_keyboard_page_open(const char *title, const char *initial, int max_len,
                           pk_keyboard_commit_fn on_commit,
                           pk_keyboard_cancel_fn on_cancel)
{
    s_title     = (title != NULL) ? title : "";
    s_on_commit = on_commit;
    s_on_cancel = on_cancel;
    s_max_len = (max_len < 1) ? 1
              : (max_len > PK_KBD_TEXT_MAX ? PK_KBD_TEXT_MAX : max_len);

    s_len = 0;
    s_text[0] = '\0';
    if (initial != NULL) {
        for (const char *p = initial; *p && s_len < s_max_len; ++p)
            s_text[s_len++] = *p;
        s_text[s_len] = '\0';
    }

    s_press_valid = false;
    s_active      = true;

    /*
     * 编辑期间藏掉 FAB。
     *
     * 它默认落在 x≥728、垂直位置还能被用户拖到任意高度，正压在键盘最右一列
     * （「删除」「确定」两枚键）上。而 touch_gt911 里页面的自绘命中排在 LVGL
     * 之前——不藏的话，FAB 会被键盘挡得点不动，同时它半透明的圆盘还盖在
     * 「确定」上，是两头都不讨好。
     *
     * 藏掉之后出口仍有两个：页首「取消」与键盘右下角「确定」，都是屏上写着
     * 字的实心按钮，比一个含义随页面变化的圆钮更不会让人困住。
     */
    pk_ui_fab_sync();   /* s_active 已置真 → 必然算成"藏" */
}

bool pk_keyboard_page_active(void) { return s_active; }

/* ── 触摸 ──────────────────────────────────────────────────────── */

bool pk_keyboard_page_touch(int x, int y)
{
    if (!s_active) return false;
    s_press_x     = x;
    s_press_y     = y;
    s_press_valid = true;
    /* 整屏都吃：模态编辑器底下没有任何该被点到的东西。 */
    return true;
}

/* 往缓冲里追加一个字符。到顶就什么都不做——计数器已经转成警示色，
 * 这里再弹一次提示反而打断连续输入。 */
static void text_append(char c)
{
    if (s_len >= s_max_len) return;
    s_text[s_len++] = c;
    s_text[s_len]   = '\0';
}

void pk_keyboard_page_touch_up(void)
{
    if (!s_active || !s_press_valid) { s_press_valid = false; return; }
    s_press_valid = false;

    const int x = s_press_x, y = s_press_y;

    /* 页首两个按钮。命中区用整条 76 px 的带，视觉只有 48——与 settings_draw
     * 「视觉按 spec、命中放宽到整行」是同一条规矩。 */
    if (y < KBD_HDR_H) {
        if (x >= KBD_CANCEL_X0 && x < KBD_CANCEL_X0 + KBD_BTN_W) {
            const pk_keyboard_cancel_fn cb = s_on_cancel;
            close_page();
            if (cb != NULL) cb();
        } else if (x >= KBD_CLEAR_X0 && x < KBD_CLEAR_X0 + KBD_BTN_W) {
            s_len = 0;
            s_text[0] = '\0';
        }
        return;
    }

    if (y < KBD_GRID_TOP) return;      /* 输入框那条带不响应点击 */

    const int row = (y - KBD_GRID_TOP) / KBD_KEY_H;
    const int col = x / KBD_KEY_W;
    if (row < 0 || row >= KBD_ROW_CNT || col < 0 || col >= KBD_COLS) return;

    const char k = KBD_KEYS[row][col];
    if (k == KBD_K_DEL) {
        if (s_len > 0) s_text[--s_len] = '\0';
    } else if (k == KBD_K_OK) {
        /* 先关页再回调：回调里可能会重开 BLE 广播、写 NVS，那期间渲染任务
         * 已经切回设置页，不会再去读这边正在被改的缓冲。 */
        char out[PK_KBD_TEXT_MAX + 1];
        memcpy(out, s_text, (size_t)s_len + 1);
        const pk_keyboard_commit_fn cb = s_on_commit;
        close_page();
        if (cb != NULL) cb(out);
    } else {
        text_append(k);
    }
}
