/*
 * pfd_attitude.c — attitude indicator (sky/ground horizon with vertical
 * gradient + pitch ladder + bank arc + chevron pointer + sky pointer +
 * yellow wing reticle).
 *
 * Geometry follows the G1000-style layout spec (§3): the attitude
 * region occupies x ∈ [50, 248), y ∈ [18, 138) — 198 × 120 pixels,
 * sitting between the top status bar and the bottom HSI region. All
 * rotations happen around the region's geometric center (149, 78).
 *
 * The bank arc is drawn with a virtual center placed *below* the
 * visible region (149, 200) so the visible portion of the arc curves
 * cleanly across the top of the attitude box. Same trick the old
 * portrait code used, just with smaller radius and re-centered.
 *
 * The sky/ground colors use a perpendicular-distance LUT for the
 * G1000 gradient — brightest at the horizon, darker further away —
 * so the band texture stays correct under bank.
 */

#include "pfd_attitude.h"

#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"

#include "display.h"
#include "pfd_layout.h"
#include "pfd_aa_text.h"
#include "pfd_draw.h"
#include "pfd_font.h"

/* --- Layout constants ----------------------------------------------- *
 *
 * The attitude indicator fills the WHOLE panel below the top status
 * bar — Garmin G1000 treats it as the screen background, not a small
 * sub-region. Other widgets (statusbar / ALT tape / HSI / GS / VS)
 * draw OVER the attitude as opaque overlays.
 */
#define PFD_ATTITUDE_LEFT       PFD_ATT_LEFT
#define PFD_ATTITUDE_RIGHT      PK_DISPLAY_W
/* 天地背景一直填到屏幕底，而不是止于姿态区下沿：下面的 HSI 罗盘是半透明的，
 * 底下没有天地就是一片未初始化的 framebuffer。
 * 注意这**不等于**几何中心的取法 —— PFD_CX/CY 来自 pfd_layout.h，按姿态区
 * 算；此前这里用 (18 + PK_DISPLAY_H)/2 把两件事混为一谈，导致地平线下沉。 */
#define PFD_ATTITUDE_TOP        PFD_ATT_TOP
#define PFD_ATTITUDE_BOT        PK_DISPLAY_H

/* Bank arc: virtual center placed below the visible region so the
 * arc curves cleanly across the top of the attitude indicator. With
 * R=100 and CY=130, the 0° tick lands at y=30 (just below the
 * statusbar) and ±60° ticks land at x=160±87 → arc spans ~174 px
 * wide ≈ 54% of the 320-px panel (Garmin G1000 keeps the bank
 * indicator inside the middle 1/3..3/5 of screen width). */
#define BANK_ARC_CX             PFD_BANK_ARC_CX
#define BANK_ARC_CY             PFD_BANK_ARC_CY
#define BANK_ARC_R              PFD_BANK_ARC_R

/* Gradient LUT size: max perpendicular distance from horizon we map
 * to distinct colors. Beyond this the gradient saturates at the "far"
 * end. 160 keeps the gradient ramp visible across the full screen. */
#define ATTITUDE_HEIGHT         PFD_ATT_H

/* --- Palette (spec §4, RGB565, panel byte order) ------------------- */
#define COL_HORIZON_LINE        pk_rgb565(255, 255, 255)
#define COL_RETICLE             pk_rgb565(255, 255,   0)   /* pure yellow */
#define COL_PITCH_LINE          pk_rgb565(255, 255, 255)
#define COL_BANK_TICK           pk_rgb565(255, 255, 255)
#define COL_BANK_POINTER        pk_rgb565(255, 255,   0)   /* pure yellow */
#define COL_SKY_POINTER         pk_rgb565(255, 255, 255)
#define COL_BANK_ARC            pk_rgb565(255, 255, 255)

/* ── 俯仰梯度尺寸 ──────────────────────────────────────────────
 *
 * 半宽原值 35/24/16 是照 320 屏姿态区（约 170 px 宽）定的。800 屏姿态区
 * 有 600 px，照搬会让梯度线短得像三道划痕，与 G1000 上「±10° 线约占姿态
 * 区三分之一」的观感差得远，故按区宽等比放大。
 *
 * 标签同样从 5×7 位图 scale-1（6 px）换成 S 档 —— 6 px 在 4.3″ 屏上只有
 * 0.7 mm，低于 spec §2 的 18 px 硬下限。 */
