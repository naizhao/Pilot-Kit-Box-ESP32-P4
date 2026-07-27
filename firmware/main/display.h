/*
 * display.h — ST7789 LCD bring-up + framebuffer for the Pilot Kit PFD.
 *
 * Targets the TK024F3036 module (Sitronix ST7789 controller, 2.41",
 * 240(W) × 320(H), 4-wire SPI mode via the user's driver-board adapter).
 * Pin assignments live in docs/hardware/board_pinout.md; the macros
 * below are kept here for the few places — sdkconfig.defaults aside —
 * that need to wire them up.
 *
 * Current display stack:
 *   - panel init + LEDC backlight
 *   - single RGB565 framebuffer in PSRAM
 *   - synchronous full-frame SPI flush at roughly 30 FPS
 *   - PFD, ADS-B LIST, SETTINGS, ABOUT, and calibration views rendered
 *     by the UI layer into this framebuffer.
 *
 * All draw operations are RGB565, big-endian on the wire (ST7789's
 * native byte order in 16-bit colour mode).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* 面板分辨率。
 *
 * 用 #ifndef 包裹是为了让 PC 模拟器（sim/）能在编译期覆盖成 800×480，
 * 从而在没有硬件的情况下预览 4.3"/5" 屏的布局。固件自身不覆盖，
 * 仍取下面的默认值。
 *
 * 注意：绘制模块里还散落着大量基于 320×240 推导出来的布局常量
 * （见 pfd_*.c 顶部的 #define），仅改这里不足以正确适配新分辨率 ——
 * 那是分辨率参数化的后续工作。 */
#ifndef PK_DISPLAY_W
#define PK_DISPLAY_W           320
#endif
#ifndef PK_DISPLAY_H
#define PK_DISPLAY_H           240
#endif
#define PK_DISPLAY_BPP         2          /* RGB565 */
#define PK_DISPLAY_FB_BYTES    (PK_DISPLAY_W * PK_DISPLAY_H * PK_DISPLAY_BPP)

/* SPI / control pin assignments on the Waveshare ESP32-P4-WIFI6 left
 * header.
 *
 * SCK/MOSI/CS use GPIO 30/29/28 — these are SPI2's IO_MUX direct pins
 * (SPI2_CK_PAD / SPI2_D_PAD / SPI2_CS_PAD per datasheet Table 2-3),
 * so the lines bypass the GPIO matrix and are eligible for the full
 * 80 MHz ST7789 SPI ceiling. DC is on GPIO 31 — esp_lcd toggles it
 * as a plain GPIO output around every command/parameter boundary, so
 * it doesn't need IO_MUX direct.
 *
 * Right header was previously used but is land-mined: pins 46 and 47
 * are separated by a GND pad, which any multi-pin Dupont housing will
 * short one signal to ground (verified — caused the "init OK but
 * panel uniform pale blue" symptom during early LCD bring-up). The
 * left-header 28/29/30/31 region has the GND below it, not between
 * the signal pads, so a 4- or 5-pin housing is safe here.
 *
 * See docs/hardware/board_pinout.md §1 for the silkscreen-to-GPIO
 * map and §3 for the LCD wiring diagram. */
#define PK_LCD_SPI_HOST        SPI2_HOST
#define PK_LCD_PIN_SCLK        30   /* SPI2_CK_PAD (IO_MUX direct) */
#define PK_LCD_PIN_MOSI        29   /* SPI2_D_PAD  (IO_MUX direct) */
#define PK_LCD_PIN_MISO        -1   /* unused at runtime; would be GPIO 31
                                       (SPI2_Q_PAD) if we ever read RDDID */
#define PK_LCD_PIN_CS          28   /* SPI2_CS_PAD (IO_MUX direct) */
#define PK_LCD_PIN_DC          31   /* plain GPIO output, software-toggled */
/* TK024F304189-SPI breakout has an on-board RC reset (R8=18k, C7=100n)
 * tying RST to VCC, so we don't drive it from the host. esp_lcd_panel
 * falls back to a software SWRESET (cmd 0x01) over SPI. If a future
 * variant exposes RST, set this to the GPIO you wired it to. */
#define PK_LCD_PIN_RST         -1
#define PK_LCD_PIN_BL          50

#define PK_LCD_SPI_HZ          (40 * 1000 * 1000)  /* 40 MHz — comfortable
                                                     on IO_MUX direct
                                                     (SPI2_CK/D/CS_PAD).
                                                     ST7789 spec ceiling is
                                                     ~80 MHz; on a real PCB
                                                     with short traces we can
                                                     try 60-80 later. */
#define PK_LCD_BL_PWM_FREQ_HZ  20000               /* outside audible range */

/*
 * RGB565 helper. Pass 8-bit r/g/b (0..255); output is in the native
 * byte order the ST7789 expects on the SPI wire (big-endian).
 */
static inline uint16_t pk_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (uint16_t)((v >> 8) | (v << 8));  /* swap to big-endian */
}

/*
 * Bring the display up: SPI bus + ST7789 panel + backlight off (it'll
 * come on at the requested brightness as soon as the first frame is
 * pushed). Safe to call once during boot. Returns ESP_OK on success.
 */
esp_err_t pk_display_init(void);

/*
 * Set backlight brightness, 0 (off) .. 255 (max). The LEDC channel
 * is configured in pk_display_init().
 */
void pk_display_set_brightness(uint8_t level);

/*
 * Issue an ST7789 display-off command (DISPOFF, 0x28 — wrapped by
 * esp_lcd_panel_disp_on_off). All liquid-crystal segments turn fully
 * dark regardless of the framebuffer contents. The current MODE sleep
 * path deliberately does not call this on ESP32-P4 rev 1.x hardware
 * because testing showed it can block deep sleep entry; keep it for
 * controlled tests and future board revisions.
 */
void pk_display_panel_off(void);

/*
 * Push one rectangle of RGB565 pixels to the panel. The caller owns
 * the pixel buffer (must remain valid until the blocking flush
 * returns). All coordinates are clipped to the panel rectangle.
 */
esp_err_t pk_display_draw(uint16_t x0, uint16_t y0,
                          uint16_t x1, uint16_t y1,
                          const uint16_t *pixels);

/*
 * Pointer to the firmware-owned single framebuffer in PSRAM
 * (PK_DISPLAY_W × PK_DISPLAY_H × 2 B). UI renderers write here before
 * pk_display_flush_full() pushes the frame to the panel.
 */
uint16_t *pk_display_framebuffer(void);

/*
 * Push the entire framebuffer to the panel synchronously.
 */
esp_err_t pk_display_flush_full(void);

/*
 * Sanity check: paint a vertical R→G→B gradient into the framebuffer
 * and push it once. If you see a clean sweep, SPI + panel init +
 * backlight + memory access ordering are all working.
 */
void pk_display_test_pattern(void);
