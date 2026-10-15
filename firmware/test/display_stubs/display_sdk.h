#pragma once

/* Hardware contracts used by the real display.c; behavior lives in the test. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef int BaseType_t;
typedef unsigned TickType_t;
typedef void *SemaphoreHandle_t;
#define pdFALSE 0
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t semaphore, BaseType_t *woken);

typedef void *esp_ldo_channel_handle_t;
typedef void *esp_lcd_dsi_bus_handle_t;
typedef void *esp_lcd_panel_io_handle_t;
typedef void *esp_lcd_panel_handle_t;
typedef void esp_lcd_panel_t;
typedef void *ppa_client_handle_t;
typedef struct { int chan_id, voltage_mv; } esp_ldo_channel_config_t;
typedef struct { int bus_id, num_data_lanes, phy_clk_src, lane_bit_rate_mbps; } esp_lcd_dsi_bus_config_t;
typedef struct { int virtual_channel, lcd_cmd_bits, lcd_param_bits; } esp_lcd_dbi_io_config_t;
typedef struct {
    int virtual_channel, dpi_clk_src, dpi_clock_freq_mhz;
    int in_color_format, out_color_format, num_fbs;
    struct { int h_size, v_size, hsync_back_porch, hsync_pulse_width, hsync_front_porch;
             int vsync_back_porch, vsync_pulse_width, vsync_front_porch; } video_timing;
} esp_lcd_dpi_panel_config_t;
typedef struct { int cmd; const void *data; size_t data_bytes; unsigned delay_ms; } st7701_lcd_init_cmd_t;
typedef struct {
    const st7701_lcd_init_cmd_t *init_cmds;
    size_t init_cmds_size;
    struct { esp_lcd_dsi_bus_handle_t dsi_bus; const esp_lcd_dpi_panel_config_t *dpi_config; } mipi_config;
    struct { unsigned use_mipi_interface; } flags;
} st7701_vendor_config_t;
typedef struct { int reset_gpio_num, rgb_ele_order, bits_per_pixel; const void *vendor_config; } esp_lcd_panel_dev_config_t;
typedef struct { int unused; } esp_lcd_dpi_panel_event_data_t;
typedef bool (*display_event_cb_t)(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *, void *);
typedef struct { display_event_cb_t on_color_trans_done, on_refresh_done; } esp_lcd_dpi_panel_event_callbacks_t;

typedef struct { int speed_mode, duty_resolution, timer_num, freq_hz, clk_cfg; } ledc_timer_config_t;
typedef struct {
    int gpio_num, speed_mode, channel, intr_type, timer_sel, duty, hpoint;
    struct { unsigned output_invert; } flags;
} ledc_channel_config_t;
enum { LEDC_TIMER_0, LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, LEDC_TIMER_10_BIT,
       LEDC_AUTO_CLK, LEDC_INTR_DISABLE, MIPI_DSI_DPI_CLK_SRC_DEFAULT,
       LCD_COLOR_FMT_RGB565, LCD_RGB_ELEMENT_ORDER_RGB, PPA_OPERATION_SRM,
       PPA_DATA_BURST_LENGTH_128, PPA_SRM_COLOR_MODE_RGB565,
       PPA_SRM_ROTATION_ANGLE_270, PPA_TRANS_MODE_BLOCKING };
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
typedef struct { int oper_type, max_pending_trans_num, data_burst_length; } ppa_client_config_t;
typedef struct {
    struct { const void *buffer; unsigned pic_w, pic_h, block_w, block_h, block_offset_x, block_offset_y; int srm_cm; } in;
    struct { void *buffer; size_t buffer_size; unsigned pic_w, pic_h, block_offset_x, block_offset_y; int srm_cm; } out;
    int rotation_angle;
    float scale_x, scale_y;
    bool mirror_x, mirror_y, rgb_swap, byte_swap;
    int mode;
} ppa_srm_oper_config_t;

esp_err_t ppa_do_scale_rotate_mirror(ppa_client_handle_t, const ppa_srm_oper_config_t *);
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t, int, int, int, int, const void *);
esp_err_t ledc_timer_config(const ledc_timer_config_t *);
esp_err_t ledc_channel_config(const ledc_channel_config_t *);
esp_err_t ledc_set_duty(int, int, uint32_t);
esp_err_t ledc_update_duty(int, int);
esp_err_t esp_ldo_acquire_channel(const esp_ldo_channel_config_t *, esp_ldo_channel_handle_t *);
esp_err_t esp_lcd_new_dsi_bus(const esp_lcd_dsi_bus_config_t *, esp_lcd_dsi_bus_handle_t *);
esp_err_t esp_lcd_new_panel_io_dbi(esp_lcd_dsi_bus_handle_t, const esp_lcd_dbi_io_config_t *, esp_lcd_panel_io_handle_t *);
esp_err_t esp_lcd_new_panel_st7701(esp_lcd_panel_io_handle_t, const esp_lcd_panel_dev_config_t *, esp_lcd_panel_handle_t *);
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t);
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t);
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t, bool);
esp_err_t esp_lcd_dpi_panel_get_frame_buffer(esp_lcd_panel_handle_t, unsigned, void **, void **);
esp_err_t esp_lcd_dpi_panel_register_event_callbacks(esp_lcd_panel_handle_t, const esp_lcd_dpi_panel_event_callbacks_t *, void *);
esp_err_t ppa_register_client(const ppa_client_config_t *, ppa_client_handle_t *);
void *heap_caps_aligned_alloc(size_t, size_t, unsigned);
