/*
 * pfd_font.h — PFD font helpers.
 *
 * Scale-1 and normal scaled rendering use a dependency-free 5×7 ASCII
 * bitmap.
 *
 * 2026-07-30：cockpit 12×16 字形子集（pfd_font_aa.c）连同
 * pk_font_puts_cockpit() 一并删除——它只服务 320×240 版面，而小屏兼容预览
 * 已停止维护。正文用字一律走 pfd_aa_text.c 的抗锯齿字体（含中日韩）；本文件
 * 只留给 1~2 个字符的极小标注（距离环数字、罗盘 N/E/S/W）。
 *
 * Any integer scale ≥ 1 is supported by the bitmap renderer.
 *
 *   scale 1 →  5 ×  7 visible,  6 ×  8 cell
 *   scale 2 → 10 × 14 visible, 12 × 16 cell  (the workhorse for labels)
 *   scale 3 → 15 × 21 visible, 18 × 24 cell  (G1000 ALT / HDG boxes)
 *
 * The single-glyph advance gap is one source pixel × scale (i.e. the
 * gap doubles in scale 2, triples in scale 3). Callers right-justifying
 * compute `len * PK_FONT_CELL_W(scale)`.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#define PK_FONT_W     5
#define PK_FONT_H     7
#define PK_FONT_CELL_W(scale)  ((PK_FONT_W + 1) * (scale))

/* Special character for ° (degree symbol). ASCII has no canonical
 * encoding for this; we hijack 0x7F (DEL) in our glyph table. The
 * pk_font_puts() helper translates a literal C "\xb0" or '~' (which
 * we don't otherwise emit) into 0x7F at call-time. */
#define PK_FONT_DEGREE  0x7F

/* 2026-08-01：0x80..0x87 那八个私有码位（八向罗盘箭头）连同
 * PK_FONT_ARROW_* 宏、pk_font_arrow_for_delta_deg() 与字形表尾部的 8 行
 * 一并删除。各页早已改用 AA 字体的真 UTF-8 箭头（U+2191/U+2193 等），私有
 * 码位喂给 pk_aa_puts 只会解成非法前导字节。字形表从此就是纯 ASCII 0x20..0x7F。
 * 别再往这里加私有码位——要新符号就加进 AA 字库。 */

/*
 * Render one ASCII character into the RGB565 framebuffer.
 *
 *   fb         pointer to the start of a fb_w × fb_h RGB565 buffer
 *   fb_w, fb_h framebuffer dimensions
 *   x, y       top-left of the glyph cell
 *   c          ASCII char in 0x20..0x7F (others render as space)
 *   color      RGB565 colour, native byte order (see pk_rgb565)
 *   scale      1 or 2
 */
void pk_font_putchar(uint16_t *fb, int fb_w, int fb_h,
                     int x, int y, char c,
                     uint16_t color, int scale);

/* Same but for a null-terminated string. Returns the advance in
 * pixels (handy for right-justified layout). */
int pk_font_puts(uint16_t *fb, int fb_w, int fb_h,
                 int x, int y, const char *s,
                 uint16_t color, int scale);

