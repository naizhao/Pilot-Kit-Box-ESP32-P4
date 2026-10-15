#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SDMMC_HOST_SLOT_0 0
#define SDMMC_SLOT_FLAG_INTERNAL_PULLUP 1
typedef struct { int slot;void *pwr_ctrl_handle;esp_err_t (*init)(void);esp_err_t (*deinit)(void);
    bool (*check_buffer_alignment)(int,const void *,size_t); } sdmmc_host_t;
typedef struct { int flags,width,clk,cmd,d0,d1,d2,d3; } sdmmc_slot_config_t;
#define SDMMC_HOST_DEFAULT() ((sdmmc_host_t){0})
#define SDMMC_SLOT_CONFIG_DEFAULT() ((sdmmc_slot_config_t){0})
static inline esp_err_t sdmmc_host_deinit_slot(int slot) { (void)slot;return ESP_OK; }
static inline bool sdmmc_host_check_buffer_alignment(int slot,const void *buf,size_t size)
{ (void)slot;return buf&&size&&((uintptr_t)buf%64==0)&&(size%64==0); }
