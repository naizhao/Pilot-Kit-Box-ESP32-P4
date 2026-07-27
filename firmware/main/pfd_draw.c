/*
 * pfd_draw.c — shared 2D rasterization primitives. Extracted from pfd.c
 * so the per-widget render files can share them without duplication.
 *
 * All four routines clip against PK_DISPLAY_W / PK_DISPLAY_H. Callers
 * don't have to bounds-check before invoking.
 */

#include "pfd_draw.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#include "display.h"

static uint16_t rgb565_to_native(uint16_t c)
{
    return (uint16_t)((c >> 8) | (c << 8));
}

static uint16_t native_to_rgb565(uint16_t c)
{
    return (uint16_t)((c >> 8) | (c << 8));
}

static uint8_t clamp_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int floor_to_int(float v)
{
    return (int)floorf(v);
}

static int ceil_to_int(float v)
{
    return (int)ceilf(v);
}

void pk_pfd_put_pixel(uint16_t *fb, int x, int y, uint16_t c)
{
    if (x < 0 || x >= PK_DISPLAY_W || y < 0 || y >= PK_DISPLAY_H) return;
    fb[y * PK_DISPLAY_W + x] = c;
}

void pk_pfd_blend_pixel(uint16_t *fb, int x, int y, uint16_t c, uint8_t alpha)
{
    if (alpha == 0) return;
    if (x < 0 || x >= PK_DISPLAY_W || y < 0 || y >= PK_DISPLAY_H) return;
    if (alpha == 255) {
        fb[y * PK_DISPLAY_W + x] = c;
        return;
    }

    uint16_t *dstp = fb + y * PK_DISPLAY_W + x;
    uint16_t dst = rgb565_to_native(*dstp);
    uint16_t src = rgb565_to_native(c);

    int sr = (src >> 11) & 0x1F;
    int sg = (src >>  5) & 0x3F;
    int sb =  src        & 0x1F;
    int dr = (dst >> 11) & 0x1F;
    int dg = (dst >>  5) & 0x3F;
    int db =  dst        & 0x1F;

    int a = alpha;
    int ia = 255 - a;
    int r = (sr * a + dr * ia + 127) / 255;
    int g = (sg * a + dg * ia + 127) / 255;
    int b = (sb * a + db * ia + 127) / 255;

    *dstp = native_to_rgb565((uint16_t)((r << 11) | (g << 5) | b));
}

static uint8_t quantize_dither(uint8_t v, int levels, int threshold)
{
    int scaled = (int)v * levels;
    int q = scaled / 255;
    int rem = scaled - q * 255;
    if (q < levels && rem > threshold) ++q;
    return clamp_u8(q);
}

