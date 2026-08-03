/*
 * cal_wizard.c — 磁力计（BNO085）校准向导：画 8 字提示页。
 *
 * 图 8 的轨迹是 Bernoulli 双纽线：
 *
 *     x(t) = a · cos(t) / (1 + sin²(t))
 *     y(t) = a · sin(t)·cos(t) / (1 + sin²(t))
 *
 * 静态轮廓画暗，上面跑一个亮点，3 s 一圈——用户跟着这个点比着手比"读一段
 * 文字描述该怎么动"直观得多。
 *
 * 版面（800×480）
 * ---------------
 *   y=0    ┌───────────────────────────────────────────────┐
 *          │ 罗盘校准                                       │ 顶栏 48
 *   y=48   ├───────────────────────────────────────────────┤
 *          │                  ╭──╮╭──╮                     │
 *          │                  ╰──╳──╯   ● 动点              │ 图 8：64…242
 *          │                                               │
 *   y=258  │              画 8 字移动设备                    │ 说明行 1（M）
 *   y=290  │               各方向旋转                        │ 说明行 2（M）
 *   y=330  │                质量  1 / 3                     │ 精度数值（M）
 *   y=364  │        ▓▓▓▓▓▓▓▓░░░░░░░░░░░░░░░░░░░░           │ 进度条 400×24
 *   y=408  │ 校准后航向更准                    [ 稍后再说 ]   │ 页脚 + 关闭按钮
 *   y=480  └───────────────────────────────────────────────┘
 *
 * 坐标不是拍脑袋定的，两条来源：
 *
 *   1. **页面骨架照抄 about_page.c / diag_page.c**：顶栏高 PFD_BAR_BOT(48)、
 *      标题落在 PK_UI_PAD_L × PK_UI_TITLE_Y、栏底 2 px 分隔线、正文字号
 *      PK_UI_ITEM_SIZE(M=18 px 视觉)。切页时标题不该跳位置、不该变大小。
 *   2. **按钮尺寸照抄 traffic_page.c**：56 px 高、距屏幕边 16 px、命中区
 *      再外扩 12 px（凑够 ~9 mm 的手指目标，见那边 BTN_HIT_PAD 的注释）。
 *
 * 图 8 的半径是从"上下留白"倒推的，不是试出来的：上沿 = 顶栏下沿 + 16 = 64，
 * 下沿 = 说明文字上沿 − 16 = 242，竖直半幅 89 px；双纽线的 y 峰值恒为
 * R·VSCALE·0.3536，反解得 R = 89 / (1.4 × 0.3536) ≈ 180。
 *
 * 2026-08-03 之前这一页还是 320×240 时代的绝对坐标（CY=90 / R=50 /
 * INSTR_Y=162 / BAR_Y=190），换屏后全部内容挤在上方 200 px 里、下面大片
 * 空白——那就是"界面是乱的"的由来。同一次改动里字体也从 text.h 那套
 * （8 px 位图 CJK + 5×7 缩放拉丁）换成了 pfd_aa_text 的 pk_aa_puts，与其余
 * 五页同一份抗锯齿字体：小屏那套在 217 PPI 上放大后是方块像素。
 */

#include "cal_wizard.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_timer.h"

#include "display.h"
#include "i18n.h"
#include "pfd_aa_font.h"
#include "pfd_aa_text.h"
#include "pfd_layout.h"
#include "ui_state.h"

/* ── 版面 ──────────────────────────────────────────────────────── */
#define WZ_HEADER_H          PFD_BAR_BOT      /* 48，与其余整屏页等高 */

#define WZ_FIG8_CX           (PK_DISPLAY_W / 2)      /* 400 */
#define WZ_FIG8_CY           153                     /* = 64 + 89（见文件头） */
#define WZ_FIG8_R            180                     /* 横向半幅(px) */
#define WZ_FIG8_VSCALE       1.4f                    /* 竖向拉伸，y 峰 ±89 px */
/* 720 个采样点：实测相邻两点最大间距 2.2 px（240 点时是 6.6 px，放到 R=180
 * 上轮廓会断成虚线），配 3×3 的笔触正好连续。每帧 720 次 sinf/cosf 在 P4 上
 * 约 0.2 ms，10 FPS 下可以忽略——注意这跟"整圆用 draw_line_aa"那个性能陷阱
 * 不是一回事，这里每点只写 9 个像素，没有抗锯齿混合。 */
#define WZ_FIG8_OUTLINE_PTS  720
#define WZ_FIG8_PERIOD_US    3000000     /* 一圈 3 s */
#define WZ_DOT_R             9           /* 动点半径：56 px 按钮的 1/3，远处也认得出 */

