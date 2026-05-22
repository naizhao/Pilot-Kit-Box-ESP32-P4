/*
 * pfd_font.h — PFD font helpers.
 *
 * Scale-1 and normal scaled rendering use a dependency-free 5×7 ASCII
 * bitmap. PFD numeric readouts can opt into a generated 1-bit cockpit
 * glyph subset in `pfd_font_aa.c`, preserving the fixed 12×16 cell
 * without gray antialiasing fringes or TTF hinting artifacts.
 *
 * Any integer scale ≥ 1 is supported by the normal bitmap renderer. The
 * cockpit renderer is deliberately scale-2 only and should be used only
 * for compact avionics readouts, not for general UI text or future
 * UTF-8/CJK text.
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
#define PK_FONT_CELL_H(scale)  ((PK_FONT_H + 1) * (scale))

/* Special character for ° (degree symbol). ASCII has no canonical
 * encoding for this; we hijack 0x7F (DEL) in our glyph table. The
 * pk_font_puts() helper translates a literal C "\xb0" or '~' (which
 * we don't otherwise emit) into 0x7F at call-time. */
#define PK_FONT_DEGREE  0x7F

/* Custom glyphs at 0x80..0x87 — eight-direction compass arrows for
 * traffic-display annotations (HDG column shows relative bearing
 * compared to own-ship; VS column re-uses N/S for climb/descent). The
 * ordering is the standard 45° boxed-compass sequence starting at N. */
#define PK_FONT_ARROW_N    ((char)0x80)
#define PK_FONT_ARROW_NE   ((char)0x81)
#define PK_FONT_ARROW_E    ((char)0x82)
#define PK_FONT_ARROW_SE   ((char)0x83)
#define PK_FONT_ARROW_S    ((char)0x84)
#define PK_FONT_ARROW_SW   ((char)0x85)
#define PK_FONT_ARROW_W    ((char)0x86)
#define PK_FONT_ARROW_NW   ((char)0x87)

/*
 * Convert a relative-bearing delta in degrees (positive = right of
 * own-ship's nose, negative = left, wraps at ±180) into one of the
 * 8 compass-arrow characters above. 45°-wide sectors centred on each
 * cardinal/intercardinal bearing — e.g. a delta of +60° → NE arrow,
 * -160° → SW arrow.
 */
char pk_font_arrow_for_delta_deg(int delta_deg);

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

/* Render a null-terminated string with the generated 12×16 cockpit
 * glyphs. Characters missing from the generated subset fall back to
 * the normal scale-2 bitmap font. Returns the fixed 12 px per-glyph
 * advance. */
int pk_font_puts_cockpit(uint16_t *fb, int fb_w, int fb_h,
                         int x, int y, const char *s,
                         uint16_t color);
