/*
 * pfd.c — Phase 4c v2 Primary Flight Display.
 *
 * Renders a recognisable aviation-style PFD into the 240×320 ST7789
 * framebuffer at 30 FPS. The horizon math is identical to v1; v2
 * adds the elements a pilot actually looks at:
 *
 *   ┌──────────────────────────────┐  y =   0
 *   │     bank arc + pointer       │ 35 px
 *   ├──────────────────────────────┤  y =  35
 *   │   pitch ladder over horizon  │
 *   │      + center reticle        │ 205 px
 *   ├──────────────────────────────┤  y = 240
 *   │      heading tape            │ 30 px
 *   ├──────────────────────────────┤  y = 270
 *   │   R:+12.3°   P:-5.4°         │
 *   │   HDG: 087°  ADSB: 12        │ 50 px
 *   └──────────────────────────────┘  y = 320
 *
 * The sky/ground horizon fills the 0..240 band; bank arc + pitch
 * ladder draw on top of it (over the sky). Heading tape and number
 * panel are dark, so we explicitly fill_rect those regions every frame.
 *
 * Phase 4d (deferred) will:
 *   - swap the synchronous pk_display_flush_full() for an async GDMA
 *     pipeline so render+flush can overlap
 *   - hoist common 16-bit fills into a fast memset16 (or use esp_lcd's
 *     trans_done callback to flip double-buffers)
 *   - chase 60 FPS
 *
 * The math here intentionally avoids floating-point trig in the inner
 * pixel loops — the horizon fill samples a precomputed slope, and the
 * pitch-ladder lines pre-rotate their endpoints with one sin/cos per
 * line then run Bresenham.
 */

#include "pfd.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "aircraft_state.h"
#include "display.h"
#include "imu_task.h"
#include "pfd_font.h"

static const char *TAG = "pfd";

/* --- Layout constants ----------------------------------------------- */
#define PFD_ATTITUDE_TOP        0
#define PFD_ATTITUDE_BOT        240
#define PFD_HEADING_TOP         240
#define PFD_HEADING_BOT         270
#define PFD_PANEL_TOP           270
#define PFD_PANEL_BOT           320

#define PFD_CX                  (PK_DISPLAY_W / 2)              /* 120 */
#define PFD_CY                  ((PFD_ATTITUDE_TOP + PFD_ATTITUDE_BOT) / 2)  /* 120 */
#define PFD_PIXELS_PER_DEG      3

/* Bank-arc geometry: pretend the arc has a centre well below the panel
 * so its visible top arcs cleanly across the upper screen. */
#define BANK_ARC_CX             PFD_CX
#define BANK_ARC_CY             200
#define BANK_ARC_R              190

/* Heading tape pixels per degree. 240 px / 80° visible = 3 px/° feels
 * natural for the panel size. */
#define HDG_PX_PER_DEG          3

/* --- Palette (RGB565, panel byte order) ----------------------------- */
#define COL_SKY                 pk_rgb565( 30, 130, 230)
#define COL_GROUND              pk_rgb565(120,  85,  50)
#define COL_HORIZON             pk_rgb565(255, 255, 255)
#define COL_RETICLE             pk_rgb565(255, 215,   0)
#define COL_PITCH_LINE          pk_rgb565(255, 255, 255)
#define COL_BANK_TICK           pk_rgb565(255, 255, 255)
#define COL_BANK_POINTER        pk_rgb565(255, 215,   0)
#define COL_PANEL_BG            pk_rgb565( 12,  12,  16)
#define COL_TAPE_BG             pk_rgb565( 20,  20,  28)
#define COL_TAPE_TICK           pk_rgb565(200, 200, 200)
#define COL_TAPE_LABEL          pk_rgb565( 80, 220, 240)
#define COL_TAPE_CARET          pk_rgb565(255, 215,   0)
#define COL_LABEL               pk_rgb565( 80, 220, 240)
#define COL_VALUE               pk_rgb565(240, 240, 240)
#define COL_ACCENT              pk_rgb565(255, 215,   0)

/* --- Primitives ----------------------------------------------------- */

