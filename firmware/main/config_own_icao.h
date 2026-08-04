/*
 * config_own_icao.h — 本机 ICAO 绑定关系，NVS 持久化。
 *
 * own_icao 是用户在 ADS-B 列表里选"这架是我"后记录的 24-bit ICAO 地址。
 * 之前纯 RAM（ui_state.c 的 s_own_icao_runtime），重启即丢；本模块把它
 * 落进 NVS，开机时 pk_config_own_icao_load() 读回并通过 pk_ui_set_own_icao()
 * 恢复绑定。0 = 无绑定/已解绑，!=0 = 绑定的 ICAO24。
 *
 * 结构照 config_ac_category.c（volatile + ensure_nvs + get/set/load）；
 * 独立 namespace 而非并进 config_traffic，因为 own_icao 喂的是 PFD 的
 * ALT/VS/GS 数据源选择，与地图显示偏好不是一回事。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 立即写 NVS（在 pk_ui_set_own_icao 内部调，释放 mutex 后） */
void pk_config_own_icao_set(uint32_t icao24);

/* 写 0 进 NVS = 解绑（在 pk_ui_clear_own_icao 内部调） */
void pk_config_own_icao_clear(void);

/* 开机读 NVS，非 0 则调 pk_ui_set_own_icao() 恢复绑定 */
void pk_config_own_icao_load(void);

#ifdef __cplusplus
}
#endif
