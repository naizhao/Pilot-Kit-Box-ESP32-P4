/*
 * text.h — UTF-8 混排文本渲染（ASCII 走 AA 字体，CJK 走生成的位图子集）。
 *
 * 只剩校准向导 cal_wizard.c 这一个使用者。settings / diag / about / list /
 * traffic 各页都已改走 pfd_aa_text 的 pk_aa_puts（中西文同一份 AA 字体）。
 *
 * 2026-07-30 移除了 pk_text_puts_ui / pk_text_puts_page_title /
 * pk_text_puts_page_body / pk_text_ui_width：它们只服务两页里已删除的
 * *_render_legacy()，其中三个还吊着 12×12 UI 档字库 text_font_cjk_ui.c。
 * 硬件已换成 4.3″ 800×480 触摸屏，不会再退回 2.4″ 逐行版面。
 */
#pragma once

#include <stdint.h>

int pk_text_width(const char *s, int ascii_scale);
int pk_text_title_width(const char *s);

int pk_text_puts(uint16_t *fb, int fb_w, int fb_h,
                 int x, int y, const char *s,
                 uint16_t color, int ascii_scale);

int pk_text_puts_title(uint16_t *fb, int fb_w, int fb_h,
                       int x, int y, const char *s,
                       uint16_t color);
