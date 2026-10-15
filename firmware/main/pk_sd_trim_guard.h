#pragma once
#include <stddef.h>
#include <stdint.h>

/*
 * pk_sd_trim_guard.h — SD TRIM/CMD38 链接期 no-op 守卫。
 *
 * 防御性边界：IDF 6.0.1 的 FatFS CTRL_TRIM 先检查 sdmmc_can_trim()；
 * 该函数只接受支持 TRIM 的 MMC，普通 microSD 不会进入擦除调用。因此
 * 不能把这条 FatFS 路径认定为本次 microSD 故障来源。
 * 本项目仍保留擦除 no-op。FatFS 释放簇时忽略 CTRL_TRIM 返回值，提示
 * 不下发不影响 FAT 簇释放；ESP_OK 表示守卫处理了提示，不表示物理擦除。
 * 本契约针对当前 FatFS 配置；未启用的 LittleFS SDMMC 后端不在验证范围。
 *
 * 接线在 firmware/main/CMakeLists.txt：-Wl,--wrap=sdmmc_erase_sectors 把
 * 所有链接期 sdmmc_erase_sectors 引用重定向到本文件的 __wrap 实现；
 * -u,__wrap_sdmmc_erase_sectors 确保提前扫描 libmain.a 时提取守卫。
 * 接线由 test_sd_trim_link.py 的真实 RISC-V 归档链接测试验证。
 */

/* 被拦截的擦除（TRIM/DISCARD）次数。诊断/日志读取用，宽松计数即可。 */
uint32_t pk_sd_trim_guard_count(void);
