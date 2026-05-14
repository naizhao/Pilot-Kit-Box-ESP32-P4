/*
 * SPDX-FileCopyrightText: 2021-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <sys/cdefs.h>
#include "sdkconfig.h"

#if CONFIG_LCD_ENABLE_DEBUG_LOG
// The local log level must be defined before including esp_log.h
// Set the maximum log level for this source file
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_compiler.h"

#define TK024F3036_CMD_RAMCTRL               0xb0
#define TK024F3036_DATA_LITTLE_ENDIAN_BIT    (1 << 3)

static const char *TAG = "lcd_panel.TK024F3036";

static esp_err_t panel_TK024F3036_del(esp_lcd_panel_t *panel);
static esp_err_t panel_TK024F3036_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_TK024F3036_init(esp_lcd_panel_t *panel);
static esp_err_t panel_TK024F3036_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                          const void *color_data);
static esp_err_t panel_TK024F3036_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_TK024F3036_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_TK024F3036_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_TK024F3036_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_TK024F3036_disp_on_off(esp_lcd_panel_t *panel, bool off);
static esp_err_t panel_TK024F3036_sleep(esp_lcd_panel_t *panel, bool sleep);

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val;    // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val;    // save current value of LCD_CMD_COLMOD register
    uint8_t ramctl_val_1;
    uint8_t ramctl_val_2;
} TK024F3036_panel_t;

esp_err_t
esp_lcd_new_panel_TK024F3036(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                         esp_lcd_panel_handle_t *ret_panel)
{
#if CONFIG_LCD_ENABLE_DEBUG_LOG
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif
    esp_err_t ret = ESP_OK;
    TK024F3036_panel_t *TK024F3036 = NULL;
    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    // leak detection of TK024F3036 because saving TK024F3036->base address
    ESP_COMPILER_DIAGNOSTIC_PUSH_IGNORE("-Wanalyzer-malloc-leak")
    TK024F3036 = calloc(1, sizeof(TK024F3036_panel_t));
    ESP_GOTO_ON_FALSE(TK024F3036, ESP_ERR_NO_MEM, err, TAG, "no mem for TK024F3036 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        TK024F3036->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        TK024F3036->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported RGB element order");
        break;
    }

    uint8_t fb_bits_per_pixel = 0;
    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        TK024F3036->colmod_val = 0x55;
        fb_bits_per_pixel = 16;
        break;
    case 18: // RGB666
        TK024F3036->colmod_val = 0x66;
        // each color component (R/G/B) should occupy the 6 high bits of a byte, which means 3 full bytes are required for a pixel
        fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    TK024F3036->ramctl_val_1 = 0x00;
    TK024F3036->ramctl_val_2 = 0xf0;    // Use big endian by default
    if ((panel_dev_config->data_endian) == LCD_RGB_DATA_ENDIAN_LITTLE) {
        // Use little endian
        TK024F3036->ramctl_val_2 |= TK024F3036_DATA_LITTLE_ENDIAN_BIT;
    }

    TK024F3036->io = io;
    TK024F3036->fb_bits_per_pixel = fb_bits_per_pixel;
    TK024F3036->reset_gpio_num = panel_dev_config->reset_gpio_num;
    TK024F3036->reset_level = panel_dev_config->flags.reset_active_high;
    TK024F3036->base.del = panel_TK024F3036_del;
    TK024F3036->base.reset = panel_TK024F3036_reset;
    TK024F3036->base.init = panel_TK024F3036_init;
    TK024F3036->base.draw_bitmap = panel_TK024F3036_draw_bitmap;
    TK024F3036->base.invert_color = panel_TK024F3036_invert_color;
    TK024F3036->base.set_gap = panel_TK024F3036_set_gap;
    TK024F3036->base.mirror = panel_TK024F3036_mirror;
    TK024F3036->base.swap_xy = panel_TK024F3036_swap_xy;
    TK024F3036->base.disp_on_off = panel_TK024F3036_disp_on_off;
    TK024F3036->base.disp_sleep = panel_TK024F3036_sleep;
    *ret_panel = &(TK024F3036->base);
    ESP_LOGD(TAG, "new TK024F3036 panel @%p", TK024F3036);

    return ESP_OK;

err:
    if (TK024F3036) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(TK024F3036);
    }
    return ret;
    ESP_COMPILER_DIAGNOSTIC_POP("-Wanalyzer-malloc-leak")
}

static esp_err_t panel_TK024F3036_del(esp_lcd_panel_t *panel)
{
    TK024F3036_panel_t *TK024F3036 = __containerof(panel, TK024F3036_panel_t, base);

    if (TK024F3036->reset_gpio_num >= 0) {
        gpio_reset_pin(TK024F3036->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del TK024F3036 panel @%p", TK024F3036);
    free(TK024F3036);
    return ESP_OK;
}

static esp_err_t panel_TK024F3036_reset(esp_lcd_panel_t *panel)
{
    TK024F3036_panel_t *TK024F3036 = __containerof(panel, TK024F3036_panel_t, base);
    esp_lcd_panel_io_handle_t io = TK024F3036->io;

    // perform hardware reset
    if (TK024F3036->reset_gpio_num >= 0) {
        gpio_set_level(TK024F3036->reset_gpio_num, TK024F3036->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(TK024F3036->reset_gpio_num, !TK024F3036->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
    } else { // perform software reset
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG,
                            "io tx param failed");
        vTaskDelay(pdMS_TO_TICKS(20)); // spec, wait at least 5m before sending new command
    }

    return ESP_OK;
}

static esp_err_t panel_TK024F3036_init(esp_lcd_panel_t *panel)
{
    TK024F3036_panel_t *TK024F3036 = __containerof(panel, TK024F3036_panel_t, base);
    esp_lcd_panel_io_handle_t io = TK024F3036->io;
    // LCD goes into sleep mode and display will be turned off after power on reset, exit sleep mode first

    //************* Start Initial Sequence **********//
	ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x11, NULL, 0), TAG, "io tx param failed");
    vTaskDelay(pdMS_TO_TICKS(120)); // Typically need 120ms delay after sleep out command

	ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x36, (uint8_t[]) { 0xA0 }, 1), TAG, "io tx param failed");   //Set_address_mode 这个寄存器用于旋转屏幕

	ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x3A, (uint8_t[]) { 0x05 }, 1), TAG, "io tx param failed");  //65k mode 

	ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xB2, (uint8_t[]) { 0x1F,0x1F , 0x00 , 0x33 , 0x33 }, 5), TAG, "io tx param failed");  

	ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xB7, (uint8_t[]) { 0x12 }, 1), TAG, "io tx param failed");  //VGH=12.54V,VGL=-8.23V 

	ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xBB, (uint8_t[]) { 0x66 }, 1), TAG, "io tx param failed");  

	ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xC0, (uint8_t[]) { 0x2C }, 1), TAG, "io tx param failed"); 

	ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xC2, (uint8_t[]) { 0x01 }, 1), TAG, "io tx param failed");   

	ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xC3, (uint8_t[]) { 0x15 }, 1), TAG, "io tx param failed"); //4.6V 

	ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xC4, (uint8_t[]) { 0x20 }, 1), TAG, "io tx param failed");   //VDV, 0x20:0v

	ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xC6, (uint8_t[]) { 0x13 }, 1), TAG, "io tx param failed");     

	ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xD0, (uint8_t[]) { 0xA4 , 0xA1 }, 2), TAG, "io tx param failed");   

	ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xD6, (uint8_t[]) { 0xA1 }, 1), TAG, "io tx param failed");   //sleep in后，gate输出为GND

	ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xE0, (uint8_t[]) { 0xF0,  0x06,  0x0D,  0x0B,  0x0A,  0x07,  0x2E,  0x43,  0x45,  0x38,  0x14,  0x13,  0x25,  0x29 }, 14), TAG, "io tx param failed");

	ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0xE1, (uint8_t[]) { 0xF0,  0x07,  0x0A,  0x08,  0x07,  0x23,  0x2E,  0x33,  0x44,  0x3A,  0x16,  0x17,  0x26,  0x2C }, 14), TAG, "io tx param failed");  


	ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x21, NULL, 0), TAG, "io tx param failed");
    // Note: The following commands don't have parameters but are commands
    
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x29, NULL, 0), TAG, "io tx param failed");
    vTaskDelay(pdMS_TO_TICKS(50));

    return ESP_OK;
}

