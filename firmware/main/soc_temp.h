/*
 * soc_temp.h — 片内温度采样与过热告警（顶栏 TEMP 状态位的数据源）。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* 安装并使能片内温度传感器。失败不影响其他功能——pk_soc_temp_get() 会一直
 * 返回 false，顶栏按既有降级逻辑不显示这一格。 */
esp_err_t pk_soc_temp_init(void);

/*
 * 返回当前是否处于过热告警态；temp_c 非空时写入最近一次读数（摄氏、已取整）。
 *
 * 可以每帧调用：内部按 1 Hz 采样，其余时间返回缓存值。
 */
bool pk_soc_temp_get(int *temp_c);
