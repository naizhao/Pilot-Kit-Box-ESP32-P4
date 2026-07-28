/*
 * display.c — ESP32-P4-WIFI6-Touch-LCD-4.3 显示 bring-up。
 *
 * ST7701 以 2-lane MIPI-DSI 接入，DPI 按原生 480×800 连续扫描。上层仍
 * 在 800×480 RGB565-swapped framebuffer 中绘制；每帧由 PPA 顺时针旋转
 * 90°并同时 byte-swap 到非活动 DPI framebuffer，再于 VSYNC 切换。
 */

#include "display.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "driver/ledc.h"
#include "driver/ppa.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st7701.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "display";

#define BL_LEDC_TIMER          LEDC_TIMER_0
#define BL_LEDC_MODE           LEDC_LOW_SPEED_MODE
#define BL_LEDC_CHANNEL        LEDC_CHANNEL_0
#define BL_LEDC_DUTY_BITS      LEDC_TIMER_10_BIT
#define BL_LEDC_MAX_DUTY       ((1U << 10) - 1U)
#define LCD_REFRESH_TIMEOUT_MS 100
#define LCD_FB_ALIGN           128

static esp_ldo_channel_handle_t s_dsi_phy_ldo;
static esp_lcd_dsi_bus_handle_t s_dsi_bus;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static ppa_client_handle_t s_ppa;
static SemaphoreHandle_t s_refresh_done;
static uint16_t *s_fb;
static uint16_t *s_dpi_fb[2];
static unsigned s_front_fb;
static volatile uint32_t s_refresh_count;

/* 微雪 4.3 寸板官方 BSP 的 ST7701 模组专用初始化序列。 */
static const st7701_lcd_init_cmd_t s_st7701_init[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t[]){0x08}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0x63, 0x00}, 2, 0},
    {0xC1, (uint8_t[]){0x0D, 0x02}, 2, 0},
    {0xC2, (uint8_t[]){0x17, 0x08}, 2, 0},
    {0xCC, (uint8_t[]){0x10}, 1, 0},
    {0xB0, (uint8_t[]){0x40, 0xC9, 0x94, 0x0E, 0x10, 0x05, 0x0B, 0x09,
                       0x08, 0x26, 0x04, 0x52, 0x10, 0x69, 0x6B, 0x69}, 16, 0},
    {0xB1, (uint8_t[]){0x40, 0xD2, 0x98, 0x0C, 0x92, 0x07, 0x09, 0x08,
                       0x07, 0x25, 0x02, 0x0E, 0x0C, 0x6E, 0x78, 0x55}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x5D}, 1, 0},
    {0xB1, (uint8_t[]){0x4E}, 1, 0},
    {0xB2, (uint8_t[]){0x87}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x4E}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xB9, (uint8_t[]){0x10, 0x1F}, 2, 0},
    {0xBB, (uint8_t[]){0x03}, 1, 0},
    {0xBC, (uint8_t[]){0x00}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xD0, (uint8_t[]){0x88}, 1, 0},
    {0xE0, (uint8_t[]){0x00, 0x3A, 0x02}, 3, 0},
    {0xE1, (uint8_t[]){0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0,
                       0x00, 0x40, 0x40}, 11, 0},
    {0xE2, (uint8_t[]){0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0,
                       0x00, 0xA0, 0x00, 0xA0, 0x00}, 13, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0,
                       0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0,
                       0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}, 16, 0},
    {0xEB, (uint8_t[]){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7, 0},
    {0xEC, (uint8_t[]){0x08, 0x01}, 2, 0},
    {0xED, (uint8_t[]){0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF,
                       0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}, 16, 0},
    {0xEF, (uint8_t[]){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x11, NULL, 0, 120},
    {0x29, NULL, 0, 0},
};

static bool IRAM_ATTR lcd_refresh_done_cb(
    esp_lcd_panel_handle_t panel,
    esp_lcd_dpi_panel_event_data_t *event_data,
    void *user_ctx)
{
    (void)panel;
    (void)event_data;

    BaseType_t high_task_woken = pdFALSE;
    ++s_refresh_count;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &high_task_woken);
    return high_task_woken == pdTRUE;
}

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer_config = {
        .speed_mode = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_DUTY_BITS,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = PK_LCD_BL_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        return err;
    }

    const ledc_channel_config_t channel_config = {
        .gpio_num = PK_LCD_PIN_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = 1,
    };
    return ledc_channel_config(&channel_config);
}

