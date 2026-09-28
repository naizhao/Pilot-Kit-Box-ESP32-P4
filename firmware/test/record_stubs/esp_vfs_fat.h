#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
typedef struct { bool format_if_mount_failed;int max_files,allocation_unit_size; } esp_vfs_fat_sdmmc_mount_config_t;
static inline esp_err_t esp_vfs_fat_info(const char *p,uint64_t *t,uint64_t *f)
{ (void)p;*t=1024;*f=512;return ESP_OK; }
static inline esp_err_t esp_vfs_fat_sdmmc_mount(const char *p,const sdmmc_host_t *h,const sdmmc_slot_config_t *s,
 const esp_vfs_fat_sdmmc_mount_config_t *c,sdmmc_card_t **out)
{ (void)p;(void)h;(void)s;(void)c;static sdmmc_card_t card;*out=&card;return ESP_OK; }
static inline esp_err_t esp_vfs_fat_sdcard_unmount(const char *p,sdmmc_card_t *c)
{ (void)p;(void)c;return ESP_OK; }
static inline void esp_vfs_fat_unregister_path(const char *p) { (void)p; }
esp_err_t esp_vfs_fat_sdcard_format(const char *p,sdmmc_card_t *c);