#define WZ_INSTR1_Y          258
#define WZ_INSTR2_Y          (WZ_INSTR1_Y + PK_AA_M_H + 6)   /* 290 */

#define WZ_QUAL_Y            330                             /* 质量数值（M） */
#define WZ_BAR_W             400
#define WZ_BAR_H             24
#define WZ_BAR_X             ((PK_DISPLAY_W - WZ_BAR_W) / 2) /* 200 */
#define WZ_BAR_Y             364

/* 底部动作行：左边页脚提示，右下角「稍后再说」。
 *
 * 为什么按钮不居中：这是一条"次要动作"，居中会和它上面居中的进度条抢视觉
 * 中轴；右下角是对话框里放次要动作的老位置，而左边那句提示正好填掉右对齐
 * 留下的空白，一行干两件事，省下一整行的垂直空间（480 px 真的不够铺）。
 *
 * 不为 FAB 让路——FAB 浮在内容上是全局产品原则。默认位置在 x[728,784]
 * y[312,368]（pk_ui_nav.c 的 FAB_DEFAULT_Y），与这一行 y[408,464] 不重叠。 */
#define WZ_BTN_W             168
#define WZ_BTN_H             56
#define WZ_BTN_M             16          /* 距屏幕边，同 traffic_page 的 BTN_M */
#define WZ_BTN_HIT_PAD       12          /* 命中外扩，同 traffic_page 的 BTN_HIT_PAD */
#define WZ_BTN_RADIUS        14
#define WZ_BTN_X             (PK_DISPLAY_W - WZ_BTN_M - WZ_BTN_W)   /* 616 */
#define WZ_BTN_Y             (PK_DISPLAY_H - WZ_BTN_M - WZ_BTN_H)   /* 408 */

/* 页脚与按钮共用这一行，垂直居中对齐。 */
#define WZ_FOOT_X            PK_UI_PAD_L
#define WZ_FOOT_Y            (WZ_BTN_Y + (WZ_BTN_H - PK_AA_S_H) / 2)

/* ── 配色 ────────────────────────────────────────────────────────
 * 底色/分隔线取 about_page 的同一组；进度条三档直接用诊断页的
 * COL_ALERT / COL_WARN / COL_ONLINE 三色——"红黄绿"在这台设备上是同一套
 * 语义，这一页原来自带一份略有出入的近似色，切页时能看出色差。 */
#define COL_BG               pk_rgb565( 12,  12,  16)
#define COL_DIVIDER          pk_rgb565( 60,  70,  86)
#define COL_OUTLINE          pk_rgb565( 70,  82, 102)
#define COL_DOT              pk_rgb565(255, 215,   0)
#define COL_INSTR            pk_rgb565(255, 255, 255)
#define COL_INSTR_DIM        pk_rgb565(150, 170, 195)
#define COL_BAR_BG           pk_rgb565( 30,  38,  50)
#define COL_BAR_LOW          pk_rgb565(255,  90,  40)   /* = diag COL_ALERT  */
#define COL_BAR_MID          pk_rgb565(255, 176,   0)   /* = diag COL_WARN   */
#define COL_BAR_HIGH         pk_rgb565( 80, 220,  80)   /* = diag COL_ONLINE */

static void fill_rect(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > PK_DISPLAY_W) x1 = PK_DISPLAY_W;
    if (y1 > PK_DISPLAY_H) y1 = PK_DISPLAY_H;
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = fb + y * PK_DISPLAY_W;
        for (int x = x0; x < x1; ++x) row[x] = c;
    }
}

/* 实心圆：逐行扫描，不用 draw_line_aa 围一圈（那条路在地图页上把帧率打到
 * 5 FPS，见 arc_aa 那次的结论）。 */
static void fill_circle(uint16_t *fb, int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; ++dy) {
        const int y = cy + dy;
        if (y < 0 || y >= PK_DISPLAY_H) continue;
        const int half = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
        fill_rect(fb, cx - half, y, cx + half + 1, y + 1, c);
    }
}

/* 该像素是否落在圆角矩形内。只在四角做圆检测，其余直接通过——照抄
 * about_page.c 的 in_rounded_rect（那边给 logo 底板用）。 */
static bool in_rounded_rect(int col, int row, int w, int h, int r)
{
    int dx = 0, dy = 0;
    if (col < r)              dx = r - col;
    else if (col >= w - r)    dx = col - (w - r - 1);
    if (row < r)              dy = r - row;
    else if (row >= h - r)    dy = row - (h - r - 1);
    if (dx == 0 || dy == 0) return true;
    return dx * dx + dy * dy <= r * r;
}

