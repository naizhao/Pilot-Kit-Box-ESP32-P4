/*
 * config_storage.h — ADS-B 日志存储位置（flash LittleFS / microSD），NVS 持久化。
 *
 * 设置在下次创建 file sink 时生效（即重启后）；运行中切换只改 NVS。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PK_LOG_STORE_FLASH = 0,   /* 板载 flash LittleFS "storage" 分区 */
    PK_LOG_STORE_SD    = 1,   /* microSD /sdcard（缺卡时自动回退 flash） */
} pk_log_store_t;

pk_log_store_t pk_log_store_get(void);
void           pk_log_store_set(pk_log_store_t s);

/* 启动时从 NVS 读取，须在 record_sinks_install_defaults() 前调用。 */
void pk_config_storage_load(void);

#ifdef __cplusplus
}
#endif
