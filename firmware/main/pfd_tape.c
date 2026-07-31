/*
 * pfd_tape.c — Garmin G1000-style right-side altitude tape.
 *
 * Spec §3 geometry: 72 px wide tape band on the right of the attitude
 * region, with the center-value digit box (74 × 24) bleeding 2 px
 * left of the tape so it overlaps the attitude indicator like the
 * real G1000.
 *
 * Scale: 1 px = 5 ft. Minor ticks every 20 ft (4 px apart), major
 * ticks every 100 ft (20 px apart), labels every 200 ft.
 *
 * Stale handling: when `valid` is false we still draw the frame +
 * ticks (so the widget silhouette persists) but the center box shows
 * "----" in grey and tick labels are suppressed.
 */

#include "pfd_tape.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "pfd_layout.h"
#include "pfd_aa_text.h"
#include "pfd_draw.h"
#include "pfd_font.h"

#define TAPE_X0   PFD_ALT_X0       /* tape band: 64 px wide (was 72) */
#define TAPE_X1   PFD_ALT_X1
#define TAPE_TOP   PFD_TAPE_TOP
#define TAPE_BOT  PFD_TAPE_BOT       /* shortened to y=168 to make room for the
                               three info boxes (BARO / metric / VS)
                               below the tape. TAPE_CY shifts from 113
                               to 93 automatically. */
#define TAPE_CY   ((TAPE_TOP + TAPE_BOT) / 2)   /* 93 */

/* ── 刻度密度 ──────────────────────────────────────────────────
 *
 * 标注间距必须大于标签字形的 cell 高，否则相邻标签直接叠在一起。
 * 800 屏用 S 档（cell 30 px），沿用 320 的「200 ft / 40 px」就贴死了。
 *
 * 800：带高 250 px，取 2 ft/px → 视窗 ±250 ft，与真机 G1000 高度带
 *      相当；标签每 100 ft = 50 px 间距，容得下 30 px 的 cell。
 * 320：历史值，1 px = 5 ft。 */
/* 2026-08-01：原有一份 `#else` 的 320 档刻度密度（5 ft/px、200 ft 一标）。
 * PK_DISPLAY_W 由 display.h 无条件钉死在 800，那条分支编不到，一并删除。 */
#define FT_PER_PX      2
#define MINOR_FT      20
#define MAJOR_FT     100
#define LABEL_EVERY  100

#define FT_TO_PX(dft)  ((dft) / FT_PER_PX)
/* 视窗半高换算成 ft，决定要遍历哪一段刻度。 */
#define TAPE_HALF_FT   (((TAPE_BOT - TAPE_TOP) / 2) * FT_PER_PX)

/* ── 当前值框 ──────────────────────────────────────────────────
 *
 * 大屏用 XL 档（43 px cap ≈ 5.0 mm，spec §2 规定「PFD 当前值」用它）。
 *
 * 5 位数 × 37 px advance = 185 px，而高度带只有 100 px 宽，所以值框
 * **向左突出压在姿态仪上**。这不是妥协，正是 G1000 的原样 —— 当前高度
 * 是主仪表最该一眼看到的数，让它占满该占的宽度；姿态仪被遮的是靠边
 * 的一小条，那里本来也没有信息。
 *
 * 刻度标签用 S 档：4 位数 × 17 px = 68 px，塞得进 100 px 的带宽。 */
#define VAL_PAD_X   4
#define VAL_PAD_Y   3

/* 2026-07-30：这里原有一个 #else 的 320 档版本（cockpit 12×16 字形、框与带
 * 同宽）。小屏兼容预览停止维护后，它引用的 pk_font_puts_cockpit() 已被删除，
 * 分支一并清掉——只剩一条路径就不必再写 #if。 */
#define VAL_DIGITS  5
#define BOX_W       (VAL_DIGITS * PK_AA_XL_W + 2 * VAL_PAD_X + 2)
#define BOX_H       (PK_AA_XL_H + 2 * VAL_PAD_Y + 2)
#define VAL_PUTS(fb, x, y, s, col) \
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), PK_AA_XL)
#define LBL_PUTS(fb, x, y, s, col) \
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), PK_AA_M)
#define LBL_W       PK_AA_M_W
#define LBL_CELL_H  PK_AA_M_H

/* 右对齐贴住面板右缘；框高居中于带中线。 */
#define BOX_X1   PFD_ALT_X1
#define BOX_X0   (BOX_X1 - BOX_W)
#define BOX_Y0   (TAPE_CY - BOX_H / 2)
#define BOX_Y1   (BOX_Y0 + BOX_H)

