/*
 * pfd_font.h — minimal 5×7 ASCII font + draw helpers for the PFD.
 *
 * Drop-in dependency-free renderer aimed at the PFD's numeric readouts
 * and the heading-tape / bank-arc labels. The glyph table mirrors the
 * canonical 5×7 ASCII font (public domain, widely circulated as
 * "font5x7"); each character is 5 columns × 7 rows packed column-major
 * with bit 0 = top row.
 *
 * Two rendering scales are supported. Scale 1 → 5 px wide × 7 px tall
 * (with a 1-px gap → 6×8 effective cell). Scale 2 doubles every pixel
 * → 10×14 visible (12×16 cell), legible from arm's length on the
 * 240×320 ST7789 panel.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#define PK_FONT_W     5
#define PK_FONT_H     7
#define PK_FONT_CELL_W(scale)  ((PK_FONT_W + 1) * (scale))
#define PK_FONT_CELL_H(scale)  ((PK_FONT_H + 1) * (scale))

/* Special character for ° (degree symbol). ASCII has no canonical
 * encoding for this; we hijack 0x7F (DEL) in our glyph table. The
 * pk_font_puts() helper translates a literal C "\xb0" or '~' (which
 * we don't otherwise emit) into 0x7F at call-time. */
#define PK_FONT_DEGREE  0x7F

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
