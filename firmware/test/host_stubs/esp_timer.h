#pragma once
/*
 * host_stubs/esp_timer.h — 单调时钟的注入点。
 *
 * 这里**只声明不定义**：实现由每个测试自己提供，测试因此可以把时间任意
 * 推进。GPS fix 新鲜度、ADS-B 字段过期这类判定全都以它为准，用真实墙钟去
 * 测等于把"等 5 秒"写进单测。
 */
#include <stdint.h>

int64_t esp_timer_get_time(void);