static inline void put_pixel(uint16_t *fb, int x, int y, uint16_t c)
{
    if (x < 0 || x >= PK_DISPLAY_W || y < 0 || y >= PK_DISPLAY_H) return;
    fb[y * PK_DISPLAY_W + x] = c;
}

static void fill_rect(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x0 < 0) x0 = 0;
    if (x1 > PK_DISPLAY_W) x1 = PK_DISPLAY_W;
    if (y0 < 0) y0 = 0;
    if (y1 > PK_DISPLAY_H) y1 = PK_DISPLAY_H;
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = fb + y * PK_DISPLAY_W;
        for (int x = x0; x < x1; ++x) row[x] = c;
    }
}

/* Bresenham line. Cheap, jaggy at fine angles — acceptable for v2;
 * v3 could add Wu anti-aliasing if the visual quality matters. */
static void draw_line(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx =  abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        put_pixel(fb, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Filled triangle via barycentric / scanline. Used for the bank-arc
 * pointer (small enough that this is cheap). */
static void draw_triangle(uint16_t *fb,
                          int ax, int ay, int bx, int by, int cx, int cy,
                          uint16_t c)
{
    /* Find bbox + iterate pixels with cross-product sign test. */
    int xmin = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx);
    int xmax = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
    int ymin = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy);
    int ymax = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);
    if (xmin < 0) xmin = 0;
    if (xmax >= PK_DISPLAY_W) xmax = PK_DISPLAY_W - 1;
    if (ymin < 0) ymin = 0;
    if (ymax >= PK_DISPLAY_H) ymax = PK_DISPLAY_H - 1;
    for (int py = ymin; py <= ymax; ++py) {
        for (int px = xmin; px <= xmax; ++px) {
            int s1 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
            int s2 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
            int s3 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
            bool neg = (s1 < 0) || (s2 < 0) || (s3 < 0);
            bool pos = (s1 > 0) || (s2 > 0) || (s3 > 0);
            if (!(neg && pos)) put_pixel(fb, px, py, c);
        }
    }
}

/* --- Sky / ground horizon ------------------------------------------- */

static void draw_horizon(uint16_t *fb, float roll_deg, float pitch_deg)
{
    const float rad = roll_deg * (float)M_PI / 180.0f;
    const float slope = -tanf(rad);
    const float pitch_px = pitch_deg * PFD_PIXELS_PER_DEG;

    for (int y = PFD_ATTITUDE_TOP; y < PFD_ATTITUDE_BOT; ++y) {
        uint16_t *row = fb + y * PK_DISPLAY_W;
        for (int x = 0; x < PK_DISPLAY_W; ++x) {
            float hy = (float)PFD_CY + pitch_px + slope * ((float)x - (float)PFD_CX);
            row[x] = ((float)y < hy) ? COL_SKY : COL_GROUND;
        }
    }
    for (int x = 0; x < PK_DISPLAY_W; ++x) {
        float hy = (float)PFD_CY + pitch_px + slope * ((float)x - (float)PFD_CX);
        int yi = (int)(hy + 0.5f);
        put_pixel(fb, x, yi, COL_HORIZON);
        put_pixel(fb, x, yi - 1, COL_HORIZON);
    }
}

