#pragma once
#include "../host_stubs/nvs.h"
#include <stdint.h>
static inline esp_err_t nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *out)
{ (void)h;(void)key;(void)out;return ESP_ERR_NVS_NOT_FOUND; }
static inline esp_err_t nvs_set_u8(nvs_handle_t h,const char *key,uint8_t value)
{ (void)h;(void)key;(void)value;return ESP_OK; }
