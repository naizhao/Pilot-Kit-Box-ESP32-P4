/*
 * lv_port.h — LVGL 在固件侧的移植层（模拟器对应 sim/lv_backend.c）。
 *
 * 结构与模拟器保持一致：PFD 画进一块铺满全屏的 lv_canvas，触摸控件叠在其上，
 * 由 LVGL 合成。两边的图层关系必须相同，否则模拟器验过的布局在真机上不成立。
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

/* 初始化 LVGL 与显示器。必须在 pk_display_init() 之后调用——它要拿
 * framebuffer 作为 LVGL 的绘制缓冲。 */
esp_err_t pk_lv_port_init(void);

/*
 * 取得（首次调用时创建）铺满全屏的背景 canvas，返回它的像素缓冲——那就是
 * PFD 各渲染器的绘制目标，用法与原来的 pk_display_framebuffer() 完全一样。
 *
 * 缓冲单独分配在 PSRAM，而不是复用 display 的 framebuffer：LVGL 合成时要把
 * canvas 混到 display 缓冲上，两者同为一块内存的话，源和目的重叠。
 */
uint16_t *pk_lv_port_canvas_px(void);

/* 标记 canvas 已被直接改写，下一帧需要重绘。 */
void pk_lv_port_invalidate(void);

/* 推进 LVGL 的时间基准并跑一次任务处理。由 UI 任务每帧调用。 */
void pk_lv_port_tick(uint32_t elapsed_ms);

/* 诊断：取出并清零 flush（PPA 旋转 + 等 VSYNC）的累计耗时与次数。 */
void pk_lv_port_flush_stats(int64_t *us, uint32_t *cnt);