#if PK_DISPLAY_W >= 800
/* ±10° 线全宽约占姿态区宽度的 22%（与 320 上 70/320 的观感一致）：
 * 800 姿态区宽 600 → 132 px 全宽 → 半宽 66，其余两档按 35:24:16 同比。 */
#  define LADDER_W10        66
#  define LADDER_W20        45
#  define LADDER_W30        30
#  define LADDER_PUTS(fb, x, y, s, col) \
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), PK_AA_M)
#  define LADDER_LBL_W      PK_AA_M_W
#  define LADDER_LBL_H      PK_AA_M_H
#  define LADDER_LBL_GAP    8
/* 梯度线的下界是**航向框顶**，不是罗盘顶：框坐在罗盘正上方、姿态区下沿，
 * 是这一侧最先挡路的东西。画到框上只会两层叠字。 */
#  define LADDER_TOP        (BANK_ARC_CY - BANK_ARC_R + 4)
#  define LADDER_BOT        (PFD_HDGBOX_Y0 - 10)
/* 坡度刻度长度 / 天空指针 / chevron 同样按物理尺寸对齐 320（×1.3），
 * 而不是按面板像素等比（×2.5）—— 后者会让这些符号在新屏上大得突兀。 */
#  define BANK_TICK_S        5
#  define BANK_TICK_M        8
#  define BANK_TICK_L       13
#  define SKYPTR_H          14
#  define SKYPTR_HW         10
#  define CHEVRON_LEN       16
#  define CHEVRON_HW         8
/* 坡度弧比俯仰梯度更粗：它是姿态的**参考框架**，梯度是框架内的刻度，
 * 主次要一眼分得出。 */
#  define BANK_ARC_TH      2.8f
#else
#  define LADDER_W10        35
#  define LADDER_W20        24
#  define LADDER_W30        16
#  define LADDER_PUTS(fb, x, y, s, col) \
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), 1)
#  define LADDER_LBL_W      6
#  define LADDER_LBL_H      6
#  define LADDER_LBL_GAP    2
#  define LADDER_BOT        (PFD_ATTITUDE_BOT + 20)
#  define BANK_TICK_S        4
#  define BANK_TICK_M        6
#  define BANK_TICK_L       10
#  define SKYPTR_H          12
#  define SKYPTR_HW          6
#  define CHEVRON_LEN       12
#  define CHEVRON_HW         6
#  define BANK_ARC_TH      2.0f
#endif


/* --- Gradient LUTs (sky/ground), built once on first render -------- */

/* 梯度查找表留在 PSRAM。
 *
 * 试过搬进内部 RAM——它每像素被查一次，直觉上该放快介质。结果是开机即挂：
 *
 *     assert failed: vApplicationGetIdleTaskMemory port_common.c:53
 *     (pxStackBufferTemp != NULL)
 *
 * 这两张表 18.7 KB，而内部 RAM 在 PSRAM/USB/DSI/BLE 都起来之后已经紧到
 * 连 FreeRTOS idle task 的栈都分不出。放 PSRAM 的实际代价没有想象中大：
 * idx 沿 x 是缓慢单调变化的，命中的都是同一段 cache line。 */
/* 维度是 [idx][cell]，不是 [cell][idx]——顺序反了会慢一截。
 *
 * 内层循环沿 x 走时，idx（到地平线的距离）变化很慢，而 cell 每个像素都在
 * 0..3 之间轮转（cell = (y&3)<<2 | (x&3)）。按 [cell][idx] 存的话，相邻两个
 * 像素要读的两项相隔 ATTITUDE_HEIGHT×2 = 584 字节，每 4 个像素就踩 4 条不同
 * 的 cache line。调换之后同一 idx 的 16 个 cell 连续排布，只占 32 字节，
 * 一条 64 字节的 line 就全装下了。 */
static EXT_RAM_BSS_ATTR uint16_t s_sky_grad[ATTITUDE_HEIGHT][16];
static EXT_RAM_BSS_ATTR uint16_t s_ground_grad[ATTITUDE_HEIGHT][16];
static bool s_grad_built = false;

static uint8_t blend_u8(uint8_t a, uint8_t b, int t)
{
    return (uint8_t)(((int)a * (256 - t) + (int)b * t) >> 8);
}