uint16_t pk_pfd_rgb565_dither(uint8_t r, uint8_t g, uint8_t b, int x, int y)
{
    static const uint8_t bayer4[16] = {
         0,  8,  2, 10,
        12,  4, 14,  6,
         3, 11,  1,  9,
        15,  7, 13,  5,
    };
    int cell = ((y & 3) << 2) | (x & 3);
    int threshold = ((int)bayer4[cell] * 255 + 127) / 16;
    int r5 = quantize_dither(r, 31, threshold);
    int g6 = quantize_dither(g, 63, threshold);
    int b5 = quantize_dither(b, 31, threshold);
    return native_to_rgb565((uint16_t)((r5 << 11) | (g6 << 5) | b5));
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

void pk_pfd_draw_line_aa(uint16_t *fb,
                         float x0, float y0, float x1, float y1,
                         float width, uint16_t c)
{
    if (width < 1.0f) width = 1.0f;
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len2 = dx * dx + dy * dy;
    float half = width * 0.5f;
    float pad = half + 1.0f;

    int xmin = floor_to_int(fminf(x0, x1) - pad);
    int xmax = ceil_to_int (fmaxf(x0, x1) + pad);
    int ymin = floor_to_int(fminf(y0, y1) - pad);
    int ymax = ceil_to_int (fmaxf(y0, y1) + pad);
    if (xmin < 0) xmin = 0;
    if (xmax >= PK_DISPLAY_W) xmax = PK_DISPLAY_W - 1;
    if (ymin < 0) ymin = 0;
    if (ymax >= PK_DISPLAY_H) ymax = PK_DISPLAY_H - 1;

    if (len2 <= 0.0001f) {
        pk_pfd_blend_pixel(fb, (int)(x0 + 0.5f), (int)(y0 + 0.5f), c, 255);
        return;
    }

    for (int y = ymin; y <= ymax; ++y) {
        float py = (float)y + 0.5f;
        for (int x = xmin; x <= xmax; ++x) {
            float px = (float)x + 0.5f;
            float t = ((px - x0) * dx + (py - y0) * dy) / len2;
            t = clampf(t, 0.0f, 1.0f);
            float nx = x0 + t * dx;
            float ny = y0 + t * dy;
            float ex = px - nx;
            float ey = py - ny;
            float dist = sqrtf(ex * ex + ey * ey);
            float coverage = half + 0.5f - dist;
            if (coverage <= 0.0f) continue;
            uint8_t alpha = coverage >= 1.0f
                                ? 255
                                : (uint8_t)(coverage * 255.0f + 0.5f);
            pk_pfd_blend_pixel(fb, x, y, c, alpha);
        }
    }
}

void pk_pfd_draw_arc_aa(uint16_t *fb,
                        float cx, float cy, float radius,
                        float start_deg, float end_deg,
                        float width, uint16_t c)
{
    if (radius <= 0.0f) return;
    float span = end_deg - start_deg;
    while (span < 0.0f) span += 360.0f;
    if (span <= 0.0f) return;

    int steps = ceil_to_int(span / 1.5f);
    if (steps < 1) steps = 1;

    float prev_a = start_deg * (float)M_PI / 180.0f;
    float prev_x = cx + radius * sinf(prev_a);
    float prev_y = cy - radius * cosf(prev_a);
    for (int i = 1; i <= steps; ++i) {
        float a_deg = start_deg + span * (float)i / (float)steps;
        float a = a_deg * (float)M_PI / 180.0f;
        float x = cx + radius * sinf(a);
        float y = cy - radius * cosf(a);
        pk_pfd_draw_line_aa(fb, prev_x, prev_y, x, y, width, c);
        prev_x = x;
        prev_y = y;
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

void pk_pfd_draw_aircraft(uint16_t *fb, int cx, int cy,
                          float rot_deg, int size, uint16_t c)
{
    /* 机体坐标：y 轴向上为负（与屏幕一致），机头在 (0, -size)。
     * 后掠翼 + 收窄的尾部，四个点就够表达朝向，再多在 10 px 尺度上也糊。 */
    const float pts[4][2] = {
        {  0.00f, -1.00f },   /* 机头   */
        {  0.85f,  0.45f },   /* 右翼尖 */
        {  0.00f,  0.15f },   /* 尾部凹口 */
        { -0.85f,  0.45f },   /* 左翼尖 */
    };

    const float rad = rot_deg * (float)M_PI / 180.0f;
    const float cs = cosf(rad), sn = sinf(rad);

    int px[4], py[4];
    for (int i = 0; i < 4; ++i) {
        float x = pts[i][0] * (float)size;
        float y = pts[i][1] * (float)size;
        /* 屏幕 y 向下，故顺时针旋转就是标准旋转矩阵。 */
        px[i] = cx + (int)lroundf(x * cs - y * sn);
        py[i] = cy + (int)lroundf(x * sn + y * cs);
    }

    /* 拆成两个三角形填充——箭头是凹多边形，一次三角形填不出那个尾部凹口。 */
    pk_pfd_draw_triangle(fb, px[0], py[0], px[1], py[1], px[2], py[2], c);
    pk_pfd_draw_triangle(fb, px[0], py[0], px[2], py[2], px[3], py[3], c);
}
