/*
 * pfd_speed_tape.c — Garmin G1000-style left-side ground-speed tape.
 *
 * Mirrors pfd_tape.c (ALT tape) geometry to the left edge:
 *   • Tape band  x ∈ [0, 64),  y ∈ [18, 168)  — 64 px wide, 150 px tall
 *   • Metric pad x ∈ [0, 64),  y ∈ [170, 208) — km/h + mph conversions
 *   • Right-edge 1 px cyan divider  (x = 63)   — mirrors ALT left-edge cyan
 *   • Ticks grow LEFTward from the cyan edge (中心侧) — 镜像 ALT 刻度方向
 *   • Labels left-justified at x = 2 (屏幕边),文字在左、刻度在右
 *   • Centre value box at y ∈ [STAPE_CY-10, STAPE_CY+10] — derived from tape centre
 *
 * Scale: 1 px = 2 kt.  Minor ticks every 5 kt (2.5 px → 1 px steps),
 * major ticks every 25 kt, labels every 50 kt.
 *
 * Stale handling: frame + ticks always draw; when valid=false the centre
 * box shows "---" in grey and tick labels are suppressed.
 */

#include "pfd_speed_tape.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "pfd_layout.h"
#include "pfd_draw.h"
#include "pfd_aa_text.h"

/* ── geometry ─────────────────────────────────────────────────────────── */
#define STAPE_X0     PFD_SPD_X0
#define STAPE_X1    PFD_SPD_X1
#define STAPE_TOP   PFD_TAPE_TOP
#define STAPE_BOT  PFD_TAPE_BOT      /* tape band bottom */
#define STAPE_CY   ((STAPE_TOP + STAPE_BOT) / 2)   /* 93 */

/* ── 刻度密度 ──────────────────────────────────────────────────
 *
 * 与 pfd_tape.c 同理：标注间距必须大于标签的 cell 高，否则相邻标签叠在
 * 一起。沿用 320 的「50 kt / 25 px」在 800 屏上会直接把 S 档（cell 30 px）
 * 的两个标签压成一团。
 *
 * 800：带高 250 px，取 2 px/kt → 视窗 ±62 kt，与真机 G1000 速度带相当；
 *      标签每 20 kt = 40 px 间距。注意这里是**每节 2 像素**，与 320 的
 *      「每像素 2 节」正好互为倒数，故用宏封装换算方向。
 * 320：历史值，1 px = 2 kt。 */
/* 2026-08-01：原有一份 `#else` 的 320 档刻度密度（0.5 px/kt、50 kt 一标）。
 * PK_DISPLAY_W 由 display.h 无条件钉死在 800，那条分支编不到，一并删除。 */
#define KT_TO_PX(dkt)  ((dkt) * 2)
#define TAPE_HALF_KT   ((STAPE_BOT - STAPE_TOP) / 2 / 2)
#define MINOR_KT     5
#define MAJOR_KT    10
#define LABEL_EVERY 20

/* ── 当前值框 ──────────────────────────────────────────────────
 * 与 pfd_tape.c 镜像对称：那边贴右缘向左突出，这边贴左缘向右突出。
 * 3 位数 × 37 px = 111 px，只比 100 px 的带宽多出一点点。 */
#define VAL_PAD_X   4
#define VAL_PAD_Y   3

/* 2026-07-30：320 档的 #else 版本随小屏兼容预览一并删除，理由同 pfd_tape.c。 */
#define VAL_DIGITS  3
#define BOX_W       (VAL_DIGITS * PK_AA_XL_W + 2 * VAL_PAD_X + 2)
#define BOX_H       (PK_AA_XL_H + 2 * VAL_PAD_Y + 2)
#define VAL_PUTS(fb, x, y, s, col) \
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), PK_AA_XL)
#define LBL_PUTS(fb, x, y, s, col) \
        pk_aa_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, (x), (y), (s), (col), PK_AA_M)
#define LBL_CELL_H  PK_AA_M_H

#define BOX_X0   STAPE_X0
#define BOX_X1   (BOX_X0 + BOX_W)
#define BOX_Y0   (STAPE_CY - BOX_H / 2)
#define BOX_Y1   (BOX_Y0 + BOX_H)

