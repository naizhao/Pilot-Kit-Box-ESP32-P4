/*
 * config_ble.h — BLE 开关，NVS 持久化（IMPLEMENTATION_PLAN 的 P2-4）。
 *
 * **下次开机生效**，不是即时。原因是硬约束不是偷懒：
 *
 *   1. ble_gatt_init() 里 NimBLE 的 host/controller 一旦起来，IDF 没有
 *      干净的卸载路径——nimble_port_deinit() 之后再 init 在 P4 上并不可靠。
 *   2. 更要命的是**顺序**：hosted 握手必须排在 pk_display_init() 之前，
 *      否则 DSI PHY 抢 LDO ch3 会让 C6 起不来、整机 26 秒一重启
 *      （project_hosted_before_dsi_ldo）。运行时再去启 BLE，等于把那个
 *      顺序打乱。
 *
 * 所以这里只管存意图，main.c 在开机时读它决定要不要调 ble_gatt_init()。
 * 设置页上写明"重启后生效"，别让人以为点了没反应。
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool pk_ble_enabled_get(void);
void pk_ble_enabled_set(bool on);

/* 启动时从 NVS 读取，须在 ble_gatt_init() 之前调用。 */
void pk_config_ble_load(void);

#ifdef __cplusplus
}
#endif