/* --- Pitch ladder ---------------------------------------------------- *
 *
 * Six labeled horizontal marks at ±10°/±20°/±30°. Each mark in
 * unrotated screen coordinates is a horizontal segment centred on
 * (PFD_CX, mark_y). Roll is applied as a rotation around (PFD_CX,
 * PFD_CY). We pre-compute cos/sin once per frame and run Bresenham for
 * each segment endpoint after rotation. Labels are placed at the
 * rotated endpoints, axis-aligned (a real PFD ladder would rotate the
 * digit glyphs too — that's a v3 nicety).
 */

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
        int half_w = (abs_p == 10) ? 30 : (abs_p == 20 ? 20 : 14);
        /* mark_y in unrotated space: pitch_deg degrees up from horizon */
        int mark_y = PFD_CY + (int)((pitch_deg - (float)p) * PFD_PIXELS_PER_DEG + 0.5f);
        if (mark_y < PFD_ATTITUDE_TOP - 20 || mark_y > PFD_ATTITUDE_BOT + 20) continue;
        int lx, ly, rx, ry;
        rotate_about_center(cs, sn, PFD_CX - half_w, mark_y, &lx, &ly);
        rotate_about_center(cs, sn, PFD_CX + half_w, mark_y, &rx, &ry);
        draw_line(fb, lx, ly, rx, ry, COL_PITCH_LINE);
        /* For negative-pitch marks ("below the horizon") split into a
         * dashed line by drawing only half — gives the eye a cue at a
         * glance. v3 could draw proper dashes. */
        if (p < 0) {
            int mx = (lx + rx) / 2;
            int my = (ly + ry) / 2;
            draw_line(fb, lx, ly, mx, my, COL_PITCH_LINE);
            /* Erase the right half by overpainting with the
             * background — but background varies (sky/ground). Cheaper
             * compromise: skip drawing the right half. We did already
             * draw the full line above, so do nothing extra — Phase
             * 4c v3 can do this properly. */
        }
        /* Labels: just the magnitude in degrees, axis-aligned. */
        char label[4];
        snprintf(label, sizeof(label), "%d", abs_p);
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                     lx - 14, ly - 3, label, COL_PITCH_LINE, 1);
        pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                     rx + 2, ry - 3, label, COL_PITCH_LINE, 1);
    }
}

/* --- Bank arc -------------------------------------------------------- */

static void place_on_arc(float angle_deg, int *x, int *y)
{
    float rad = angle_deg * (float)M_PI / 180.0f;
    *x = (int)((float)BANK_ARC_CX + (float)BANK_ARC_R * sinf(rad) + 0.5f);
    *y = (int)((float)BANK_ARC_CY - (float)BANK_ARC_R * cosf(rad) + 0.5f);
}

static const int8_t bank_ticks[] = { -30, -20, -10, 0, 10, 20, 30 };

static void draw_bank_arc(uint16_t *fb, float roll_deg)
{
    /* Ticks: tiny radial line segments, 5 px long. The 0° tick gets a
     * 7-px white triangle as the reference indicator. */
    for (size_t i = 0; i < sizeof(bank_ticks) / sizeof(bank_ticks[0]); ++i) {
        int angle = bank_ticks[i];
        int x0, y0, x1, y1;
        place_on_arc((float)angle, &x0, &y0);
        /* Outer endpoint, 6 px further out radially. */
        float rad = (float)angle * (float)M_PI / 180.0f;
        x1 = (int)((float)BANK_ARC_CX + (float)(BANK_ARC_R + 6) * sinf(rad) + 0.5f);
        y1 = (int)((float)BANK_ARC_CY - (float)(BANK_ARC_R + 6) * cosf(rad) + 0.5f);
        draw_line(fb, x0, y0, x1, y1, COL_BANK_TICK);
        if (angle == 0) {
            /* Triangle reference at the top — distinguishes the
             * 0°-roll position from the ordinary ticks. */
            int tx, ty;
            place_on_arc(0.0f, &tx, &ty);
            draw_triangle(fb,
                          tx,     ty + 1,
                          tx - 4, ty - 7,
                          tx + 4, ty - 7,
                          COL_BANK_TICK);
        }
    }
    /* Pointer: yellow triangle on the arc at the current roll. The
     * tip points outward (away from BANK_ARC_CY) so it visually
     * "lifts" off the arc. */
    int px, py;
    place_on_arc(roll_deg, &px, &py);
    float rad = roll_deg * (float)M_PI / 180.0f;
    int ax = (int)((float)BANK_ARC_CX + (float)(BANK_ARC_R - 8) * sinf(rad) + 0.5f);
    int ay = (int)((float)BANK_ARC_CY - (float)(BANK_ARC_R - 8) * cosf(rad) + 0.5f);
    /* Triangle base perpendicular to the radial direction. */
    int bx = (int)(px + 5.0f * cosf(rad) + 0.5f);
    int by = (int)(py + 5.0f * sinf(rad) + 0.5f);
    int cx = (int)(px - 5.0f * cosf(rad) + 0.5f);
    int cy = (int)(py - 5.0f * sinf(rad) + 0.5f);
    draw_triangle(fb, ax, ay, bx, by, cx, cy, COL_BANK_POINTER);
}