static void build_gradient_luts(void)
{
    if (s_grad_built) return;
    for (int cell = 0; cell < 16; ++cell) {
        int dx = cell & 3;
        int dy = cell >> 2;
        for (int i = 0; i < ATTITUDE_HEIGHT; ++i) {
            int t = (i * 256) / (ATTITUDE_HEIGHT - 1);   /* 0 near, 256 far */
            uint8_t sky_r = blend_u8( 35,  15, t);
            uint8_t sky_g = blend_u8(145,  70, t);
            uint8_t sky_b = blend_u8(235, 140, t);
            uint8_t gnd_r = blend_u8(170,  95, t);
            uint8_t gnd_g = blend_u8(125,  65, t);
            uint8_t gnd_b = blend_u8( 80,  35, t);
            s_sky_grad[i][cell] = pk_pfd_rgb565_dither(sky_r, sky_g, sky_b, dx, dy);
            s_ground_grad[i][cell] = pk_pfd_rgb565_dither(gnd_r, gnd_g, gnd_b, dx, dy);
        }
    }
    s_grad_built = true;
}

/* 一整行的暂存，放内部 RAM（1.6 KB）。
 *
 * 为什么值得多一次搬运：往 PSRAM 写一个 cache line 之前，硬件要先把它读进来
 * （write-allocate），于是 768 KB 的逐像素写实际变成 768 KB 读 + 768 KB 写。
 * 先在内部 RAM 把整行画完、再一次 memcpy 过去，写就变成连续的整行块传输。
 *
 * 地平线抗锯齿那几个像素也在行缓冲里混合——原来的 pk_pfd_blend_pixel() 会
 * 读改写 PSRAM，正好破坏纯写的访问模式。 */
static uint16_t s_line[PK_DISPLAY_W];

/* 面板是大端 RGB565，混合要在主机序下做。与 pfd_draw.c 里那对同名静态函数
 * 一致（都是字节交换），不值得为两行代码开一个跨模块接口。 */
static inline uint16_t swap565(uint16_t c)
{
    return (uint16_t)((c >> 8) | (c << 8));
}

/* pk_pfd_blend_pixel() 的行内版本，逻辑逐字照搬，只是目标换成行缓冲。 */
static inline void blend_in_line(uint16_t *line, int x, uint16_t c, uint8_t alpha)
{
    if (alpha == 0) return;
    if (alpha == 255) { line[x] = c; return; }

    uint16_t dst = swap565(line[x]);
    uint16_t src = swap565(c);

    int sr = (src >> 11) & 0x1F, sg = (src >> 5) & 0x3F, sb = src & 0x1F;
    int dr = (dst >> 11) & 0x1F, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
    int a = alpha, ia = 255 - a;
    int r = (sr * a + dr * ia + 127) / 255;
    int g = (sg * a + dg * ia + 127) / 255;
    int b = (sb * a + db * ia + 127) / 255;
    line[x] = swap565((uint16_t)((r << 11) | (g << 5) | b));
}

/* --- Sky / ground horizon with gradient ----------------------------- */