/* 圆角按钮底板 + 2 px 描边。配色照抄 traffic_page.c 的 draw_btn_plate：
 * 按住时底色提亮一档，这是按钮唯一的"我收到了"信号（10 FPS 下没有这个反馈，
 * 点下去会像是没反应）。 */
static void draw_btn_plate(uint16_t *fb, int x, int y, int w, int h, bool down)
{
    const uint16_t face = down ? pk_rgb565( 62,  84, 112)
                               : pk_rgb565( 22,  30,  42);
    const uint16_t edge = down ? pk_rgb565(210, 228, 245)
                               : pk_rgb565(120, 145, 175);
    for (int row = 0; row < h; ++row) {
        const int yy = y + row;
        if (yy < 0 || yy >= PK_DISPLAY_H) continue;
        uint16_t *dst = fb + yy * PK_DISPLAY_W;
        for (int col = 0; col < w; ++col) {
            const int xx = x + col;
            if (xx < 0 || xx >= PK_DISPLAY_W) continue;
            if (!in_rounded_rect(col, row, w, h, WZ_BTN_RADIUS)) continue;
            const bool border = !in_rounded_rect(col - 2, row - 2,
                                                 w - 4, h - 4,
                                                 WZ_BTN_RADIUS - 2);
            dst[xx] = border ? edge : face;
        }
    }
}

/* 居中画一行文本。整页除标题外全部走它——标题按全局规矩左对齐到
 * PK_UI_PAD_L（pfd_layout.h），不居中。 */
static void puts_center(uint16_t *fb, int y, const char *s,
                        uint16_t color, pk_aa_size_t size)
{
    const int w = pk_aa_text_width(s, size);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               (PK_DISPLAY_W - w) / 2, y, s, color, size);
}

static void fig8_point(float t, int *x, int *y)
{
    const float sin_t = sinf(t);
    const float cos_t = cosf(t);
    const float denom = 1.0f + sin_t * sin_t;
    *x = (int)(WZ_FIG8_CX + WZ_FIG8_R * cos_t / denom + 0.5f);
    *y = (int)(WZ_FIG8_CY + WZ_FIG8_R * sin_t * cos_t / denom *
                WZ_FIG8_VSCALE + 0.5f);
}

static void draw_outline(uint16_t *fb)
{
    for (int i = 0; i < WZ_FIG8_OUTLINE_PTS; ++i) {
        const float t = (float)i * 2.0f * (float)M_PI / WZ_FIG8_OUTLINE_PTS;
        int x, y;
        fig8_point(t, &x, &y);
        fill_rect(fb, x - 1, y - 1, x + 2, y + 2, COL_OUTLINE);
    }
}

static void draw_animated_dot(uint16_t *fb)
{
    const int64_t t_us = esp_timer_get_time();
    const float phase = (float)((t_us % WZ_FIG8_PERIOD_US)) /
                        (float)WZ_FIG8_PERIOD_US;
    int x, y;
    fig8_point(phase * 2.0f * (float)M_PI, &x, &y);
    fill_circle(fb, x, y, WZ_DOT_R, COL_DOT);
}

static void draw_progress_bar(uint16_t *fb, uint8_t accuracy)
{
    /* 数值先行：一眼看到"几分之三"，条只是把它可视化。 */
    char buf[48];
    snprintf(buf, sizeof(buf), "%s  %u / 3",
             pk_i18n_text(PK_TR_CAL_QUALITY), accuracy);
    puts_center(fb, WZ_QUAL_Y, buf, COL_INSTR, PK_UI_ITEM_SIZE);

    const uint16_t fill_col = (accuracy >= 2) ? COL_BAR_HIGH
                            : (accuracy >= 1) ? COL_BAR_MID
                            :                   COL_BAR_LOW;

    /* 边框也吃档位色。
     *
     * 只染填充的话，「低橙」这一档**永远画不出来**：acc=0 时 filled_w=0，
     * 条子是全空的，屏上只剩一条深灰。于是这台设备最要紧的那个状态——
     * "一点都没校准"——反而是唯一没有颜色的状态，看起来像进度条坏了。
     * 边框吃色之后 acc=0 是"橙色空框"，acc=3 是"绿色实条"，四档都有色。 */
    fill_rect(fb, WZ_BAR_X, WZ_BAR_Y,
              WZ_BAR_X + WZ_BAR_W, WZ_BAR_Y + WZ_BAR_H, fill_col);
    fill_rect(fb, WZ_BAR_X + 2, WZ_BAR_Y + 2,
              WZ_BAR_X + WZ_BAR_W - 2, WZ_BAR_Y + WZ_BAR_H - 2, COL_BAR_BG);

    const int filled_w = (WZ_BAR_W - 4) * (int)accuracy / 3;
    if (filled_w > 0) {
        fill_rect(fb, WZ_BAR_X + 2, WZ_BAR_Y + 2,
                  WZ_BAR_X + 2 + filled_w, WZ_BAR_Y + WZ_BAR_H - 2,
                  fill_col);
    }
}

