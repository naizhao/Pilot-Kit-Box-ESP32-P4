/*
 * lv_backend.h — 模拟器的 LVGL 渲染层。
 *
 * 为什么 PFD 要经过 LVGL 而不是继续直写 framebuffer
 * -------------------------------------------------
 * 4.3″ 版本要上触摸导航（FAB、dock、二级页面、Toast），那些是 LVGL 控件。
 * 控件必须能叠在 PFD 之上并与之做 alpha 混合，所以 PFD 不能再独占整块
 * framebuffer —— 它得变成 LVGL 图层里的一层。
 *
 * 采用的是 spec 阶段 4 定的最小改动路径：**PFD 的绘制逻辑一行不改**，
 * 只是把它写入的目标从裸 framebuffer 换成一个铺满全屏的 lv_canvas 的
 * buffer。pk_pfd_*_render() 的签名与实现原样保留。
 *
 * 字节序
 * ------
 * 本项目的 pk_rgb565() 在源头就把颜色转成大端（ST7789 的 SPI 线序），而
 * LVGL 默认按主机序解释 RGB565。v9.5 原生支持 LV_COLOR_FORMAT_RGB565_SWAPPED
 * 并带专门的混合路径，所以这里显式声明该格式即可 —— 不必去动 pk_rgb565
 * 的约定，也就避免了一次会波及现役固件的字节序改动。
 */
#pragma once

#include <stdint.h>

/*
 * 初始化 LVGL、显示器与全屏 canvas。
 * 返回 canvas 的像素缓冲 —— 把它当作原来的 framebuffer 传给 pk_pfd_*_render。
 */
uint16_t *pk_sim_lv_init(void);

/*
 * 跑一帧：标记 canvas 失效 → 让 LVGL 合成 → 返回最终画面缓冲。
 * dt_ms 推进 LVGL 的时间基准（动画与定时器据此走）。
 */
uint16_t *pk_sim_lv_render(uint32_t dt_ms);