void pk_display_set_brightness(uint8_t level)
{
    const uint32_t duty = ((uint32_t)level * BL_LEDC_MAX_DUTY) / UINT8_MAX;
    if (ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty) == ESP_OK) {
        (void)ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
    }
}

void pk_display_panel_off(void)
{
    if (s_panel != NULL) {
        (void)esp_lcd_panel_disp_on_off(s_panel, false);
    }
}

static esp_err_t rotate_and_present(void)
{
    const unsigned back_fb = s_front_fb ^ 1U;
    const ppa_srm_oper_config_t operation = {
        .in = {
            .buffer = s_fb,
            .pic_w = PK_DISPLAY_W,
            .pic_h = PK_DISPLAY_H,
            .block_w = PK_DISPLAY_W,
            .block_h = PK_DISPLAY_H,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = s_dpi_fb[back_fb],
            .buffer_size = PK_LCD_NATIVE_FB_BYTES,
            .pic_w = PK_LCD_NATIVE_W,
            .pic_h = PK_LCD_NATIVE_H,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        /* PPA 角度按逆时针定义；270° CCW 等价于实物所需的 90° CW。 */
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mirror_x = false,
        .mirror_y = false,
        .rgb_swap = false,
        .byte_swap = true,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &operation);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PPA rotate failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0,
                                    PK_LCD_NATIVE_W, PK_LCD_NATIVE_H,
                                    s_dpi_fb[back_fb]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DPI framebuffer switch failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * draw_bitmap() 只更新下一帧要扫描的 framebuffer 指针。单纯等待 binary
     * semaphore 有一个竞态：提交前留下的 VSYNC token 会让函数过早返回，
     * 下一次 PPA 就可能覆盖仍在扫描的 buffer。提交后采样计数，再等它变化；
     * 若采样前已发生 VSYNC，最多多等一帧，但绝不会少等一帧。
     */
    const uint32_t refresh_after_submit = s_refresh_count;
    while (xSemaphoreTake(s_refresh_done, 0) == pdTRUE) {
        /* 清掉与当前计数对应的旧 token。 */
    }
    while (s_refresh_count == refresh_after_submit) {
        if (xSemaphoreTake(s_refresh_done,
                           pdMS_TO_TICKS(LCD_REFRESH_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "waiting for LCD VSYNC timed out after %u ms",
                     LCD_REFRESH_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }
    }
    s_front_fb = back_fb;
    return ESP_OK;
}

esp_err_t pk_display_init(void)
{
    esp_err_t err = backlight_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight init failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_ldo_channel_config_t ldo_config = {
        .chan_id = PK_LCD_DSI_PHY_LDO_CHANNEL,
        .voltage_mv = PK_LCD_DSI_PHY_LDO_MV,
    };
    err = esp_ldo_acquire_channel(&ldo_config, &s_dsi_phy_ldo);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DSI PHY LDO acquire failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = PK_LCD_DSI_LANE_COUNT,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = PK_LCD_DSI_LANE_BIT_RATE_MBPS,
    };
    err = esp_lcd_new_dsi_bus(&bus_config, &s_dsi_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DSI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_config, &s_panel_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DSI DBI control IO init failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = PK_LCD_DPI_CLOCK_MHZ,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 2,
        .video_timing = {
            .h_size = PK_LCD_NATIVE_W,
            .v_size = PK_LCD_NATIVE_H,
            .hsync_back_porch = 42,
            .hsync_pulse_width = 12,
            .hsync_front_porch = 42,
            .vsync_back_porch = 2,
            .vsync_pulse_width = 8,
            .vsync_front_porch = 60,
        },
    };
    st7701_vendor_config_t vendor_config = {
        .init_cmds = s_st7701_init,
        .init_cmds_size = sizeof(s_st7701_init) / sizeof(s_st7701_init[0]),
        .mipi_config = {
            .dsi_bus = s_dsi_bus,
            .dpi_config = &dpi_config,
        },
        .flags.use_mipi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PK_LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    err = esp_lcd_new_panel_st7701(s_panel_io, &panel_config, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ST7701 panel create failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_panel_reset(s_panel);
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(s_panel);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(s_panel, true);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ST7701 panel init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_dpi_panel_get_frame_buffer(
        s_panel, 2, (void **)&s_dpi_fb[0], (void **)&s_dpi_fb[1]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DPI framebuffer lookup failed: %s", esp_err_to_name(err));
        return err;
    }
    memset(s_dpi_fb[0], 0, PK_LCD_NATIVE_FB_BYTES);
    memset(s_dpi_fb[1], 0, PK_LCD_NATIVE_FB_BYTES);

    s_refresh_done = xSemaphoreCreateBinary();
    if (s_refresh_done == NULL) {
        ESP_LOGE(TAG, "LCD VSYNC semaphore allocation failed");
        return ESP_ERR_NO_MEM;
    }
    const esp_lcd_dpi_panel_event_callbacks_t callbacks = {
        .on_refresh_done = lcd_refresh_done_cb,
    };
    err = esp_lcd_dpi_panel_register_event_callbacks(
        s_panel, &callbacks, s_refresh_done);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LCD VSYNC callback registration failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    const ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    err = ppa_register_client(&ppa_config, &s_ppa);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PPA client registration failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    s_fb = heap_caps_aligned_alloc(
        LCD_FB_ALIGN, PK_DISPLAY_FB_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "logical framebuffer allocation failed (%u bytes)",
                 (unsigned)PK_DISPLAY_FB_BYTES);
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb, 0, PK_DISPLAY_FB_BYTES);
    s_front_fb = 0;

    ESP_LOGI(TAG,
             "ST7701 DSI ready: logical %dx%d -> PPA 90 CW -> native %dx%d, "
             "2 DPI buffers, app framebuffer %u KiB PSRAM",
             PK_DISPLAY_W, PK_DISPLAY_H, PK_LCD_NATIVE_W, PK_LCD_NATIVE_H,
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
    if (s_panel == NULL || s_ppa == NULL || s_refresh_done == NULL ||
        s_fb == NULL || pixels == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (x0 >= PK_DISPLAY_W || y0 >= PK_DISPLAY_H) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x1 > PK_DISPLAY_W) {
        x1 = PK_DISPLAY_W;
    }
    if (y1 > PK_DISPLAY_H) {
        y1 = PK_DISPLAY_H;
    }
    if (x1 <= x0 || y1 <= y0) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t width = (size_t)(x1 - x0);
    const size_t height = (size_t)(y1 - y0);
    if (!(pixels == s_fb && x0 == 0 && y0 == 0 &&
          x1 == PK_DISPLAY_W && y1 == PK_DISPLAY_H)) {
        for (size_t row = 0; row < height; ++row) {
            memcpy(s_fb + ((size_t)y0 + row) * PK_DISPLAY_W + x0,
                   pixels + row * width,
                   width * sizeof(uint16_t));
        }
    }
    return rotate_and_present();
}

esp_err_t pk_display_flush_full(void)
{
    return pk_display_draw(0, 0, PK_DISPLAY_W, PK_DISPLAY_H, s_fb);
}

void pk_display_test_pattern(void)
{
    if (s_fb == NULL) {
        return;
    }
    for (int y = 0; y < PK_DISPLAY_H; ++y) {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        if (y < PK_DISPLAY_H / 3) {
            r = (uint8_t)(255 * y / (PK_DISPLAY_H / 3));
        } else if (y < 2 * PK_DISPLAY_H / 3) {
            g = (uint8_t)(255 * (y - PK_DISPLAY_H / 3) /
                          (PK_DISPLAY_H / 3));
        } else {
            b = (uint8_t)(255 * (y - 2 * PK_DISPLAY_H / 3) /
                          (PK_DISPLAY_H / 3));
        }
        const uint16_t color = pk_rgb565(r, g, b);
        for (int x = 0; x < PK_DISPLAY_W; ++x) {
            s_fb[(size_t)y * PK_DISPLAY_W + x] = color;
        }
    }
    if (pk_display_flush_full() == ESP_OK) {
        pk_display_set_brightness(180);
        ESP_LOGI(TAG, "test pattern presented; backlight at 70%%");
    }
}
