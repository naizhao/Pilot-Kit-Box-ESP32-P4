/*
 * display.h — 微雪 ESP32-P4-WIFI6-Touch-LCD-4.3 显示接口。
 *
 * 面板原生为 480×800 ST7701 MIPI-DSI；固件继续向上层提供 800×480
 * 横屏 framebuffer。display.c 在每次提交时用 PPA 顺时针旋转 90°，
 * 上层绘制和 LVGL 端不需要感知面板原生方向。
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PK_DISPLAY_W          800
#define PK_DISPLAY_H          480
#define PK_DISPLAY_BPP        2
#define PK_DISPLAY_FB_BYTES   (PK_DISPLAY_W * PK_DISPLAY_H * PK_DISPLAY_BPP)

#define PK_LCD_NATIVE_W       480
#define PK_LCD_NATIVE_H       800
#define PK_LCD_NATIVE_FB_BYTES \
    (PK_LCD_NATIVE_W * PK_LCD_NATIVE_H * PK_DISPLAY_BPP)

/* 板载 LCD 固定参数，来自实物原理图与微雪官方 BSP。 */
#define PK_LCD_PIN_RST                    27
#define PK_LCD_PIN_BL                     26
#define PK_LCD_PIN_BL_EN                  33
#define PK_LCD_BL_PWM_FREQ_HZ             5000
#define PK_LCD_DSI_LANE_COUNT             2
#define PK_LCD_DSI_LANE_BIT_RATE_MBPS     500
#define PK_LCD_DPI_CLOCK_MHZ              30
#define PK_LCD_DSI_PHY_LDO_CHANNEL        3
#define PK_LCD_DSI_PHY_LDO_MV             2500

/*
 * 历史绘制器和 LVGL framebuffer 都存放 RGB565 swapped。PPA 在旋转时
 * 执行 byte_swap，使 DPI framebuffer 中的数据恢复为面板需要的本机序。
 */
static inline uint16_t pk_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (uint16_t)((v >> 8) | (v << 8));
}

/* 初始化 DSI/DPI、ST7701、PPA、双缓冲与背光；仅在启动时调用一次。 */
esp_err_t pk_display_init(void);

/* 背光亮度：0（关闭）至 255（最亮）。 */
void pk_display_set_brightness(uint8_t level);

/* 当前亮度档（设置页显示用）。硬件侧只有 set，占空比反推不出档位。 */
uint8_t pk_backlight_level_get(void);

/* 发送 ST7701 display-off 命令。 */
void pk_display_panel_off(void);

/*
 * 提交一个逻辑横屏矩形。传入像素按紧凑行排列；函数返回前数据已经复制到
 * 固件 framebuffer，并在下一个 VSYNC 切换到旋转后的 DPI framebuffer。
 */
esp_err_t pk_display_draw(uint16_t x0, uint16_t y0,
                          uint16_t x1, uint16_t y1,
                          const uint16_t *pixels);

/* 返回固件拥有的 800×480 RGB565-swapped PSRAM framebuffer。 */
uint16_t *pk_display_framebuffer(void);

/* 同步提交完整 framebuffer。 */
esp_err_t pk_display_flush_full(void);

/* 显示 R/G/B 渐变，用于真机方向、颜色和全屏覆盖检查。 */
void pk_display_test_pattern(void);

#ifdef __cplusplus
}
#endif

/* 诊断：取出并清零 PPA 旋转 / 等 VSYNC 的累计耗时。 */
void pk_display_flush_split(int64_t *ppa_us, int64_t *wait_us, uint32_t *cnt);