/* 「稍后再说」当前被按住？按下置位、松手清除，同 traffic_page 的 s_btn_down。 */
static bool s_btn_down;

static void draw_later_button(uint16_t *fb)
{
    draw_btn_plate(fb, WZ_BTN_X, WZ_BTN_Y, WZ_BTN_W, WZ_BTN_H, s_btn_down);

    const char *label = pk_i18n_text(PK_TR_CAL_LATER);
    const int   lw    = pk_aa_text_width(label, PK_UI_ITEM_SIZE);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               WZ_BTN_X + (WZ_BTN_W - lw) / 2,
               WZ_BTN_Y + (WZ_BTN_H - pk_aa_cell_h(PK_UI_ITEM_SIZE)) / 2,
               label, pk_rgb565(225, 235, 248), PK_UI_ITEM_SIZE);
}

void pk_cal_wizard_render(uint16_t *fb)
{
    /* 整屏清底：这一页独占屏幕，底下那一页一个像素都不该露出来。 */
    fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, COL_BG);

    draw_outline(fb);
    draw_animated_dot(fb);

    puts_center(fb, WZ_INSTR1_Y, pk_i18n_text(PK_TR_CAL_LINE1),
                COL_INSTR, PK_UI_ITEM_SIZE);
    puts_center(fb, WZ_INSTR2_Y, pk_i18n_text(PK_TR_CAL_LINE2),
                COL_INSTR, PK_UI_ITEM_SIZE);

    draw_progress_bar(fb, pk_ui_cal_wizard_last_accuracy());

    /* 页脚：讲"为什么值得校准"。原文是"按 MODE 跳过"，4.3″ 板上根本没有
     * MODE 键——跳过改由右边那枚按钮承担（见 i18n_catalog.py 的 CAL_FOOTER）。 */
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, WZ_FOOT_X, WZ_FOOT_Y,
               pk_i18n_text(PK_TR_CAL_FOOTER), COL_INSTR_DIM, PK_AA_S);
    draw_later_button(fb);

    /* 顶栏最后画：图 8 的上沿离它只有 16 px，先画顶栏的话轮廓会压在标题上。 */
    fill_rect(fb, 0, 0, PK_DISPLAY_W, WZ_HEADER_H - 2, COL_BG);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               PK_UI_PAD_L, PK_UI_TITLE_Y, pk_i18n_text(PK_TR_CAL_TITLE),
               PK_UI_TITLE_COL, PK_UI_TITLE_SIZE);
    fill_rect(fb, 0, WZ_HEADER_H - 2, PK_DISPLAY_W, WZ_HEADER_H, COL_DIVIDER);
}

/* ── 触摸 ──────────────────────────────────────────────────────────
 *
 * 与 touch_gt911.c 的约定同其余各页：返回 true 表示这一下被本页吃掉。归属
 * 由 pk_touch_arbiter 在按下那一刻定死，本页不做每帧重判（那是"拖 FAB 被
 * 列表抢走"那个老 bug 的成因，见 pk_touch_arbiter.h）。
 *
 * 只有「稍后再说」一个命中区；其余落点一律放行，好让 FAB → 导航网格这条
 * 常规退路照常可用。 */
bool pk_cal_wizard_touch(int x, int y)
{
    const int x0 = WZ_BTN_X - WZ_BTN_HIT_PAD;
    const int y0 = WZ_BTN_Y - WZ_BTN_HIT_PAD;
    const int x1 = WZ_BTN_X + WZ_BTN_W + WZ_BTN_HIT_PAD;
    const int y1 = WZ_BTN_Y + WZ_BTN_H + WZ_BTN_HIT_PAD;
    if (x < x0 || x >= x1 || y < y0 || y >= y1) return false;

    s_btn_down = true;
    /* 关页 **并** 抑制自动重弹。只关页的话 10 s 后 pk_ui_cal_wizard_tick()
     * 会把用户原样拽回来——那等于这个按钮没做（成因与抑制策略见 ui_state.c
     * 里 s_cal_auto_suppressed 的注释）。 */
    pk_ui_cal_wizard_dismiss();
    return true;
}

void pk_cal_wizard_touch_up(void)
{
    s_btn_down = false;
}
