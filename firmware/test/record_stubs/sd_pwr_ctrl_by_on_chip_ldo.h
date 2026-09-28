#pragma once
#include "esp_err.h"
typedef void *sd_pwr_ctrl_handle_t;
typedef struct { int ldo_chan_id; } sd_pwr_ctrl_ldo_config_t;
static inline esp_err_t sd_pwr_ctrl_new_on_chip_ldo(const sd_pwr_ctrl_ldo_config_t *c,sd_pwr_ctrl_handle_t *out)
{ (void)c;*out=(void *)1;return ESP_OK; }
