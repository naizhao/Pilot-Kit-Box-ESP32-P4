/*
 * pfd_draw.c — shared 2D rasterization primitives. Extracted from pfd.c
 * so the per-widget render files can share them without duplication.
 *
 * All four routines clip against PK_DISPLAY_W / PK_DISPLAY_H. Callers
 * don't have to bounds-check before invoking.
 */

#include "pfd_draw.h"

#include <stdbool.h>
#include <stdlib.h>

#include "display.h"

void pk_pfd_put_pixel(uint16_t *fb, int x, int y, uint16_t c)
{
    if (x < 0 || x >= PK_DISPLAY_W || y < 0 || y >= PK_DISPLAY_H) return;
    fb[y * PK_DISPLAY_W + x] = c;
}

void pk_pfd_fill_rect(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c)
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

/* Bresenham line. Cheap, jaggy at fine angles — acceptable; Wu
 * anti-aliasing would be a v3 nicety. */
void pk_pfd_draw_line(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx =  abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1) {
        pk_pfd_put_pixel(fb, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Darken every pixel toward black by alpha/256 (alpha in [0, 256]).
 * Works directly in RGB565: unpack 5/6/5 channels, scale each by the
 * "keep" factor (256 - alpha), repack. Output is in panel byte order
 * since input was. ~6 ALU ops per pixel — at 30 FPS this is fine for
 * the few thousand-pixel overlay regions we use it on. */
void pk_pfd_darken_rect(uint16_t *fb, int x0, int y0, int x1, int y1, int alpha)
{
    if (alpha <= 0) return;
    if (alpha > 256) alpha = 256;
    if (x0 < 0) x0 = 0;
    if (x1 > PK_DISPLAY_W) x1 = PK_DISPLAY_W;
    if (y0 < 0) y0 = 0;
    if (y1 > PK_DISPLAY_H) y1 = PK_DISPLAY_H;

    const int keep = 256 - alpha;
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = fb + y * PK_DISPLAY_W;
        for (int x = x0; x < x1; ++x) {
            uint16_t px = row[x];
            /* Framebuffer holds panel-byte-order RGB565; undo the swap
             * to get the native R5G6B5 layout, scale, repack, re-swap. */
            uint16_t v = (uint16_t)((px >> 8) | (px << 8));
            uint16_t r = (v >> 11) & 0x1F;
            uint16_t g = (v >>  5) & 0x3F;
            uint16_t b =  v        & 0x1F;
            r = (uint16_t)((r * keep) >> 8);
            g = (uint16_t)((g * keep) >> 8);
            b = (uint16_t)((b * keep) >> 8);
            uint16_t nv = (uint16_t)((r << 11) | (g << 5) | b);
            row[x] = (uint16_t)((nv >> 8) | (nv << 8));
        }
    }
}

/* Filled triangle via bounding-box scan + sign-of-cross-product test. */
void pk_pfd_draw_triangle(uint16_t *fb,
                          int ax, int ay, int bx, int by, int cx, int cy,
                          uint16_t c)
{
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
            if (!(neg && pos)) pk_pfd_put_pixel(fb, px, py, c);
        }
    }
}