static void draw_horizon(uint16_t *fb, float roll_deg, float pitch_deg)
{
    const float rad      = roll_deg * (float)M_PI / 180.0f;
    const float slope    = -tanf(rad);
    const float cos_roll = fabsf(cosf(rad));
    const float pitch_px = pitch_deg * PFD_PIXELS_PER_DEG;

    /* 内层循环直接递推「到地平线的垂直距离」perp，每像素只剩一次加法。
     *
     * 原来每像素要算 6 次浮点：求 hy 的乘加、求 dy 的减、乘 cos 得 perp、
     * 取整得 idx、再算 coverage。800×480 就是每帧 230 万次浮点运算。
     *
     * 但这些量沿 x 全是线性的。设 perp = (y - hy)·cos，x 每加 1 时
     * hy 加 slope = -tan(roll)，于是
     *
     *     Δperp = -slope·cos = tan(roll)·cos(roll) = sin(roll)
     *
     * 步长是个常数，而且连乘法都不需要——行首算一次，之后一路加过去。
     *
     * 递推 perp 而不是 dy，还顺手修掉了一个边界错误：clamp 必须在乘 cos
     * **之后**判断。对 dy 判断的话，大坡度时 |dy| 很大但 perp 仍在表内，
     * 会被误当成远处而 clamp 到表尾——差异图上就是左下与右上两块。
     *
     * Q12（1/4096 px）精度：单步截断误差 1/4096，一行 800 像素累积 0.2 px，
     * 落到整数 idx 上就消失了。perp 有界（屏内到地平线的垂直距离），
     * 933·4096 ≈ 3.8e6，稳稳在 int32 内。 */
    const int32_t step_fp  = (int32_t)(sinf(rad) * 4096.0f);
    const int     idx_max  = ATTITUDE_HEIGHT - 1;
    const int32_t cov_full = 6144;         /* 1.5 px in Q12 */

    for (int y = PFD_ATTITUDE_TOP; y < PFD_ATTITUDE_BOT; ++y) {
        uint16_t *row = fb + y * PK_DISPLAY_W;

        /* 行首那一个像素仍用浮点算准，之后全靠递推。 */
        const float hy_left = (float)PFD_CY + pitch_px +
                              slope * ((float)PFD_ATTITUDE_LEFT - (float)PFD_CX);
        int32_t perp_fp = (int32_t)(((float)y - hy_left) * cos_roll * 4096.0f);

        const int cell_row = (y & 3) << 2;
        for (int x = PFD_ATTITUDE_LEFT; x < PFD_ATTITUDE_RIGHT; ++x) {
            const int32_t ap = perp_fp < 0 ? -perp_fp : perp_fp;
            int idx = (int)(ap >> 12);
            if (idx > idx_max) idx = idx_max;

            const int cell = cell_row | (x & 3);
            /* perp 与 dy 同号（cos_roll ≥ 0），可以直接用它判天地。 */
            s_line[x] = (perp_fp < 0) ? s_sky_grad[idx][cell]
                                      : s_ground_grad[idx][cell];

            /* 2 px 地平线 + 1 px 抗锯齿边。绝大多数像素离地平线很远，
             * 这个整数比较直接把它们挡在外面。 */
            if (ap < cov_full) {
                const int32_t cov = cov_full - ap;           /* Q12 */
                const uint8_t alpha = cov >= 4096 ? 255
                                                  : (uint8_t)((cov * 255) >> 12);
                blend_in_line(s_line, x, COL_HORIZON_LINE, alpha);
            }
            perp_fp += step_fp;
        }
        /* 整行一次搬过去：连续块写，避开 write-allocate 的读回。 */
        memcpy(row + PFD_ATTITUDE_LEFT, s_line + PFD_ATTITUDE_LEFT,
               (size_t)(PFD_ATTITUDE_RIGHT - PFD_ATTITUDE_LEFT) * sizeof(uint16_t));
    }
}

/* --- Pitch ladder --------------------------------------------------- */

static const int8_t pitch_marks[] = {-30, -20, -10, 10, 20, 30};

static void rotate_about_center(float cs, float sn,
                                int x_in, int y_in,
                                int *x_out, int *y_out)
{
    float dx = (float)(x_in - PFD_CX);
    float dy = (float)(y_in - PFD_CY);
    *x_out = (int)(PFD_CX + cs * dx - sn * dy + 0.5f);
    *y_out = (int)(PFD_CY + sn * dx + cs * dy + 0.5f);
}

