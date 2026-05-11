/*
 * display.c — Phase 4a ST7789 LCD bring-up.
 *
 * Uses the ESP-IDF v6 built-in esp_lcd stack:
 *   esp_lcd_new_panel_io_spi  → DC/CS framing on top of a regular
 *                               spi_master device
 *   esp_lcd_new_panel_st7789  → init sequence (sleep-out, color mode,
 *                               MADCTL, INVON, etc.) provided by the
 *                               driver in components/esp_lcd
 *
 * The single framebuffer sits in PSRAM (~150 KiB) — internal SRAM is
 * tight once the IQ ring buffer, URB pool and BLE stack are all live.
 * Drawing happens directly into it; pk_display_flush_full() chunks
 * the buffer to the panel via esp_lcd_panel_draw_bitmap() which in
 * turn uses GDMA for the SPI transfer.
 *
 * The backlight is driven by an LEDC PWM channel rather than a fixed
 * GPIO so Phase 4d can implement smooth dim-on / dim-off plus reflect
 * configurable brightness once the buttons + UI land.
 */

#include "display.h"

#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"

static const char *TAG = "display";

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t    s_panel;
static uint16_t                 *s_fb;

#define BL_LEDC_TIMER     LEDC_TIMER_0
#define BL_LEDC_MODE      LEDC_LOW_SPEED_MODE
#define BL_LEDC_CHANNEL   LEDC_CHANNEL_0
#define BL_LEDC_DUTY_BITS LEDC_TIMER_8_BIT       /* 0..255 maps directly */

/* --- Backlight ------------------------------------------------------- */

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t t = {
        .speed_mode      = BL_LEDC_MODE,
        .timer_num       = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_DUTY_BITS,
        .freq_hz         = PK_LCD_BL_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&t);
    if (err != ESP_OK) return err;

    const ledc_channel_config_t ch = {
        .gpio_num   = PK_LCD_PIN_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = 0,           /* off until first frame is ready */
        .hpoint     = 0,
    };
    return ledc_channel_config(&ch);
}

void pk_display_set_brightness(uint8_t level)
{
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, level);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

/* --- Panel ----------------------------------------------------------- */

esp_err_t pk_display_init(void)
{
    /* 1. Backlight off; we'll fade it on after the first frame. */
    esp_err_t err = backlight_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight_init: %s", esp_err_to_name(err));
        return err;
    }

    /* 2. SPI bus.
     *    max_transfer_sz covers a whole 240×320×2 = 153,600 B frame so
     *    a full flush is one DMA descriptor chain. */
    const spi_bus_config_t bus = {
        .sclk_io_num     = PK_LCD_PIN_SCLK,
        .mosi_io_num     = PK_LCD_PIN_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = PK_DISPLAY_FB_BYTES,
    };
    err = spi_bus_initialize(PK_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return err;
    }

    /* 3. Panel IO layer (DC framing, CS automatic). */
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num         = PK_LCD_PIN_CS,
        .dc_gpio_num         = PK_LCD_PIN_DC,
        .spi_mode            = 0,
        .pclk_hz             = PK_LCD_SPI_HZ,
        .trans_queue_depth   = 10,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
    };
    err = esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)PK_LCD_SPI_HOST, &io_cfg, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new_panel_io_spi: %s", esp_err_to_name(err));
        return err;
    }

    /* 4. ST7789 vendor driver. */
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PK_LCD_PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new_panel_st7789: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    /* ST7789 ships with INVON in most modules; flip if Phase 4a test
     * pattern comes back inverted. The TK024F3036 module datasheet
     * doesn't say either way — measure on first light. */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    /* 5. Framebuffer in PSRAM. */
    s_fb = heap_caps_aligned_alloc(64, PK_DISPLAY_FB_BYTES,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "PSRAM framebuffer alloc (%u B) failed",
                 (unsigned)PK_DISPLAY_FB_BYTES);
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb, 0, PK_DISPLAY_FB_BYTES);

    ESP_LOGI(TAG, "ST7789 %dx%d ready, framebuffer @ %p (%u KiB PSRAM)",
             PK_DISPLAY_W, PK_DISPLAY_H, (void *)s_fb,
             (unsigned)(PK_DISPLAY_FB_BYTES / 1024));
    return ESP_OK;
}

uint16_t *pk_display_framebuffer(void)
{
    return s_fb;
}

esp_err_t pk_display_draw(uint16_t x0, uint16_t y0,
                          uint16_t x1, uint16_t y1,
                          const uint16_t *pixels)
{
    if (s_panel == NULL || pixels == NULL) return ESP_ERR_INVALID_STATE;
    if (x0 >= PK_DISPLAY_W) x0 = PK_DISPLAY_W - 1;
    if (y0 >= PK_DISPLAY_H) y0 = PK_DISPLAY_H - 1;
    if (x1 >  PK_DISPLAY_W) x1 = PK_DISPLAY_W;
    if (y1 >  PK_DISPLAY_H) y1 = PK_DISPLAY_H;
    if (x1 <= x0 || y1 <= y0) return ESP_ERR_INVALID_ARG;
    return esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, pixels);
}

esp_err_t pk_display_flush_full(void)
{
    return pk_display_draw(0, 0, PK_DISPLAY_W, PK_DISPLAY_H, s_fb);
}

void pk_display_test_pattern(void)
{
    if (s_fb == NULL) return;
    /* Vertical sweep: top → red, middle → green, bottom → blue.
     * This is a cheap way to see panel orientation, MADCTL byte order
     * (if RGB and BGR are swapped, the gradient runs blue-green-red),
     * and the backlight working all at once. */
    for (int y = 0; y < PK_DISPLAY_H; ++y) {
        uint8_t r = 0, g = 0, b = 0;
        if (y < PK_DISPLAY_H / 3) {
            r = (uint8_t)(255 * y / (PK_DISPLAY_H / 3));
        } else if (y < (2 * PK_DISPLAY_H) / 3) {
            g = (uint8_t)(255 * (y - PK_DISPLAY_H / 3) / (PK_DISPLAY_H / 3));
        } else {
            b = (uint8_t)(255 * (y - (2 * PK_DISPLAY_H) / 3) / (PK_DISPLAY_H / 3));
        }
        uint16_t c = pk_rgb565(r, g, b);
        for (int x = 0; x < PK_DISPLAY_W; ++x) {
            s_fb[y * PK_DISPLAY_W + x] = c;
        }
    }
    pk_display_flush_full();
    pk_display_set_brightness(180);  /* ≈ 70%, comfortable indoors */
    ESP_LOGI(TAG, "test pattern flushed; backlight @ 70%%");
}
