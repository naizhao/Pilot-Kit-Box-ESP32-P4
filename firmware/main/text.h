/*
 * text.h — UTF-8 UI text renderer.
 *
 * pk_text_puts() keeps legacy ASCII on the crisp 5x7 bitmap font and uses
 * thresholded generated CJK subsets for non-ASCII text. pk_text_puts_ui() is
 * a separate 4bpp alpha path for dense mixed-language UI pages.
 */
#pragma once

#include <stdint.h>

int pk_text_width(const char *s, int ascii_scale);
int pk_text_title_width(const char *s);
int pk_text_ui_width(const char *s);

int pk_text_puts(uint16_t *fb, int fb_w, int fb_h,
                 int x, int y, const char *s,
                 uint16_t color, int ascii_scale);

int pk_text_puts_title(uint16_t *fb, int fb_w, int fb_h,
                       int x, int y, const char *s,
                       uint16_t color);

int pk_text_puts_ui(uint16_t *fb, int fb_w, int fb_h,
                    int x, int y, const char *s,
                    uint16_t color);

int pk_text_puts_page_title(uint16_t *fb, int fb_w, int fb_h,
                            int x, int y, const char *s,
                            uint16_t color);

int pk_text_puts_page_body(uint16_t *fb, int fb_w, int fb_h,
                           int x, int y, const char *s,
                           uint16_t color);
