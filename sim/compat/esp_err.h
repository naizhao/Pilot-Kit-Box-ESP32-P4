/*
 * esp_err.h — PC 模拟器的最小实现。
 *
 * display.h 会 include 它（因为 pk_display_init() 等返回 esp_err_t）。
 * 模拟器只用到 display.h 里的 PK_DISPLAY_W/H 宏和 pk_rgb565()，
 * 那些真正操作硬件的函数不会被链接，所以这里只需要类型存在即可。
 */
#pragma once

typedef int esp_err_t;

#define ESP_OK           0
#define ESP_FAIL        -1
#define ESP_ERR_NO_MEM   0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