static void draw_pitch_ladder(uint16_t *fb, float roll_deg, float pitch_deg)
{
    const float rad = roll_deg * (float)M_PI / 180.0f;
    const float cs = cosf(rad);
    const float sn = sinf(rad);

    for (size_t i = 0; i < sizeof(pitch_marks) / sizeof(pitch_marks[0]); ++i) {
        int p = pitch_marks[i];
        int abs_p = p < 0 ? -p : p;
        /* 三级视觉层次：离地平线越远，越短、越细、越淡。
         *
         * 中心附近是最常用的读数区，该最实；±30° 只是余光里的方位感，压下去
         * 反而让整个姿态区不那么吵。这也是与坡度弧拉开差别的手段——两者本来
         * 同为纯白 1.4 px，在弧与梯度交叠处根本分不出谁是谁。 */
        int   half_w = (abs_p == 10) ? LADDER_W10
                     : (abs_p == 20 ? LADDER_W20 : LADDER_W30);
        float line_th = (abs_p == 10) ? 1.7f : (abs_p == 20 ? 1.4f : 1.1f);
        /* 没有 alpha 通道，用明度模拟不透明度：深色天地背景上，压暗即变淡。 */
        uint16_t lcol = (abs_p == 10) ? pk_rgb565(255, 255, 255)
                      : (abs_p == 20) ? pk_rgb565(215, 218, 225)
                                      : pk_rgb565(175, 180, 190);
        int mark_y = PFD_CY + (int)((pitch_deg - (float)p) *
                                    PFD_PIXELS_PER_DEG + 0.5f);
        /* 上界取**弧顶**而不是姿态区顶：俯仰梯度是坡度弧「框」内的刻度，跑到
         * 弧上方就会横穿天空指针，读起来两层信息糊在一起。这样一来 pitch=0
         * 时 ±20° 线不显示——本就不该显示，大仰角的刻度要等姿态转过去才该
         * 进入视野，这正是梯度随姿态滚动的正常行为。
         *
         * 判定按**标签**而非线：越界的线只是被弧盖住，越界的标签却会露出半
         * 个字。 */
        if (mark_y - LADDER_LBL_H / 2 < LADDER_TOP || mark_y > LADDER_BOT) {
            continue;
        }
        /* 圆形裁剪：把梯度线约束在坡度弧**内侧**。
         *
         * 加大弧半径只是拉开了当前这几档的间距，pitch 变化时更远的线照样会
         * 穿出去。真机 G1000 是把整个俯仰梯度裁在弧内的，这里用同样的办法：
         * 线到弧心的垂直距离为 dy 时，弧内可用的最大半宽是 √(lim² - dy²)；
         * dy 已经超出 lim 的整条不画。 */
        {
            const int lim = BANK_ARC_R - 8;      /* 留出弧本身的粗细 */
            int dy = mark_y - BANK_ARC_CY;
            if (dy < 0) dy = -dy;

            /* 只有**这一行确实有弧**时才需要收窄。
             *
             * 初版写的是 `if (dy >= lim) continue;`，把 dy 超出的整条丢掉——
             * 那是错的：dy 超出意味着该 y 上压根没有弧（比如弧顶之上的
             * +20° 线），两者不可能交叠，却被整条抹掉了。这个错误还进一步
             * 误导了布局决策，让人以为弧不能下移，否则上方刻度就会消失。 */
            if (dy < lim) {
                int max_hw = (int)sqrtf((float)(lim * lim - dy * dy));
                if (half_w > max_hw) half_w = max_hw;
            }
        }
        int lx, ly, rx, ry;
        rotate_about_center(cs, sn, PFD_CX - half_w, mark_y, &lx, &ly);
        rotate_about_center(cs, sn, PFD_CX + half_w, mark_y, &rx, &ry);
        pk_pfd_draw_line_aa(fb, (float)lx, (float)ly, (float)rx, (float)ry,
                            line_th, lcol);
        if (p < 0) {
            /* "Below the horizon" marks rendered with an extra half-
             * length overlay — closest we get to a dashed line without
             * background-aware erasing. */
            int mx = (lx + rx) / 2;
            int my = (ly + ry) / 2;
            pk_pfd_draw_line_aa(fb, (float)lx, (float)ly, (float)mx, (float)my,
                                line_th, lcol);
        }
        /* 数字贴在梯度线两端外侧，垂直中心与线对齐。 */
        char label[4];
        snprintf(label, sizeof(label), "%d", abs_p);
        int lw = (int)strlen(label) * LADDER_LBL_W;
        LADDER_PUTS(fb, lx - lw - LADDER_LBL_GAP, ly - LADDER_LBL_H / 2,
                    label, lcol);
        LADDER_PUTS(fb, rx + LADDER_LBL_GAP, ry - LADDER_LBL_H / 2,
                    label, lcol);
    }
}

/* --- Bank arc + chevron + sky pointer ------------------------------ */

static void place_on_arc(float angle_deg, int *x, int *y)
{
    float rad = angle_deg * (float)M_PI / 180.0f;
    *x = (int)((float)BANK_ARC_CX + (float)BANK_ARC_R * sinf(rad) + 0.5f);
    *y = (int)((float)BANK_ARC_CY - (float)BANK_ARC_R * cosf(rad) + 0.5f);
}

static const int8_t bank_ticks[] = { -60, -45, -30, -20, -10, 10, 20, 30, 45, 60 };