static esp_err_t panel_TK024F3036_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                          const void *color_data)
{
    TK024F3036_panel_t *TK024F3036 = __containerof(panel, TK024F3036_panel_t, base);
    esp_lcd_panel_io_handle_t io = TK024F3036->io;

    x_start += TK024F3036->x_gap;
    x_end += TK024F3036->x_gap;
    y_start += TK024F3036->y_gap;
    y_end += TK024F3036->y_gap;

    // define an area of frame memory where MCU can access
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF,
        x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF,
        (x_end - 1) & 0xFF,
    }, 4), TAG, "io tx param failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF,
        y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF,
        (y_end - 1) & 0xFF,
    }, 4), TAG, "io tx param failed");
    // transfer frame buffer
    size_t len = (x_end - x_start) * (y_end - y_start) * TK024F3036->fb_bits_per_pixel / 8;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len), TAG, "io tx color failed");

    return ESP_OK;
}

static esp_err_t panel_TK024F3036_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    TK024F3036_panel_t *TK024F3036 = __containerof(panel, TK024F3036_panel_t, base);
    esp_lcd_panel_io_handle_t io = TK024F3036->io;
    int command = 0;
    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG,
                        "io tx param failed");
    return ESP_OK;
}

static esp_err_t panel_TK024F3036_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    TK024F3036_panel_t *TK024F3036 = __containerof(panel, TK024F3036_panel_t, base);
    esp_lcd_panel_io_handle_t io = TK024F3036->io;
    if (mirror_x) {
        TK024F3036->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        TK024F3036->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        TK024F3036->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        TK024F3036->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        TK024F3036->madctl_val
    }, 1), TAG, "io tx param failed");
    return ESP_OK;
}