/* --- Reticle (fixed) ------------------------------------------------- */

static void draw_reticle(uint16_t *fb)
{
    const int cx = PFD_CX;
    const int cy = PFD_CY;
    fill_rect(fb, cx - 30, cy - 1, cx - 8,  cy + 2, COL_RETICLE);
    fill_rect(fb, cx +  8, cy - 1, cx + 30, cy + 2, COL_RETICLE);
    fill_rect(fb, cx -  1, cy - 1, cx +  2, cy + 9, COL_RETICLE);
    fill_rect(fb, cx -  2, cy - 2, cx +  3, cy + 3, COL_RETICLE);
}

/* --- Heading tape ---------------------------------------------------- *
 *
 * Bottom strip showing ±40° of compass heading around the current yaw,
 * scrolling sideways as the airplane turns. Major ticks at every 10°
 * with three-digit labels every 30°. A yellow caret in the middle
 * points to the current heading.
 */

static void draw_heading_tape(uint16_t *fb, float yaw_deg)
{
    fill_rect(fb, 0, PFD_HEADING_TOP, PK_DISPLAY_W, PFD_HEADING_BOT, COL_TAPE_BG);

    /* Each visible degree maps to HDG_PX_PER_DEG pixels. We need every
     * heading h such that |h - yaw_deg| (modulo 360) <= half-window. */
    const int half_window = PK_DISPLAY_W / (2 * HDG_PX_PER_DEG) + 5;

    int yaw_floor = (int)floorf(yaw_deg);
    for (int dh = -half_window; dh <= half_window; ++dh) {
        int hdg = ((yaw_floor + dh) % 360 + 360) % 360;
        int x = PFD_CX + (int)(((float)hdg - yaw_deg + (float)dh) * HDG_PX_PER_DEG / 1.0f + 0.5f);
        /* Simpler: position relative to the centre by dh degrees offset. */
        x = PFD_CX + dh * HDG_PX_PER_DEG -
            (int)((yaw_deg - (float)yaw_floor) * HDG_PX_PER_DEG + 0.5f);
        if (x < -10 || x > PK_DISPLAY_W + 10) continue;
        bool major = (hdg % 10) == 0;
        bool labelled = (hdg % 30) == 0;
        int tick_h = major ? 6 : 3;
        fill_rect(fb, x, PFD_HEADING_TOP, x + 1,
                  PFD_HEADING_TOP + tick_h, COL_TAPE_TICK);
        if (labelled) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%03d", hdg);
            int w = (int)strlen(buf) * PK_FONT_CELL_W(1);
            pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                         x - w / 2, PFD_HEADING_TOP + 9, buf,
                         COL_TAPE_LABEL, 1);
        }
    }
    /* Centre caret pointing down to the heading exactly under it. */
    int cx = PFD_CX;
    draw_triangle(fb,
                  cx,      PFD_HEADING_BOT - 1,
                  cx - 5,  PFD_HEADING_BOT - 8,
                  cx + 5,  PFD_HEADING_BOT - 8,
                  COL_TAPE_CARET);
}

/* --- Bottom panel: numeric readouts ---------------------------------- */