/* ── colours ────────────────────────────────────────────────────────── */
#define COL_BG         pk_rgb565(  8,   8,  12)
#define COL_BORDER_R   pk_rgb565( 70, 220, 250)   /* cyan right edge */
#define COL_TICK       pk_rgb565(220, 220, 220)
#define COL_LABEL      pk_rgb565(240, 240, 240)
#define COL_BOX_BRDR   pk_rgb565(255, 255, 255)
#define COL_VALUE      pk_rgb565(255, 255, 255)
#define COL_STALE      pk_rgb565(100, 100, 100)

/* ── render ─────────────────────────────────────────────────────────── */
void pk_pfd_speed_tape_render(uint16_t *fb, const pk_pfd_speed_tape_t *s)
{
    /* Semi-transparent dark band over the attitude background. */
    pk_pfd_darken_rect(fb, STAPE_X0, STAPE_TOP, STAPE_X1, STAPE_BOT, 128);

    /* Right-edge 1 px cyan divider (mirrors ALT tape left-edge cyan). */
    pk_pfd_fill_rect(fb, STAPE_X1 - 1, STAPE_TOP, STAPE_X1, STAPE_BOT, COL_BORDER_R);

    /* Walk minor ticks across the visible window.
     * Tape spans (STAPE_BOT - STAPE_TOP) = 150 px at 2 kt/px = 300 kt total.
     * Show ±150 kt around centre; clamp minimum to 0. */
    int center_kt = s->valid ? s->ground_speed_kt : 60;
    if (center_kt < 0) center_kt = 0;
    int low  = ((center_kt - TAPE_HALF_KT) / MINOR_KT) * MINOR_KT;
    int high = ((center_kt + TAPE_HALF_KT) / MINOR_KT) * MINOR_KT;
    if (low < 0) low = 0;

    for (int kt = low; kt <= high; kt += MINOR_KT) {
        int y = STAPE_CY - KT_TO_PX(kt - center_kt);
        if (y < STAPE_TOP || y >= STAPE_BOT) continue;

        bool major    = (kt % MAJOR_KT) == 0;
        int  tick_len = major ? 10 : 4;

        /* 镜像 ALT:刻度贴 cyan 边(中心侧 x63)朝左伸,文字留在左(屏幕边) */
        pk_pfd_fill_rect(fb,
                         STAPE_X1 - 1 - tick_len, y,
                         STAPE_X1 - 1, y + 1,
                         COL_TICK);

        if (s->valid && major && (kt % LABEL_EVERY) == 0 && kt > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%d", kt);
            /* Left-justify label: start at x=2, clear of the tick */
            LBL_PUTS(fb, STAPE_X0 + 2, y - LBL_CELL_H / 2, buf, COL_LABEL);
        }
    }

    /* ── Centre value box ─────────────────────────────────────────── */
    pk_pfd_fill_rect(fb, BOX_X0, BOX_Y0, BOX_X1, BOX_Y1, COL_BG);
    /* Border: top, bottom, left, right */
    pk_pfd_fill_rect(fb, BOX_X0,     BOX_Y0,     BOX_X1,     BOX_Y0 + 1, COL_BOX_BRDR);
    pk_pfd_fill_rect(fb, BOX_X0,     BOX_Y1 - 1, BOX_X1,     BOX_Y1,     COL_BOX_BRDR);
    pk_pfd_fill_rect(fb, BOX_X0,     BOX_Y0,     BOX_X0 + 1, BOX_Y1,     COL_BOX_BRDR);
    pk_pfd_fill_rect(fb, BOX_X1 - 1, BOX_Y0,     BOX_X1,     BOX_Y1,     COL_BOX_BRDR);

    if (s->valid) {
        char buf[8];
        int gs = s->ground_speed_kt;
        if (gs < 0)   gs = 0;
        if (gs > 999) gs = 999;
        snprintf(buf, sizeof(buf), "%3d", gs);
        VAL_PUTS(fb, BOX_X0 + VAL_PAD_X + 1, BOX_Y0 + VAL_PAD_Y + 1, buf, COL_VALUE);
    } else {
        VAL_PUTS(fb, BOX_X0 + VAL_PAD_X + 1, BOX_Y0 + VAL_PAD_Y + 1, "---", COL_STALE);
    }

    /* 2026-07-30：这里原有一块 `#if PK_DISPLAY_W < 800` 的公制换算板（km/h +
     * mph 两行，压在速度带下方）。800 档早就把这两个数并进了左下角三行信息框
     * （pfd_infobox.c），与右下角三行对称；这块只在 320 档活着，用的是 6 px
     * 位图，低于 spec §2 的 18 px 硬下限。随小屏兼容预览一并删除。 */
}
