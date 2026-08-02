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
void pk_pfd_blend_pixel(uint16_t *fb, int x, int y, uint16_t c, uint8_t alpha);
void pk_pfd_fill_rect(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c);

/* 圆角矩形填充（抗锯齿）。r 会被收到 min(w,h)/2，传过大也不会画坏。
 * 由 boot_splash 的正方形版泛化而来，分段控件 / 卡片通用。 */
void pk_pfd_fill_round_rect(uint16_t *fb, int x0, int y0, int x1, int y1,
                            int r, uint16_t c);
void pk_pfd_draw_line(uint16_t *fb, int x0, int y0, int x1, int y1, uint16_t c);
void pk_pfd_draw_line_aa(uint16_t *fb,
                         float x0, float y0, float x1, float y1,
                         float width, uint16_t c);
void pk_pfd_draw_arc_aa(uint16_t *fb,
                        float cx, float cy, float radius,
                        float start_deg, float end_deg,
                        float width, uint16_t c);
void pk_pfd_draw_triangle(uint16_t *fb,
                          int ax, int ay, int bx, int by, int cx, int cy,
                          uint16_t c);

/*
 * 飞机俯视剪影，朝向 rot_deg（0° = 屏幕正上方，顺时针为正，与航向同向）。
 * size 为机身半长；后掠翼外形，小尺寸下也能一眼看出机头指哪。
 *
 * 交通目标此前一律画成菱形——菱形是各向同性的，看不出目标在往哪飞，而
 * 「它朝我来还是背我去」恰恰是防撞时最要紧的一条信息。
 *
 * 放在通用绘制层是因为 PFD 的 HSI 外圈与雷达页要用同一个符号；两处各画
 * 一份迟早走偏。
 */
void pk_pfd_draw_aircraft(uint16_t *fb, int cx, int cy,
                          float rot_deg, int size, uint16_t c);

/*
 * 同一副剪影的空心（仅描边）版本——地面目标专用。
 *
 * 航电惯例：地面目标用扁平/空心符号或异色，与空中目标一眼可辨（本机若把
 * 滑行道上的飞机与低空进近的飞机混画成同一种实心符号，飞行员会把两者
 * 当同一种威胁读，这是安全问题不是美化）。选空心而非纯异色的理由：颜色
 * 在本项目里已经承载「威胁等级」语义（见 traffic_page.c target_color /
 * pfd_hsi_traffic.c 同高度琥珀/高于青/低于蓝灰/无高度灰），再抢一种颜色
 * 表示地面会和这套编码冲突；形状是唯一没被占用的维度。
 */
void pk_pfd_draw_aircraft_outline(uint16_t *fb, int cx, int cy,
                                  float rot_deg, int size, uint16_t c);

uint16_t pk_pfd_rgb565_dither(uint8_t r, uint8_t g, uint8_t b, int x, int y);

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
