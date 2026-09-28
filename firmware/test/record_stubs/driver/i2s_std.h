#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
typedef void *i2s_chan_handle_t;
typedef struct { unsigned dma_desc_num,dma_frame_num; } i2s_chan_config_t;
typedef struct { int mclk_multiple; } i2s_clock_t;
typedef struct { int slot_bit_width; } i2s_slot_t;
typedef struct {
    int mclk,bclk,ws,dout,din;
    struct { bool mclk_inv,bclk_inv,ws_inv; } invert_flags;
} i2s_gpio_t;
typedef struct { i2s_clock_t clk_cfg;i2s_slot_t slot_cfg;i2s_gpio_t gpio_cfg; } i2s_std_config_t;
#define I2S_CHANNEL_DEFAULT_CONFIG(port,role) ((i2s_chan_config_t){0})
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) ((i2s_clock_t){0})
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits,mode) ((i2s_slot_t){0})
#define I2S_GPIO_UNUSED (-1)
#define I2S_MCLK_MULTIPLE_384 384
#define I2S_SLOT_BIT_WIDTH_24BIT 24
static inline esp_err_t i2s_new_channel(const i2s_chan_config_t *cfg,void *tx,i2s_chan_handle_t *rx)
{ (void)cfg;(void)tx;*rx=(void *)1;return ESP_OK; }
static inline esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t rx,const i2s_std_config_t *cfg)
{ (void)rx;(void)cfg;return ESP_OK; }
static inline esp_err_t i2s_channel_enable(i2s_chan_handle_t rx) { (void)rx;return ESP_OK; }
esp_err_t i2s_channel_disable(i2s_chan_handle_t rx);
esp_err_t i2s_channel_read(i2s_chan_handle_t rx,void *dst,size_t capacity,size_t *got,unsigned wait_ms);
