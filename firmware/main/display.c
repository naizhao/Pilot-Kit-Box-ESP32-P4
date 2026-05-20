/*
 * display.c — Phase 4a TK024F3036 (ST7789 + transflective TFT) LCD
 * bring-up.
 *
 * The panel is the Holocene Technology TK024F3036 240x320 2.4-inch
 * transflective IPS module. Its driver IC is ST7789 BUT the panel needs
 * a vendor-specific init sequence (porch control, VGH/VGL, VCOM, gamma
 * curves) that the upstream esp_lcd_new_panel_st7789 driver doesn't
 * emit — without them the chip accepts SPI commands but never drives
 * the TFT properly and you get "backlight on, no content." So we use
 * the supplier's reference panel driver in components/lcd_tk024f3036
 * (Apache 2.0, copied from the vendor's ESP32-S3 LVGL demo and matched
 * against the panel datasheet).
 *
 * Otherwise the rest of the stack is standard:
 *   esp_lcd_new_panel_io_spi   → DC/CS framing on top of spi_master
 *   esp_lcd_new_panel_TK024F3036 → init sequence + draw_bitmap/etc.
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"             /* esp_rom_delay_us */
#include "LCD_TK024F3036.h"
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

#if 0  /* Bring-up diagnostics — kept here in case we ever return to debug
        * the LCD again. Re-enable by removing the #if 0 / #endif. */

static void lcd_gpio_selftest(const char *name, int gpio)
{
    if (gpio < 0) {
        ESP_LOGI(TAG, "  %s = -1 (not assigned), skipping", name);
        return;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(5));   /* settle */
    int with_pu = gpio_get_level(gpio);

    cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    gpio_config(&cfg);
    vTaskDelay(pdMS_TO_TICKS(5));
    int with_pd = gpio_get_level(gpio);

    const char *verdict;
    if (with_pu == 1 && with_pd == 0) {
        verdict = "OK (responds to PU/PD — wire free or panel high-Z)";
    } else if (with_pu == 0 && with_pd == 0) {
        verdict = "STUCK LOW (shorted to GND or panel driving low)";
    } else if (with_pu == 1 && with_pd == 1) {
        verdict = "STUCK HIGH (shorted to 3V3 or panel driving high)";
    } else {
        verdict = "WEIRD";
    }
    ESP_LOGI(TAG, "  %s (GPIO%d): PU=%d PD=%d → %s",
             name, gpio, with_pu, with_pd, verdict);

    /* leave the pin floating; SPI bus init will reclaim it */
    gpio_reset_pin(gpio);
}

/* Compact bridge test — 5 cycles, no inter-test prompts. */
static void lcd_bridge_test_short(const char *name, int drv, int rcv)
{
    gpio_config_t drv_out = {
        .pin_bit_mask = 1ULL << drv,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&drv_out);
    gpio_config_t rcv_in = {
        .pin_bit_mask = 1ULL << rcv,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&rcv_in);
    int hi = 0;
    for (int i = 0; i < 5; ++i) {
        gpio_set_level(drv, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
        if (gpio_get_level(rcv) == 1) ++hi;
        gpio_set_level(drv, 0);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (hi >= 4) {
        ESP_LOGI(TAG, "  ✓ %s wire CONTINUOUS (%d/5 highs)", name, hi);
    } else if (hi == 0) {
        ESP_LOGW(TAG, "  ✗ %s wire BROKEN (%d/5 highs — bridge not closed)", name, hi);
    } else {
        ESP_LOGW(TAG, "  ? %s wire INTERMITTENT (%d/5 highs — hold bridge steady)", name, hi);
    }
    gpio_reset_pin(drv);
    gpio_reset_pin(rcv);
}

/* Bridge test: drive `drv` HIGH/LOW for 5 cycles, sample `rcv`.
 * Caller bridges drv↔rcv at the LCD-side header before/during the
 * test. If 4+ of 5 HIGH samples come back HIGH, the wires conduct. */
static void lcd_bridge_test(const char *name, int drv, int rcv)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "  ============================================");
    ESP_LOGI(TAG, "  Bridge test: %s ↔ MISO  (you have 5s to switch)", name);
    ESP_LOGI(TAG, "  ============================================");
    ESP_LOGI(TAG, "  → at the LCD-side header, hold a short between %s and MISO pins", name);
    vTaskDelay(pdMS_TO_TICKS(5000));

    gpio_config_t drv_out = {
        .pin_bit_mask = 1ULL << drv,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&drv_out);
    gpio_config_t rcv_in = {
        .pin_bit_mask = 1ULL << rcv,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&rcv_in);

    int hi_match = 0, lo_match = 0;
    for (int i = 0; i < 5; ++i) {
        gpio_set_level(drv, 1);
        vTaskDelay(pdMS_TO_TICKS(300));
        int hi = gpio_get_level(rcv);
        gpio_set_level(drv, 0);
        vTaskDelay(pdMS_TO_TICKS(300));
        int lo = gpio_get_level(rcv);
        ESP_LOGI(TAG, "    cycle %d: %s=H→MISO=%d, %s=L→MISO=%d", i, name, hi, name, lo);
        if (hi == 1) ++hi_match;
        if (lo == 0) ++lo_match;
    }
    if (hi_match >= 4 && lo_match >= 4) {
        ESP_LOGI(TAG, "  ✓ %s wire CONTINUOUS (host pad ↔ LCD pad)", name);
    } else if (hi_match == 0) {
        ESP_LOGW(TAG, "  ✗ %s wire BROKEN (MISO never went HIGH)", name);
    } else {
        ESP_LOGW(TAG, "  ? %s wire INTERMITTENT (%d/5 highs) — keep bridge steady",
                 name, hi_match);
    }
    gpio_reset_pin(drv);
    gpio_reset_pin(rcv);
}

