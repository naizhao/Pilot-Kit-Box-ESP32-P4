/*
 * text.c — mixed ASCII / generated CJK UTF-8 text rendering.
 */

#include "text.h"

/*
 * ASCII 走 PFD 那套抗锯齿字体（B612 Mono），不再用 5×7 位图整数缩放。
 *
 * 位图字体是 320×240 时代的产物：在 167 PPI 上原始尺寸锐利，但这块 4.3″ 是
 * 217 PPI，同样像素数物理上更小，放大又变成方块像素。spec §2 把 18 px
 * （2.1 mm）定为硬下限，位图那档根本够不着。
 *
 * CJK 仍走生成的位图子集，但尺寸已按 spec 阶梯重新生成（L30 / M26 / S21）。
 * 两套字的 cell 高度不同，混排时把 CJK 下移半个差值，行内基线才对得上。
 */
#include "pfd_aa_text.h"
#include "pfd_aa_font.h"

#include <stdbool.h>
#include <stdint.h>

#include "pfd_font.h"
#include "text_font_cjk.h"
#include "text_font_cjk_body.h"
#include "text_font_cjk_ui.h"

#define CJK_SOLID_ALPHA4_THRESHOLD 3

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

            /* 直接用字库里的灰度，不做重映射。
             *
             * 这里原有一张查表 CJK_AA_LCD_ALPHA4[16]，把 alpha 5 以上一律拉满
             * 成 15（完全不透明），只给 3、4 留了 6 和 12 两级。那是 320 屏的
             * 补偿——167 PPI 上汉字细笔画本就只有一两个像素宽，边缘再按真实
             * 灰度混合会糊成一团，索性压实。
             *
             * 但这块屏是 217 PPI 且字号已按 spec 提到 21..30 px，笔画有足够
             * 像素支撑。再压实就等于扔掉抗锯齿，边缘全变成硬阶梯——观感上就是
             * 汉字「发破」，与旁边走真灰度的拉丁字形成明显反差。 */
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

/* 与某一档 CJK 搭配的拉丁档位。CJK 是方块字、cell 即字号，拉丁 cell 含行距，
 * 所以两边不是同一个数，得按「字形高度接近」来配。 */
static pk_aa_size_t aa_size_for_cjk(int cjk_cell_h)
{
    return (cjk_cell_h >= PK_TEXT_CJK_CELL_H) ? PK_AA_L : PK_AA_M;
}

/* CJK 位图相对拉丁 cell 的垂直补偿，让同一行的基线对齐。 */
static int cjk_dy(pk_aa_size_t aa, int cjk_cell_h)
{
    const int d = (pk_aa_cell_h(aa) - cjk_cell_h) / 2;
    return d > 0 ? d : 0;
}

/* 单个 ASCII 字符（含 0x00B0 度数符号）走抗锯齿字体；返回推进宽度。 */
static int aa_putc(uint16_t *fb, int fb_w, int fb_h, int x, int y,
                   uint32_t cp, uint16_t color, pk_aa_size_t size)
{
    const char tmp[2] = { (cp == 0x00B0) ? (char)0x7F : (char)cp, '\0' };
    pk_aa_puts(fb, fb_w, fb_h, x, y, tmp, color, size);
    return pk_aa_cell_w(size);
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
    return pk_aa_cell_w(aa_size_for_cjk(cjk_cell_h_for_scale(ascii_scale)));
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
                                          : pk_aa_cell_w(PK_AA_L);
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
            w += pk_aa_cell_w(PK_AA_M);
        }
    }
    return w;
}

int pk_text_puts(uint16_t *fb, int fb_w, int fb_h,
                 int x, int y, const char *s,
                 uint16_t color, int ascii_scale)
{
    if (ascii_scale < 1) ascii_scale = 1;
    const int ch = cjk_cell_h_for_scale(ascii_scale);
    const pk_aa_size_t aa = aa_size_for_cjk(ch);
    const int dy = cjk_dy(aa, ch);
    int x0 = x;
    while (s && *s) {
        uint32_t cp = utf8_next(&s);
        if (cp == 0) break;

        if (cp <= 0x7F || cp == 0x00B0) {
            x += aa_putc(fb, fb_w, fb_h, x, y, cp, color, aa);
            continue;
        }

        const uint8_t *glyph = cjk_glyph_for_scale(cp, ascii_scale);
        if (glyph != NULL) {
            int cw = cjk_cell_w_for_scale(ascii_scale);
            put_cjk(fb, fb_w, fb_h, x, y + dy, glyph, cw, ch, color, true);
            x += cw;
        } else {
            x += aa_putc(fb, fb_w, fb_h, x, y, '?', color, aa);
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
            x += aa_putc(fb, fb_w, fb_h, x, y, cp, color, PK_AA_L);
            continue;
        }

        const uint8_t *glyph = pk_text_cjk_glyph(cp);
        if (glyph != NULL) {
            put_cjk(fb, fb_w, fb_h, x,
                    y + cjk_dy(PK_AA_L, PK_TEXT_CJK_CELL_H), glyph,
                    PK_TEXT_CJK_CELL_W, PK_TEXT_CJK_CELL_H, color, false);
            x += PK_TEXT_CJK_CELL_W;
        } else {
            x += aa_putc(fb, fb_w, fb_h, x, y, '?', color, PK_AA_L);
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
            put_cjk(fb, fb_w, fb_h, x,
                    y + cjk_dy(PK_AA_M, PK_TEXT_CJK_UI_CELL_H), glyph,
                    cw, PK_TEXT_CJK_UI_CELL_H, color, false);
            x += cw;
        } else {
            x += aa_putc(fb, fb_w, fb_h, x, y, '?', color, PK_AA_M);
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

        if (cp <= 0x7F || cp == 0x00B0) {
            x += aa_putc(fb, fb_w, fb_h, x, y, cp, color, PK_AA_L);
            continue;
        }

        const uint8_t *glyph = pk_text_cjk_glyph(cp);
        if (glyph != NULL) {
            put_cjk(fb, fb_w, fb_h, x,
                    y + cjk_dy(PK_AA_L, PK_TEXT_CJK_CELL_H), glyph,
                    PK_TEXT_CJK_CELL_W, PK_TEXT_CJK_CELL_H, color, false);
            x += PK_TEXT_CJK_CELL_W;
        } else {
            x += aa_putc(fb, fb_w, fb_h, x, y, '?', color, PK_AA_L);
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

        if (cp <= 0x7F || cp == 0x00B0) {
            x += aa_putc(fb, fb_w, fb_h, x, y, cp, color, PK_AA_M);
            continue;
        }

        uint8_t cw = 0;
        const uint8_t *glyph = pk_text_cjk_ui_glyph(cp, &cw);
        if (glyph != NULL && cw > 0) {
            put_cjk(fb, fb_w, fb_h, x,
                    y + cjk_dy(PK_AA_M, PK_TEXT_CJK_UI_CELL_H), glyph,
                    cw, PK_TEXT_CJK_UI_CELL_H, color, false);
            x += cw;
        } else {
            x += aa_putc(fb, fb_w, fb_h, x, y, '?', color, PK_AA_M);
        }
    }
    return x - x0;
}