static void draw_panel_text(uint16_t *fb,
                            const pk_imu_sample_t *s, bool imu_valid,
                            size_t aircraft_count)
{
    fill_rect(fb, 0, PFD_PANEL_TOP, PK_DISPLAY_W, PFD_PANEL_BOT, COL_PANEL_BG);

    char buf[16];

    /* Row 1: roll on the left, pitch on the right. Scale 2 = 10×14 px
     * characters, ~6 chars wide → ~72 px per readout. */
    const int row1_y = PFD_PANEL_TOP + 4;
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 4, row1_y,
                 "R", COL_LABEL, 2);
    snprintf(buf, sizeof(buf), "%+6.1f~",
             imu_valid ? s->roll_deg : 0.0f);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 28, row1_y,
                 buf, imu_valid ? COL_VALUE : COL_LABEL, 2);

    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 124, row1_y,
                 "P", COL_LABEL, 2);
    snprintf(buf, sizeof(buf), "%+6.1f~",
             imu_valid ? s->pitch_deg : 0.0f);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 148, row1_y,
                 buf, imu_valid ? COL_VALUE : COL_LABEL, 2);

    /* Row 2: HDG and ADSB count. */
    const int row2_y = PFD_PANEL_TOP + 26;
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 4, row2_y,
                 "HDG", COL_LABEL, 2);
    snprintf(buf, sizeof(buf), "%03d~",
             imu_valid ? ((int)s->yaw_deg + 360) % 360 : 0);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 52, row2_y,
                 buf, imu_valid ? COL_VALUE : COL_LABEL, 2);

    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 124, row2_y,
                 "ADSB", COL_LABEL, 2);
    snprintf(buf, sizeof(buf), "%2u", (unsigned)aircraft_count);
    pk_font_puts(fb, PK_DISPLAY_W, PK_DISPLAY_H, 188, row2_y,
                 buf, COL_ACCENT, 2);
}

/* --- Render task ----------------------------------------------------- */

static void pfd_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "pfd_task running (Phase 4c v2: ladder + arc + tape + text)");

    uint16_t *fb = pk_display_framebuffer();
    if (fb == NULL) {
        ESP_LOGE(TAG, "no framebuffer — exiting");
        vTaskDelete(NULL);
    }

    /* Big stack-eaters live here as file-static so they don't blow the
     * task stack. Pinned to PSRAM (.ext_ram.bss) so they don't compete
     * with FreeRTOS / ESP-Hosted tasks for scarce DMA-capable
     * internal RAM during the early-boot constructor storm. */
    static EXT_RAM_BSS_ATTR aircraft_t scratch[AIRCRAFT_TABLE_CAPACITY];
    int64_t  fps_window_start_us = esp_timer_get_time();
    uint32_t frames_in_window = 0;

    while (1) {
        TickType_t frame_start = xTaskGetTickCount();

        pk_imu_sample_t s;
        bool have = pk_imu_sample_get(&s);
        float roll  = have ? s.roll_deg  : 0.0f;
        float pitch = have ? s.pitch_deg : 0.0f;
        float yaw   = have ? s.yaw_deg   : 0.0f;

        size_t n_aircraft = aircraft_state_snapshot(
            scratch, sizeof(scratch) / sizeof(scratch[0]),
            esp_timer_get_time());

        draw_horizon(fb, roll, pitch);
        draw_pitch_ladder(fb, roll, pitch);
        draw_bank_arc(fb, roll);
        draw_reticle(fb);
        draw_heading_tape(fb, yaw);
        draw_panel_text(fb, &s, have, n_aircraft);

        pk_display_flush_full();
        frames_in_window++;

        int64_t now = esp_timer_get_time();
        if (now - fps_window_start_us >= 1000000) {
            ESP_LOGI(TAG, "PFD %lu FPS  | roll=%+6.2f pitch=%+6.2f yaw=%6.2f"
                          "  imu_valid=%d  aircraft=%u",
                     (unsigned long)frames_in_window,
                     roll, pitch, yaw, have, (unsigned)n_aircraft);
            frames_in_window = 0;
            fps_window_start_us = now;
        }
        vTaskDelayUntil(&frame_start, pdMS_TO_TICKS(33));   /* 30 FPS target */
    }
}

esp_err_t pk_pfd_start(void)
{
    /* 6 KiB stack — generous because trig / floating-point ESP_LOGI
     * format strings can each chew 1 KiB on RISC-V, and the dashboard
     * line at the bottom of pfd_task uses both. */
    BaseType_t ok = xTaskCreatePinnedToCore(
        pfd_task, "pfd", 6 * 1024, NULL, 4, NULL, 0);
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_NO_MEM;
}
