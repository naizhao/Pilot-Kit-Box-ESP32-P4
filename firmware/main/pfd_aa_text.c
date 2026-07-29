/*
 * pfd_aa_text.c — 抗锯齿文本渲染实现。
 *
 * 与 pfd_font.c 里 cockpit 渲染器的关键差别：那套字形是 1-bit 的
 * （生成时值只有 0x0 / 0xF），所以它对非零 alpha 直接写纯色即可。
 * 本模块的字形是真灰度（0..15 连续），必须逐像素与背景混合，否则
 * 边缘会退化成硬边，等于白白丢掉抗锯齿。
 */

#include "pfd_aa_text.h"

#include <stddef.h>

#include "display.h"
#include "pfd_draw.h"

/* ── 字形表查找 ───────────────────────────────────────────── */

typedef struct {
    const uint8_t *bitmap[2];   /* [PK_AA_REGULAR] / [PK_AA_BOLD] */
    int            cell_w;
    int            cell_h;
    unsigned       last_code;   /* 该档实际覆盖到的末位码 */
} aa_face_t;

/* XS 与 XL 两档只覆盖到 0x3F：前者服务交通目标的相对高度标签、后者服务
 * PFD 当前值，显示的都是纯数字（带正负号），存整套字母纯属浪费——app 分区
 * 实测只剩 471 KB。落在覆盖范围外的字符按空格处理（见 aa_putchar）。 */
static const aa_face_t s_faces[PK_AA_SIZE_COUNT] = {
    [PK_AA_XS] = { { pk_aa_xs_regular, pk_aa_xs_bold },
                   PK_AA_XS_W, PK_AA_XS_H, PK_AA_XS_LAST },
    [PK_AA_S]  = { { pk_aa_s_regular,  pk_aa_s_bold  },
                   PK_AA_S_W,  PK_AA_S_H,  PK_AA_S_LAST  },
    [PK_AA_M]  = { { pk_aa_m_regular,  pk_aa_m_bold  },
                   PK_AA_M_W,  PK_AA_M_H,  PK_AA_M_LAST  },
    [PK_AA_L]  = { { pk_aa_l_regular,  pk_aa_l_bold  },
                   PK_AA_L_W,  PK_AA_L_H,  PK_AA_L_LAST  },
    [PK_AA_XL] = { { pk_aa_xl_regular, pk_aa_xl_bold },
                   PK_AA_XL_W, PK_AA_XL_H, PK_AA_XL_LAST },
};

/* CJK 段。与拉丁**同一份字体资源**（gen_pfd_aa_font.py 一次生成），关键是
 * cell 高度与同档拉丁一致——混排时基线天然对上，调用方不必再算垂直补偿。
 *
 * 只有 S 与 M 两档带中文：XS 服务交通目标的高度标签、XL 服务 PFD 当前值，
 * 两者都是纯数字带正负号。 */
typedef struct {
    const uint8_t *bitmap[2];
    int            cell_w;
    int            cell_h;
} aa_cjk_face_t;

static const aa_cjk_face_t s_cjk_faces[PK_AA_SIZE_COUNT] = {
    [PK_AA_XS] = { { pk_aa_xs_cjk_regular, pk_aa_xs_cjk_bold },
                   PK_AA_XS_CJK_W, PK_AA_XS_CJK_H },
    [PK_AA_S]  = { { pk_aa_s_cjk_regular,  pk_aa_s_cjk_bold  },
                   PK_AA_S_CJK_W,  PK_AA_S_CJK_H  },
    [PK_AA_M]  = { { pk_aa_m_cjk_regular,  pk_aa_m_cjk_bold  },
                   PK_AA_M_CJK_W,  PK_AA_M_CJK_H  },
    [PK_AA_L]  = { { pk_aa_l_cjk_regular,  pk_aa_l_cjk_bold  },
                   PK_AA_L_CJK_W,  PK_AA_L_CJK_H  },
};

static pk_aa_weight_t s_weight = PK_AA_REGULAR;