#define COL_BG         pk_rgb565(  8,   8,  12)
#define COL_BORDER_L   pk_rgb565( 70, 220, 250)   /* cyan left edge */
#define COL_TICK       pk_rgb565(220, 220, 220)
#define COL_LABEL      pk_rgb565(240, 240, 240)
#define COL_BOX_BRDR   pk_rgb565(255, 255, 255)
#define COL_VALUE      pk_rgb565(255, 255, 255)
#define COL_STALE      pk_rgb565(100, 100, 100)

void pk_pfd_alt_tape_render(uint16_t *fb, const pk_pfd_alt_tape_t *a)
{
    /* Semi-transparent dark band over the attitude background — pilot
     * still sees sky/ground through the tape but with enough contrast
     * for the ticks + labels. 50% darken (alpha=128). The 1 px cyan
     * left edge stays fully opaque as the visual divider. */
    pk_pfd_darken_rect(fb, TAPE_X0, TAPE_TOP, TAPE_X1, TAPE_BOT, 128);
    pk_pfd_fill_rect(fb, TAPE_X0, TAPE_TOP, TAPE_X0 + 1, TAPE_BOT, COL_BORDER_L);

    /* Walk minor ticks across the visible window: tape spans
     * (TAPE_BOT - TAPE_TOP) px at 5 ft/px = 950 ft, so ±475 ft around
     * center. Round up to 500 for clean tick layout. */
    int center_ft = a->valid ? a->altitude_ft : 5000;
    int low  = ((center_ft - TAPE_HALF_FT) / MINOR_FT) * MINOR_FT;
    int high = ((center_ft + TAPE_HALF_FT) / MINOR_FT) * MINOR_FT;
    for (int ft = low; ft <= high; ft += MINOR_FT) {
        int y = TAPE_CY - FT_TO_PX(ft - center_ft);
        if (y < TAPE_TOP || y >= TAPE_BOT) continue;
        bool major = (ft % MAJOR_FT) == 0;
        int tick_len = major ? 10 : 4;
        pk_pfd_fill_rect(fb,
                         TAPE_X0 + 2, y,
                         TAPE_X0 + 2 + tick_len, y + 1,
                         COL_TICK);
        if (a->valid && major && (ft % LABEL_EVERY) == 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", ft);
            /* Right-justify the label inside the tape band, with the
             * digits sitting just left of the right panel edge. */
            int w = (int)strlen(buf) * LBL_W;
            LBL_PUTS(fb, TAPE_X1 - w - 2, y - LBL_CELL_H / 2, buf, COL_LABEL);
        }
    }

    /* Center value box — fill, border, then digits. Sits flush with
     * the tape band (no bleed) at scale-2 font so 5-digit altitudes
     * (up to 99999 ft) fit cleanly inside the 64 px box. */
    pk_pfd_fill_rect(fb, BOX_X0, BOX_Y0, BOX_X1, BOX_Y1, COL_BG);
    pk_pfd_fill_rect(fb, BOX_X0,     BOX_Y0,     BOX_X1,     BOX_Y0 + 1, COL_BOX_BRDR);
    pk_pfd_fill_rect(fb, BOX_X0,     BOX_Y1 - 1, BOX_X1,     BOX_Y1,     COL_BOX_BRDR);
    pk_pfd_fill_rect(fb, BOX_X0,     BOX_Y0,     BOX_X0 + 1, BOX_Y1,     COL_BOX_BRDR);
    pk_pfd_fill_rect(fb, BOX_X1 - 1, BOX_Y0,     BOX_X1,     BOX_Y1,     COL_BOX_BRDR);

    /* 定宽字体下 "%5d" 的前导空格即右对齐，高度位数变化时个位不会左右
     * 游走 —— 这正是选 B612 Mono 的理由。 */
    if (a->valid) {
        char buf[8];
        int alt = a->altitude_ft;
        if (alt < 0)     alt = 0;
        if (alt > 99999) alt = 99999;
        snprintf(buf, sizeof(buf), "%5d", alt);
        VAL_PUTS(fb, BOX_X0 + VAL_PAD_X + 1, BOX_Y0 + VAL_PAD_Y + 1, buf, COL_VALUE);
    } else {
        VAL_PUTS(fb, BOX_X0 + VAL_PAD_X + 1, BOX_Y0 + VAL_PAD_Y + 1, "-----", COL_STALE);
    }
}
