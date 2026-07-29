/*
 * pk_sdcard.h — 板载 microSD (TF1, SDMMC 4-bit) 挂载/探测/格式化。
 *
 * 引脚来自 docs/hardware/board_pinout.md（CLK=43 CMD=44 D0-D3=39/40/41/42，
 * 全线 51kΩ 上拉到 3V3）。板上没有独立卡检测脚，插拔靠后台任务
 * mount-retry / sdmmc_get_status 轮询探测。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PK_SD_NO_CARD = 0,   /* 未插卡 / 挂载失败 */
    PK_SD_MOUNTED,       /* FAT 挂载成功，可读写 */
    PK_SD_FORMATTING,    /* 正在格式化（期间不可读写） */
} pk_sd_state_t;

/* 同步做第一次挂载尝试（无卡时约几百 ms 失败返回），然后启动后台
 * 插拔探测任务。须在 record_sinks_install_defaults() 之前调用，
 * 这样 file sink 创建时能拿到准确的挂载状态。 */
void pk_sdcard_init(void);

pk_sd_state_t pk_sdcard_state(void);
bool          pk_sdcard_is_mounted(void);

/* 挂载点（"/sdcard"），无论是否挂载都返回常量字符串。 */
const char *pk_sdcard_mount_point(void);

/* 容量信息（字节）。未挂载返回 false。 */
bool pk_sdcard_info(uint64_t *out_total, uint64_t *out_free);

/* 格式化为 FAT32（IDF FATFS 不支持 exFAT，ffconf.h 写死 FF_FS_EXFAT=0）。
 * 阻塞调用，耗时数秒；要求当前已挂载。完成后保持挂载状态。 */
esp_err_t pk_sdcard_format(void);

#ifdef __cplusplus
}
#endif

/* 累计挂载尝试次数（含失败）。诊断页据此区分"没插卡"（计数不动）与
 * "插了但挂不上"（每 3 s 涨一次）——两者的 pk_sdcard_state() 都是
 * PK_SD_NO_CARD，光看状态分不出来。 */
uint32_t pk_sdcard_mount_attempts(void);