static esp_err_t panel_TK024F3036_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    TK024F3036_panel_t *TK024F3036 = __containerof(panel, TK024F3036_panel_t, base);
    esp_lcd_panel_io_handle_t io = TK024F3036->io;
    if (swap_axes) {
        TK024F3036->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        TK024F3036->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        TK024F3036->madctl_val
    }, 1), TAG, "io tx param failed");
    return ESP_OK;
}

static esp_err_t panel_TK024F3036_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    TK024F3036_panel_t *TK024F3036 = __containerof(panel, TK024F3036_panel_t, base);
    TK024F3036->x_gap = x_gap;
    TK024F3036->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_TK024F3036_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    TK024F3036_panel_t *TK024F3036 = __containerof(panel, TK024F3036_panel_t, base);
    esp_lcd_panel_io_handle_t io = TK024F3036->io;
    int command = 0;
    if (on_off) {
        command = LCD_CMD_DISPON;
    } else {
        command = LCD_CMD_DISPOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG,
                        "io tx param failed");
    return ESP_OK;
}

static esp_err_t panel_TK024F3036_sleep(esp_lcd_panel_t *panel, bool sleep)
{
    TK024F3036_panel_t *TK024F3036 = __containerof(panel, TK024F3036_panel_t, base);
    esp_lcd_panel_io_handle_t io = TK024F3036->io;
    int command = 0;
    if (sleep) {
        command = LCD_CMD_SLPIN;
    } else {
        command = LCD_CMD_SLPOUT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG,
                        "io tx param failed");
    vTaskDelay(pdMS_TO_TICKS(100));

    return ESP_OK;
}
