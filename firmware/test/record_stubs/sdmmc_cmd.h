#pragma once
#include "esp_err.h"
typedef struct { struct { char name[8]; } cid;struct { unsigned capacity,sector_size; } csd; } sdmmc_card_t;
static inline esp_err_t sdmmc_get_status(sdmmc_card_t *c) { (void)c;return ESP_OK; }
