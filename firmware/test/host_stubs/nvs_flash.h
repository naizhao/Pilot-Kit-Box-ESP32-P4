#pragma once
/*
 * host_stubs/nvs_flash.h — NVS 分区初始化的空壳。
 *
 * 返回 ESP_OK（= 分区已就绪）。"需要擦除后重试"那条分支不在本轮被测范围，
 * 伪造它只会让测试看起来覆盖得更多而实际什么也没证明。
 */
#include "nvs.h"

static inline esp_err_t nvs_flash_init(void)  { return ESP_OK; }
static inline esp_err_t nvs_flash_erase(void) { return ESP_OK; }