/* 码位表升序，二分查。返回字形序号，没有则 -1。 */
static int cjk_index(uint32_t cp)
{
    int lo = 0, hi = PK_AA_CJK_COUNT - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const uint32_t v = pk_aa_cjk_codes[mid];
        if (v == cp) return mid;
        if (v < cp) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

/* 解一个 UTF-8 码点，推进指针。非法字节按 U+FFFD 处理并前进一字节。 */
static uint32_t utf8_next(const unsigned char **ps)
{
    const unsigned char *p = *ps;
    const unsigned char c = *p;
    if (c < 0x80)        { *ps = p + 1; return c; }
    if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        *ps = p + 2; return ((uint32_t)(c & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *ps = p + 3;
        return ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6)
             | (p[2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80
        && (p[3] & 0xC0) == 0x80) {
        *ps = p + 4;
        return ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12)
             | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    }
    *ps = p + 1;
    return 0xFFFD;
}

void pk_aa_set_weight(pk_aa_weight_t w)
{
    s_weight = (w == PK_AA_BOLD) ? PK_AA_BOLD : PK_AA_REGULAR;
}

pk_aa_weight_t pk_aa_get_weight(void) { return s_weight; }

int pk_aa_cell_w(pk_aa_size_t size)
{
    if (size < 0 || size >= PK_AA_SIZE_COUNT) size = PK_AA_M;   /* 兜底取 normal */
    return s_faces[size].cell_w;
}

int pk_aa_cell_h(pk_aa_size_t size)
{
    if (size < 0 || size >= PK_AA_SIZE_COUNT) size = PK_AA_M;   /* 兜底取 normal */
    return s_faces[size].cell_h;
}

/* ── alpha 混合 ───────────────────────────────────────────────
 *
 * framebuffer 里的 RGB565 是**大端**存放（见 display.h 的 pk_rgb565，
 * 它把主机序结果做了字节交换以匹配面板的线序）。混合前必须先换回
 * 主机序，算完再换回去 —— 直接按大端字节做插值会串到相邻通道。
 */
static inline uint16_t bswap16(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}

static inline uint16_t blend565(uint16_t dst_be, uint16_t src_be, uint8_t a4)
{
    if (a4 >= 15) return src_be;            /* 全不透明，省掉一次读改写 */

    uint16_t d = bswap16(dst_be);
    uint16_t s = bswap16(src_be);

    uint32_t dr = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, db = d & 0x1F;
    uint32_t sr = (s >> 11) & 0x1F, sg = (s >> 5) & 0x3F, sb = s & 0x1F;

    /* a4 ∈ [0,15]：用 /15 而非 /16，保证 a4=15 时正好等于前景色。 */
    uint32_t r = (sr * a4 + dr * (15 - a4)) / 15;
    uint32_t g = (sg * a4 + dg * (15 - a4)) / 15;
    uint32_t b = (sb * a4 + db * (15 - a4)) / 15;

    return bswap16((uint16_t)((r << 11) | (g << 5) | b));
}

/* ── 绘制 ─────────────────────────────────────────────────── */

void pk_aa_blit_4bpp(uint16_t *fb, int fb_w, int fb_h, int x, int y,
                     const uint8_t *bitmap, int w, int h, uint16_t color)
{
    for (int row = 0; row < h; ++row) {
        int yy = y + row;
        if (yy < 0 || yy >= fb_h) continue;
        uint16_t *line = fb + (size_t)yy * fb_w;

        for (int col = 0; col < w; ++col) {
            int idx = row * w + col;
            uint8_t packed = bitmap[idx >> 1];
            uint8_t a4 = (idx & 1) ? (packed & 0x0F) : (uint8_t)(packed >> 4);
            if (!a4) continue;

            int xx = x + col;
            if (xx < 0 || xx >= fb_w) continue;
            line[xx] = blend565(line[xx], color, a4);
        }
    }
}

static void aa_putchar(uint16_t *fb, int fb_w, int fb_h,
                       int x, int y, unsigned code,
                       uint16_t color, const aa_face_t *face)
{
    if (code < PK_AA_FIRST_CODE || code > face->last_code) code = 0x20;
    if (code == 0x20) return;               /* 空格无墨，只推进 */

    const int cw = face->cell_w, ch = face->cell_h;
    /* 字节跨距要向上取整：4bpp 下一个 cell 占 ceil(w*h/2) 字节，生成脚本
     * （glyph_bytes_4bpp）就是这么算的。写成 (cw*ch)/2 在**像素数为奇数**的
     * cell 上会每字形少 1 字节，偏移一路累积，字形取到相邻字的一半——屏上
     * 表现为字符挤在一起、笔画糊成一团。
     *
     * 这个错误藏了很久：早先四档的 cell 分别是 390/540/960/2368 像素，恰好
     * 全是偶数，取整与否没区别。新加的 L 档 21×37=777 才把它暴露出来。 */
    const uint8_t *glyph = face->bitmap[s_weight]
                         + (size_t)(code - PK_AA_FIRST_CODE)
                           * (((size_t)cw * ch + 1) / 2);

    pk_aa_blit_4bpp(fb, fb_w, fb_h, x, y, glyph, cw, ch, color);
}

int pk_aa_puts(uint16_t *fb, int fb_w, int fb_h,
               int x, int y, const char *s,
               uint16_t color, pk_aa_size_t size)
{
    if (!fb || !s) return 0;
    if (size < 0 || size >= PK_AA_SIZE_COUNT) size = PK_AA_M;   /* 兜底取 normal */

    const aa_face_t *face = &s_faces[size];
    const aa_cjk_face_t *cjk = &s_cjk_faces[size];
    int advance = 0;

    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp = utf8_next(&p);

        /* 沿用 pfd_font.h 的约定：'~' 与 U+00B0 均映射到度数符号槽位。 */
        if (cp == '~' || cp == 0x00B0) cp = 0x7F;

        if (cp <= 0x7F) {
            aa_putchar(fb, fb_w, fb_h, x + advance, y,
                       (unsigned)cp, color, face);
            advance += face->cell_w;
            continue;
        }

        /* 非拉丁：查 CJK 段。该档没有中文（XS/XL）或字库里没这个字时，
         * 退回一个空位——宁可留白，也不要画出错位的字形。加字的正确做法是
         * 改 i18n catalog 后重跑 gen_pfd_aa_font.py。 */
        const int gi = (cjk->bitmap[0] != NULL) ? cjk_index(cp) : -1;
        if (gi < 0) {
            advance += face->cell_w;
            continue;
        }
        const int cw = cjk->cell_w, ch = cjk->cell_h;
        const uint8_t *glyph = cjk->bitmap[s_weight]
                             + (size_t)gi * (((size_t)cw * ch + 1) / 2);
        pk_aa_blit_4bpp(fb, fb_w, fb_h, x + advance, y, glyph, cw, ch, color);
        advance += cw;
    }
    return advance;
}