/* Software bit-bang SPI panel ID read. Bypasses ESP-IDF SPI entirely
 * — configures CS/SCK/MOSI/DC as plain GPIO outputs and MISO as
 * input, then manually clocks out 0x04 RDDID and clocks in 4 bytes.
 * Mode 0 (CPOL=0 CPHA=0): host changes MOSI on falling edge of SCK,
 * panel samples MOSI on rising edge; panel changes MISO on falling
 * edge, host samples MISO on rising edge. */
static void bitbang_panel_id(void)
{
    ESP_LOGI(TAG, "Bit-bang panel ID readback (bypassing ESP-IDF SPI):");
    const int CS  = PK_LCD_PIN_CS;
    const int SCK = PK_LCD_PIN_SCLK;
    const int MOSI = PK_LCD_PIN_MOSI;
    const int MISO = PK_LCD_PIN_MISO;
    const int DC  = PK_LCD_PIN_DC;

    gpio_set_direction(CS,   GPIO_MODE_OUTPUT);
    gpio_set_direction(SCK,  GPIO_MODE_OUTPUT);
    gpio_set_direction(MOSI, GPIO_MODE_OUTPUT);
    gpio_set_direction(DC,   GPIO_MODE_OUTPUT);
    gpio_set_direction(MISO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(MISO, GPIO_PULLDOWN_ONLY);   /* read 0 if floating */

    gpio_set_level(CS,  1);
    gpio_set_level(SCK, 0);
    gpio_set_level(DC,  1);
    esp_rom_delay_us(100);

    /* Pull CS low, DC low (command), send 0x04 MSB-first */
    gpio_set_level(CS, 0);
    gpio_set_level(DC, 0);
    esp_rom_delay_us(10);

    uint8_t cmd = 0x04;
    for (int b = 7; b >= 0; --b) {
        gpio_set_level(MOSI, (cmd >> b) & 1);
        esp_rom_delay_us(5);
        gpio_set_level(SCK, 1);   /* rising edge — panel samples MOSI */
        esp_rom_delay_us(5);
        gpio_set_level(SCK, 0);
    }

    /* DC high (data), read 4 bytes MSB-first */
    gpio_set_level(DC, 1);
    gpio_set_level(MOSI, 0);   /* idle MOSI low during read */
    esp_rom_delay_us(10);

    uint8_t rx[4] = { 0 };
    for (int i = 0; i < 4; ++i) {
        uint8_t v = 0;
        for (int b = 7; b >= 0; --b) {
            gpio_set_level(SCK, 1);   /* rising edge — host samples MISO */
            esp_rom_delay_us(5);
            v |= (uint8_t)(gpio_get_level(MISO) << b);
            gpio_set_level(SCK, 0);
            esp_rom_delay_us(5);
        }
        rx[i] = v;
    }

    gpio_set_level(CS, 1);
    esp_rom_delay_us(10);

    ESP_LOGI(TAG, "  bit-bang RDDID: %02x %02x %02x %02x",
             rx[0], rx[1], rx[2], rx[3]);
    if (rx[0] == 0xff && rx[1] == 0xff && rx[2] == 0xff && rx[3] == 0xff) {
        ESP_LOGW(TAG, "  → all 0xFF: panel not driving MISO at all");
    } else if (rx[0] == 0 && rx[1] == 0 && rx[2] == 0 && rx[3] == 0) {
        ESP_LOGW(TAG, "  → all 0x00: panel not driving MISO at all (PD wins)");
    } else {
        ESP_LOGI(TAG, "  ✓ panel responding! Got non-trivial bytes.");
    }

    /* hand the pins back so the SPI bus init can claim them */
    gpio_reset_pin(CS);
    gpio_reset_pin(SCK);
    gpio_reset_pin(MOSI);
    gpio_reset_pin(MISO);
    gpio_reset_pin(DC);
}

#endif  /* bring-up diagnostics */

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
        .miso_io_num     = PK_LCD_PIN_MISO,  /* used by the panel ID readback */
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

    /* 4. TK024F3036 vendor driver (see file header).
     *
     * Vendor init writes MADCTL=0xA0 (MV=1, MY=1) which gives landscape
     * 320x240 with origin at top-left. We flip the panel 180° (origin
     * at bottom-right) by toggling MX on and MY off → MADCTL=0x60
     * (MV=1, MX=1). The vendor driver's mirror() handler updates only
     * the named bits, so a single call does it. invert_color(false)
     * leaves the vendor's INVON (0x21) in effect. */
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PK_LCD_PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_TK024F3036(s_io, &panel_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new_panel_TK024F3036: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, false));
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

    ESP_LOGI(TAG, "TK024F3036 %dx%d ready, framebuffer @ %p (%u KiB PSRAM)",
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
     * Cheap visual proof that orientation + MADCTL byte order +
     * backlight all work. PFD overwrites this within a second. */
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
    pk_display_set_brightness(180);
    ESP_LOGI(TAG, "test pattern flushed; backlight @ 70%%");
}
