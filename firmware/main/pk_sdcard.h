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

/* 挂载代数：每成功挂载一次 +1，只增不减。
 *
 * 给「卡是不是被换过/重插过」这个问题一个不会丢失的答案。轮询
 * pk_sdcard_is_mounted() 的电平做边沿检测是不可靠的——卡不在的窗口
 * 可能比调用方的轮询周期还短，整段就被跳过。2026-08-01 实测：pk_aero
 * 读到一半被拔卡进了 ERROR 态，而卡只离开 3.5 s，它两个检查点一个落在
 * 延时里、一个落在卡已回来之后，于是一直卡在 ERROR，直到下一次拔卡才
 * 恢复。比较代数就没有这个问题：只要值变了，中间一定发生过重新挂载。 */
uint32_t pk_sdcard_mount_generation(void);

/* 「卡在位，但文件系统挂不上」——即上一次挂载尝试是**卡层协商成功、FAT
 * 挂载失败**（分区不是 FAT / 已损坏）。
 *
 * 板上没有卡检测脚，"没插卡"和"插了张读不出的卡"在 pk_sdcard_state() 上都是
 * PK_SD_NO_CARD。这个 flag 是唯一能区分两者的信号，取自挂载错误码的来源层，
 * 判据见 pk_sdcard.c 里 sd_mount_locked() 失败分支的注释。
 *
 * 语义是「当前结论」而非事件：无卡时恒为 false，挂上后清零。调用方要提示
 * 用户的话自己做边沿检测（探测任务每 3 s 会重算一次，值不会抖）。 */
bool pk_sdcard_media_error(void);

/* 注册「卸载前静默」回调（固定 4 槽，满了打 ERROR 丢弃）。
 *
 * 为什么需要它：IDF 的 esp_vfs_fat_sdcard_unmount() 会无条件 free 掉含所有
 * 打开文件 FIL 数组的 fat_ctx（vfs_fat_sdmmc.c unmount_card_core，不检查
 * 是否还有文件开着），卸载时系统里只要还有打开的 SD fd 或在途 SD I/O，
 * 就是 use-after-free + 在途事务撞上 slot 注销/LDO 断电，SDMMC 驱动状态
 * 被打坏后重插永远挂不上（2026-08-01 地图/航空模块引入的热插拔回归）。
 *
 * 契约：
 *   - 回调在 sd_detect 任务上、持 pk_sdcard 内部锁的上下文中执行，此时
 *     状态已翻成 PK_SD_NO_CARD；
 *   - 回调内只允许拿各自模块自己的锁并关闭/静默自己的 SD I/O，禁止调用
 *     会拿 pk_sdcard 锁的 API（pk_sdcard_format 等；pk_sdcard_is_mounted /
 *     pk_sdcard_state 是 volatile 读，安全）；
 *   - 回调返回时必须保证本模块无打开的 SD fd、无在途 SD I/O。 */
void pk_sdcard_register_pre_unmount_cb(void (*cb)(void));

#ifdef __cplusplus
}
#endif

/* 2026-08-01：pk_sdcard_mount_attempts() 已删。它本想让诊断页区分"没插卡"
 * （计数不动）与"插了但挂不上"（每 3 s 涨一次），但诊断页从来没接过这个数，
 * 从落地起零调用者。计数本身仍在 pk_sdcard.c 里累加并打进日志。 */
