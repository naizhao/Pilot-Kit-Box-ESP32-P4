/*
 * text.c — mixed ASCII / generated CJK UTF-8 text rendering.
 */

#include "text.h"

#include <stdbool.h>
#include <stdint.h>

#include "pfd_font.h"
#include "text_font_cjk.h"
#include "text_font_cjk_body.h"
#include "text_font_cjk_ui.h"

#define CJK_SOLID_ALPHA4_THRESHOLD 3

static const uint8_t CJK_AA_LCD_ALPHA4[16] = {
    0, 0, 0, 6, 12, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
};

static uint16_t rgb565_to_native(uint16_t c)
{
    return (uint16_t)((c >> 8) | (c << 8));
}

static uint16_t native_to_rgb565(uint16_t c)
{
    return (uint16_t)((c >> 8) | (c << 8));
}

static uint32_t utf8_next(const char **ps)
{
    const unsigned char *s = (const unsigned char *)*ps;
    if (s == NULL || *s == '\0') return 0;

    if (s[0] < 0x80) {
        *ps = (const char *)(s + 1);
        return s[0];
    }
    if ((s[0] & 0xE0) == 0xC0 &&
        (s[1] & 0xC0) == 0x80) {
        *ps = (const char *)(s + 2);
        return ((uint32_t)(s[0] & 0x1F) << 6) |
               (uint32_t)(s[1] & 0x3F);
    }
    if ((s[0] & 0xF0) == 0xE0 &&
        (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80) {
        *ps = (const char *)(s + 3);
        return ((uint32_t)(s[0] & 0x0F) << 12) |
               ((uint32_t)(s[1] & 0x3F) << 6) |
               (uint32_t)(s[2] & 0x3F);
    }
    if ((s[0] & 0xF8) == 0xF0 &&
        (s[1] & 0xC0) == 0x80 &&
        (s[2] & 0xC0) == 0x80 &&
        (s[3] & 0xC0) == 0x80) {
        *ps = (const char *)(s + 4);
        return ((uint32_t)(s[0] & 0x07) << 18) |
               ((uint32_t)(s[1] & 0x3F) << 12) |
               ((uint32_t)(s[2] & 0x3F) << 6) |
               (uint32_t)(s[3] & 0x3F);
    }

    *ps = (const char *)(s + 1);
    return '?';
}

static void put_cjk(uint16_t *fb, int fb_w, int fb_h,
                    int x, int y, const uint8_t *glyph,
                    int cell_w, int cell_h,
                    uint16_t color, bool solid)
{
    for (int row = 0; row < cell_h; ++row) {
        int yy = y + row;
        if (yy < 0 || yy >= fb_h) continue;
        for (int col = 0; col < cell_w; ++col) {
            int xx = x + col;
            if (xx < 0 || xx >= fb_w) continue;

            int idx = row * cell_w + col;
            uint8_t packed = glyph[idx >> 1];
            uint8_t alpha4 = (idx & 1) ? (packed & 0x0F) : (packed >> 4);
            if (alpha4 == 0) continue;

            if (solid) {
                if (alpha4 < CJK_SOLID_ALPHA4_THRESHOLD) continue;
                fb[yy * fb_w + xx] = color;
                continue;
            }

            alpha4 = CJK_AA_LCD_ALPHA4[alpha4];
            if (alpha4 == 0) continue;

            uint8_t alpha = (uint8_t)(alpha4 * 17);
            if (alpha == 255) {
                fb[yy * fb_w + xx] = color;
                continue;
            }

            uint16_t *dstp = fb + yy * fb_w + xx;
            uint16_t dst = rgb565_to_native(*dstp);
            uint16_t src = rgb565_to_native(color);

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
    }
}

static int cjk_cell_w_for_scale(int ascii_scale)
{
    return (ascii_scale <= 1) ? PK_TEXT_CJK_BODY_CELL_W : PK_TEXT_CJK_CELL_W;
}

static int cjk_cell_h_for_scale(int ascii_scale)
{
    return (ascii_scale <= 1) ? PK_TEXT_CJK_BODY_CELL_H : PK_TEXT_CJK_CELL_H;
}

static const uint8_t *cjk_glyph_for_scale(uint32_t cp, int ascii_scale)
{
    return (ascii_scale <= 1) ? pk_text_cjk_body_glyph(cp)
                              : pk_text_cjk_glyph(cp);
}

static int codepoint_advance(uint32_t cp, int ascii_scale)
{
    if (ascii_scale < 1) ascii_scale = 1;
    if (cp > 0x7F && cp != 0x00B0) return cjk_cell_w_for_scale(ascii_scale);
    return PK_FONT_CELL_W(ascii_scale);
}

int pk_text_width(const char *s, int ascii_scale)
{
    int w = 0;
    while (s && *s) {
        uint32_t cp = utf8_next(&s);
        if (cp == 0) break;
        w += codepoint_advance(cp, ascii_scale);
    }
    return w;
}

int pk_text_title_width(const char *s)
{
    int w = 0;
    while (s && *s) {
        uint32_t cp = utf8_next(&s);
        if (cp == 0) break;
        w += (cp > 0x7F && cp != 0x00B0) ? PK_TEXT_CJK_CELL_W
                                          : PK_FONT_CELL_W(2);
    }
    return w;
}

int pk_text_ui_width(const char *s)
{
    int w = 0;
    while (s && *s) {
        uint32_t cp = utf8_next(&s);
        if (cp == 0) break;
        uint8_t cw = 0;
        if (pk_text_cjk_ui_glyph(cp, &cw) != NULL && cw > 0) {
            w += cw;
        } else {
            w += PK_FONT_CELL_W(1);
        }
    }
    return w;
}

int pk_text_puts(uint16_t *fb, int fb_w, int fb_h,
                 int x, int y, const char *s,
                 uint16_t color, int ascii_scale)
{
    if (ascii_scale < 1) ascii_scale = 1;
    int x0 = x;
    while (s && *s) {
        uint32_t cp = utf8_next(&s);
        if (cp == 0) break;

        if (cp == 0x00B0) {
            pk_font_putchar(fb, fb_w, fb_h, x, y,
                            (char)PK_FONT_DEGREE, color, ascii_scale);
            x += PK_FONT_CELL_W(ascii_scale);
            continue;
        }

        if (cp <= 0x7F) {
            pk_font_putchar(fb, fb_w, fb_h, x, y, (char)cp, color, ascii_scale);
            x += PK_FONT_CELL_W(ascii_scale);
            continue;
        }

        const uint8_t *glyph = cjk_glyph_for_scale(cp, ascii_scale);
        if (glyph != NULL) {
            int cw = cjk_cell_w_for_scale(ascii_scale);
            int ch = cjk_cell_h_for_scale(ascii_scale);
            put_cjk(fb, fb_w, fb_h, x, y, glyph, cw, ch, color, true);
            x += cw;
        } else {
            pk_font_putchar(fb, fb_w, fb_h, x, y, '?', color, ascii_scale);
            x += PK_FONT_CELL_W(ascii_scale);
        }
    }
    return x - x0;
}

int pk_text_puts_title(uint16_t *fb, int fb_w, int fb_h,
                       int x, int y, const char *s,
                       uint16_t color)
{
    int x0 = x;
    while (s && *s) {
        uint32_t cp = utf8_next(&s);
        if (cp == 0) break;

        if (cp <= 0x7F || cp == 0x00B0) {
            char tmp[2] = {
                (cp == 0x00B0) ? (char)PK_FONT_DEGREE : (char)cp,
                '\0',
            };
            pk_font_puts_cockpit(fb, fb_w, fb_h, x, y, tmp, color);
            x += PK_FONT_CELL_W(2);
            continue;
        }

        const uint8_t *glyph = pk_text_cjk_glyph(cp);
        if (glyph != NULL) {
            put_cjk(fb, fb_w, fb_h, x, y, glyph,
                    PK_TEXT_CJK_CELL_W, PK_TEXT_CJK_CELL_H, color, false);
            x += PK_TEXT_CJK_CELL_W;
        } else {
            pk_font_puts_cockpit(fb, fb_w, fb_h, x, y, "?", color);
            x += PK_FONT_CELL_W(2);
        }
    }
    return x - x0;
}

int pk_text_puts_ui(uint16_t *fb, int fb_w, int fb_h,
                    int x, int y, const char *s,
                    uint16_t color)
{
    int x0 = x;
    while (s && *s) {
        uint32_t cp = utf8_next(&s);
        if (cp == 0) break;

        uint8_t cw = 0;
        const uint8_t *glyph = pk_text_cjk_ui_glyph(cp, &cw);
        if (glyph != NULL) {
            put_cjk(fb, fb_w, fb_h, x, y, glyph,
                    cw, PK_TEXT_CJK_UI_CELL_H, color, false);
            x += cw;
        } else {
            pk_font_putchar(fb, fb_w, fb_h, x, y, '?', color, 1);
            x += PK_FONT_CELL_W(1);
        }
    }
    return x - x0;
}

int pk_text_puts_page_title(uint16_t *fb, int fb_w, int fb_h,
                            int x, int y, const char *s,
                            uint16_t color)
{
    int x0 = x;
    while (s && *s) {
        uint32_t cp = utf8_next(&s);
        if (cp == 0) break;

        if (cp == 0x00B0) {
            pk_font_putchar(fb, fb_w, fb_h, x, y,
                            (char)PK_FONT_DEGREE, color, 2);
            x += PK_FONT_CELL_W(2);
            continue;
        }

        if (cp <= 0x7F) {
            pk_font_putchar(fb, fb_w, fb_h, x, y, (char)cp, color, 2);
            x += PK_FONT_CELL_W(2);
            continue;
        }

        const uint8_t *glyph = pk_text_cjk_glyph(cp);
        if (glyph != NULL) {
            put_cjk(fb, fb_w, fb_h, x, y, glyph,
                    PK_TEXT_CJK_CELL_W, PK_TEXT_CJK_CELL_H, color, false);
            x += PK_TEXT_CJK_CELL_W;
        } else {
            pk_font_putchar(fb, fb_w, fb_h, x, y, '?', color, 2);
            x += PK_FONT_CELL_W(2);
        }
    }
    return x - x0;
}

int pk_text_puts_page_body(uint16_t *fb, int fb_w, int fb_h,
                           int x, int y, const char *s,
                           uint16_t color)
{
    int x0 = x;
    while (s && *s) {
        uint32_t cp = utf8_next(&s);
        if (cp == 0) break;

        if (cp == 0x00B0) {
            pk_font_putchar(fb, fb_w, fb_h, x, y,
                            (char)PK_FONT_DEGREE, color, 1);
            x += PK_FONT_CELL_W(1);
            continue;
        }

        if (cp <= 0x7F) {
            pk_font_putchar(fb, fb_w, fb_h, x, y, (char)cp, color, 1);
            x += PK_FONT_CELL_W(1);
            continue;
        }

        uint8_t cw = 0;
        const uint8_t *glyph = pk_text_cjk_ui_glyph(cp, &cw);
        if (glyph != NULL && cw > 0) {
            put_cjk(fb, fb_w, fb_h, x, y, glyph,
                    cw, PK_TEXT_CJK_UI_CELL_H, color, false);
            x += cw;
        } else {
            pk_font_putchar(fb, fb_w, fb_h, x, y, '?', color, 1);
            x += PK_FONT_CELL_W(1);
        }
    }
    return x - x0;
}