static void draw_bank_arc(uint16_t *fb, float roll_deg)
{
    pk_pfd_draw_arc_aa(fb, (float)BANK_ARC_CX, (float)BANK_ARC_CY,
                       (float)BANK_ARC_R - 0.5f,
                       -60.0f, 60.0f, BANK_ARC_TH, COL_BANK_ARC);

    /* Tick marks: three-tier lengths so the visual hierarchy reads
     * cleanly — ±10° smallest, ±20° medium, ±30°/±45°/±60° longest.
     * Garmin uses similar graduated lengths. */
    for (size_t i = 0; i < sizeof(bank_ticks) / sizeof(bank_ticks[0]); ++i) {
        int angle = bank_ticks[i];
        int abs_a = angle < 0 ? -angle : angle;
        /* 长度与粗细同向变化，读起来才是一个层级而不是两套信息。 */
        int   tick_len;
        float tick_th;
        switch (abs_a) {
            case 10:  tick_len = BANK_TICK_S; tick_th = 1.4f; break;
            case 20:  tick_len = BANK_TICK_M; tick_th = 1.7f; break;
            default:  tick_len = BANK_TICK_L; tick_th = 2.1f; break;  /* 30/45/60 */
        }
        int x0, y0;
        place_on_arc((float)angle, &x0, &y0);
        float rad = (float)angle * (float)M_PI / 180.0f;
        int x1 = (int)((float)BANK_ARC_CX +
                       (float)(BANK_ARC_R + tick_len) * sinf(rad) + 0.5f);
        int y1 = (int)((float)BANK_ARC_CY -
                       (float)(BANK_ARC_R + tick_len) * cosf(rad) + 0.5f);
        pk_pfd_draw_line_aa(fb, (float)x0, (float)y0, (float)x1, (float)y1,
                            tick_th, COL_BANK_TICK);
    }

    /* Sky pointer — fixed downward-pointing inverted white triangle
     * at the top center of the attitude region. Marks the 0° bank
     * reference; the chevron below indicates current bank against it. */
    /* 天空指针放在弧的**外侧**，于是从外到内读作：白三角 → 弧 → 黄 chevron。
     * 白三角是 0° 坡度的固定基准，黄 chevron 是当前坡度——基准在外、读数在内，
     * 两者不会互相遮挡，扫一眼就知道偏了多少。
     *
     * 尺寸不迁就残余空间：弧心已按「状态栏 → 空气 → 白三角 → 弧顶」的顺序
     * 整体下移（见 pfd_layout.h 的 PFD_BANK_ARC_CY），三角有完整的 14 px，
     * 上方还留着 16 px 空气。硬塞进一道 8 px 的缝只会得到一个谁也看不清的
     * 扁片——飞行中是扫视，读不出来的元素等于没有。 */
    const int skyptr_base_y = BANK_ARC_CY - BANK_ARC_R - SKYPTR_H - 1;
    pk_pfd_draw_triangle(fb,
                         PFD_CX,             skyptr_base_y + SKYPTR_H,
                         PFD_CX - SKYPTR_HW, skyptr_base_y,
                         PFD_CX + SKYPTR_HW, skyptr_base_y,
                         COL_SKY_POINTER);

    /* Bank chevron — yellow filled triangle hanging *below* the arc
     * at the current roll angle. Tip touches the arc; base sits 12 px
     * further from BANK_ARC_CY (toward the bottom of the screen). */
    int tip_x, tip_y;
    place_on_arc(roll_deg, &tip_x, &tip_y);
    float rad = roll_deg * (float)M_PI / 180.0f;
    /* Vector pointing from arc point inward toward the virtual center —
     * that's the direction the chevron base sits. */
    float in_x = -sinf(rad);
    float in_y =  cosf(rad);
    int base_cx = (int)((float)tip_x + in_x * (float)CHEVRON_LEN + 0.5f);
    int base_cy = (int)((float)tip_y + in_y * (float)CHEVRON_LEN + 0.5f);
    /* Perpendicular to (in_x, in_y) gives the chevron base width
     * direction; 6 px half-width. */
    float perp_x = -in_y;
    float perp_y =  in_x;
    int b1x = (int)((float)base_cx + perp_x * (float)CHEVRON_HW + 0.5f);
    int b1y = (int)((float)base_cy + perp_y * (float)CHEVRON_HW + 0.5f);
    int b2x = (int)((float)base_cx - perp_x * (float)CHEVRON_HW + 0.5f);
    int b2y = (int)((float)base_cy - perp_y * (float)CHEVRON_HW + 0.5f);
    pk_pfd_draw_triangle(fb, tip_x, tip_y, b1x, b1y, b2x, b2y, COL_BANK_POINTER);
}

/* --- Reticle (fixed) ----------------------------------------------- */

