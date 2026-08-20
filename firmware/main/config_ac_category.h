/*
 * config_ac_category.h — 设置页「机型分类」+ NVS 持久化。
 *
 * 设计依据 ADS-B 数据持久化设计（内部文档）
 * 「机型分类阈值」一节：设置页加一行分段控件，NVS 存 u8，默认轻型活塞
 * （本产品用户主流）。分类枚举定义在 pk_flight_phase.h（pk_ac_category_t），
 * 阈值表/振动地板初值都已在那边按分类实现——本文件只负责"用户选的是哪一档"
 * 这一件事的存取，不重复任何阈值逻辑。
 *
 * 结构照 config_traffic.c（volatile + portMUX + ensure_nvs + get/set/load）；
 * 单独开一个模块而不是并进 config_traffic 的 "pk_tfc" namespace——机型分类
 * 喂给的是 pk_flight_phase 相位状态机，跟地图朝向/雷达量程这两个纯显示偏好
 * 不是一回事，混进同一个 namespace 会让下一个读 config_traffic.c 的人以为
 * 这仨都是"地图显示设置"。
 */
#pragma once

#include "pk_flight_phase.h"    /* pk_ac_category_t */

#ifdef __cplusplus
extern "C" {
#endif

pk_ac_category_t pk_ac_category_get(void);
void              pk_ac_category_set(pk_ac_category_t cat);  /* 立即 NVS 持久化 */

void pk_config_ac_category_load(void);  /* 开机加载，默认 PK_AC_CAT_PISTON_LIGHT */

#ifdef __cplusplus
}
#endif
