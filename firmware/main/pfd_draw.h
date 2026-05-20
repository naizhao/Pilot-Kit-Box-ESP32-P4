/*
 * pfd_draw.h — shared 2D rasterization primitives for the PFD task.
 *
 * Extracted from pfd.c so the per-widget files (pfd_attitude, pfd_hsi,
 * pfd_statusbar, pfd_tape) can share a single set of pixel/line/rect/
 * triangle routines. All routines clip to PK_DISPLAY_W/H so a widget
 * can pass any rectangle without bounds-checking first.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

void pk_pfd_put_pixel(uint16_t *fb, int x, int y, uint16_t c);
void pk_pfd_fill_rect(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c);
void pk_pfd_draw_line(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c);
void pk_pfd_draw_triangle(uint16_t *fb,
                          int ax, int ay, int bx, int by, int cx, int cy,
                          uint16_t c);

/*
 * Blend every pixel in the given rectangle toward black by `alpha`/256.
 *   alpha =   0 → no change (no-op)
 *   alpha = 128 → 50% darker  (semi-transparent dark overlay)
 *   alpha = 256 → fully black (equivalent to fill_rect with black)
 *
 * Used by ALT tape and GS/VS readouts to keep the attitude background
 * visible through the overlay. Operates in RGB565 directly so cost is
 * one short unpack/multiply/pack per pixel.
 */
void pk_pfd_darken_rect(uint16_t *fb, int x0, int y0, int x1, int y1, int alpha);
