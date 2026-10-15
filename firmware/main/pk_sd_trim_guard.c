/*
 * pk_sd_trim_guard.c — 见 pk_sd_trim_guard.h 的范围与契约。
 *
 * 保留 no-op 作为防御性边界，不据此归因 microSD 故障。FatFS 的 TRIM
 * 只是可忽略提示；返回 ESP_OK 不表示介质已经执行物理擦除。签名与
 * IDF 6.0.1 sdmmc_cmd.h 的声明一致（含 sdmmc_erase_arg_t 参数）。
 */
#include "pk_sd_trim_guard.h"
#include "sdmmc_cmd.h"

/* 链接期 --wrap 的转发符号。本守卫故意从不调用：保留声明是为了让
 * 「改回转发」的意图必须动这行代码。 */
extern esp_err_t __real_sdmmc_erase_sectors(sdmmc_card_t *card,
                                            size_t start_sector,
                                            size_t sector_count,
                                            sdmmc_erase_arg_t arg);

/* 探测任务/日志读取，宽松计数即可。 */
static uint32_t s_trim_blocks;

esp_err_t __wrap_sdmmc_erase_sectors(sdmmc_card_t *card, size_t start_sector,
                                     size_t sector_count, sdmmc_erase_arg_t arg)
{
    (void)card; (void)start_sector; (void)sector_count; (void)arg;
    s_trim_blocks++;
    return ESP_OK;   /* FatFS 视为成功；TRIM 只是 hint，no-op 语义安全 */
}

uint32_t pk_sd_trim_guard_count(void) { return s_trim_blocks; }
