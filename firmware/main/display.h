/*
 * display.h — ST7789 LCD bring-up + framebuffer for the Pilot Kit PFD.
 *
 * Targets the TK024F3036 module (Sitronix ST7789 controller, 2.41",
 * 240(W) × 320(H), 4-wire SPI mode via the user's driver-board adapter).
 * Pin assignments live in docs/hardware/board_pinout.md; the macros
 * below are kept here for the few places — sdkconfig.defaults aside —
 * that need to wire them up.
 *
 * Phase 4 is split:
 *   4a (this file)  — panel init + LEDC backlight + a single framebuffer
 *                     in PSRAM + a draw-bitmap blit helper. Plus a
 *                     test-pattern entry point that fills the screen
 *                     with a vertical RGB565 gradient so first-light
 *                     can be confirmed before the PFD code lands.
 *   4b              — BNO085 IMU driver feeds quaternions to…
 *   4c              — …pfd_render.c which paints into our framebuffer.
 *   4d              — double-buffer / GDMA tuning for 60 FPS.
 *
 * All draw operations are RGB565, big-endian on the wire (ST7789's
 * native byte order in 16-bit colour mode).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define PK_DISPLAY_W           240
#define PK_DISPLAY_H           320
#define PK_DISPLAY_BPP         2          /* RGB565 */
#define PK_DISPLAY_FB_BYTES    (PK_DISPLAY_W * PK_DISPLAY_H * PK_DISPLAY_BPP)

/* SPI / control pin assignments (Waveshare ESP32-P4-WIFI6 header
 * exposure constraints — GPIO 9-19 and 34-45 aren't on the user
 * headers, so all six SPI/control lines route through the GPIO
 * matrix. Fine at our 40 MHz target; the matrix adds ~2 ns of
 * propagation delay which is well within ST7789's setup/hold). */
#define PK_LCD_SPI_HOST        SPI2_HOST
#define PK_LCD_PIN_SCLK        47
#define PK_LCD_PIN_MOSI        33
#define PK_LCD_PIN_CS          46
#define PK_LCD_PIN_DC          48
/* Many TK024F3036-class 7-pin ST7789 modules don't expose RST — the
 * breakout has an on-board RC reset network tying RST to VCC. Set to
 * -1 to skip the GPIO toggle; esp_lcd_panel_st7789 falls back to a
 * software SWRESET command (0x01) over SPI, which the chip honours
 * the same way. If your module DOES expose RST, set this to the
 * GPIO you connected it to (49 was the original assignment). */
#define PK_LCD_PIN_RST         -1
#define PK_LCD_PIN_BL          50

#define PK_LCD_SPI_HZ          (40 * 1000 * 1000)  /* 40 MHz; ST7789 max ≈ 80 */
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
 * Push one rectangle of RGB565 pixels to the panel. The caller owns
 * the pixel buffer (must remain valid until the blocking flush
 * returns). All coordinates are clipped to the panel rectangle.
 */
esp_err_t pk_display_draw(uint16_t x0, uint16_t y0,
                          uint16_t x1, uint16_t y1,
                          const uint16_t *pixels);

/*
 * Pointer to the firmware-owned single framebuffer in PSRAM
 * (PK_DISPLAY_W × PK_DISPLAY_H × 2 B). Phase 4c writes here; the
 * 4d double-buffer split will redefine this getter.
 */
uint16_t *pk_display_framebuffer(void);

/*
 * Push the entire framebuffer to the panel synchronously. Phase 4a/c
 * use this; 4d swaps it for an asynchronous queue + GDMA flush.
 */
esp_err_t pk_display_flush_full(void);

/*
 * Phase 4a sanity check: paint a vertical R→G→B gradient into the
 * framebuffer and push it once. If you see a clean sweep, SPI + panel
 * init + backlight + memory access ordering are all working.
 */
void pk_display_test_pattern(void);
