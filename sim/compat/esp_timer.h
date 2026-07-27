/* esp_timer.h — 模拟器桩。
 *
 * 固件用 esp_timer_get_time() 取单调微秒时钟。PC 上用 clock_gettime
 * 的单调时钟等价替代 —— 渲染代码只拿它做时间差与陈旧判定，绝对值无意义。
 */
#pragma once

#include <stdint.h>
#include <time.h>

static inline int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}