static void draw_reticle(uint16_t *fb)
{
    const int cx = PFD_CX;
    const int cy = PFD_CY;
    pk_pfd_fill_rect(fb, cx - 30, cy - 1, cx -  8, cy + 2, COL_RETICLE);
    pk_pfd_fill_rect(fb, cx +  8, cy - 1, cx + 30, cy + 2, COL_RETICLE);
    pk_pfd_fill_rect(fb, cx -  1, cy - 1, cx +  2, cy + 9, COL_RETICLE);
    pk_pfd_fill_rect(fb, cx -  2, cy - 2, cx +  3, cy + 3, COL_RETICLE);
}

/*
 * 姿态失效旗（ATT FAIL）。
 *
 * 原来 valid=false 时 roll/pitch 一律取 0，然后**照常画一幅地平线**——于是
 * 没接 IMU 的盒子开机后显示的是一幅完美水平的姿态仪。空态排查时截出来才发现：
 * 这一屏不是「看起来像坏了」，是反过来的「看起来好好的，其实什么都不知道」，
 * 比黑屏危险得多——飞行员没有任何线索去怀疑它。
 *
 * 画法照通用 EFIS 惯例：撤掉天地、划一个大红叉、盖 ATT FAIL 字样。撤掉天地
 * 是关键一步，红叉只是强调；只加叉不撤天地，余光扫过去仍是一幅有姿态的图。
 *
 * 文案用英文字面量而不走 i18n：PFD 这一屏刻意零词条（HDG / ALT / VS / KM/H
 * 全是固定缩写），中英两版逐字节相同，ATT 也是 ICAO 通用缩写，不该由它开
 * 破例的头。
 */
static void draw_att_fail(uint16_t *fb)
{
    const uint16_t COL_BG   = pk_rgb565( 18,  20,  26);
    const uint16_t COL_FAIL = pk_rgb565(255,  60,  60);

    pk_pfd_fill_rect(fb, PFD_ATTITUDE_LEFT, PFD_ATTITUDE_TOP,
                     PFD_ATTITUDE_RIGHT, PFD_ATTITUDE_BOT, COL_BG);

    /* 红叉画在姿态区内，不铺满全屏：左右速度/高度带与底部 HSI 有各自的
     * 有效性，被这道叉划掉会连累它们一起显得失效。 */
    const int x0 = PFD_ATTITUDE_LEFT + 120, x1 = PFD_ATTITUDE_RIGHT - 120;
    const int y0 = PFD_ATT_TOP + 24,        y1 = PFD_ATT_TOP + PFD_ATT_H - 24;
    for (int t = -1; t <= 1; ++t) {          /* 3 px 粗：1 px 在 217 PPI 上太细 */
        pk_pfd_draw_line(fb, x0, y0 + t, x1, y1 + t, COL_FAIL);
        pk_pfd_draw_line(fb, x0, y1 + t, x1, y0 + t, COL_FAIL);
    }

    const char *msg = "ATT FAIL";
    const int w  = pk_aa_text_width(msg, PK_AA_L);
    /* 下移 56 px 而不是正中：正中是十字准星的位置，而准星要保留（它是机体
     * 固定参考）。两者叠在一起时黄色横杠正好从 ATT 和 FAIL 中间穿过去。 */
    const int ty = PFD_CY + 56 - PK_AA_L_H / 2;
    /* 压一层实底再写字：字落在两条叉线之间，不铺底会被划穿。 */
    pk_pfd_fill_rect(fb, PFD_CX - w / 2 - 14, ty - 8,
                     PFD_CX + w / 2 + 14, ty + PK_AA_L_H + 8, COL_BG);
    pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
               PFD_CX - w / 2, ty, msg, COL_FAIL, PK_AA_L);
}

/* --- Public entry --------------------------------------------------- */

void pk_pfd_attitude_render(uint16_t *fb, const pk_pfd_imu_t *imu)
{
    build_gradient_luts();

    if (!imu->valid) {
        /* 坡度弧与刻度同样撤掉：它们刻的是 roll，而 roll 此刻是个假的 0。
         * 十字准星保留——它是**机体固定参考**，不依赖任何传感器。 */
        draw_att_fail(fb);
        draw_reticle(fb);
        return;
    }

    draw_horizon(fb, imu->roll_deg, imu->pitch_deg);
    draw_pitch_ladder(fb, imu->roll_deg, imu->pitch_deg);
    draw_bank_arc(fb, imu->roll_deg);
    draw_reticle(fb);
}
