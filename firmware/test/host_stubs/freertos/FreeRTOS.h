#pragma once
/*
 * host_stubs/freertos/FreeRTOS.h — 只替换调度/锁原语，不替换任何被测逻辑。
 *
 * host 测试是单线程的：临界区与互斥量退化成空操作是正确的等价物，因为测试
 * 里不存在第二个执行流去竞争。凡是会改变**被测判定**的东西都不在这里
 * （时间由测试自己提供 esp_timer_get_time，见 host_stubs/esp_timer.h）。
 */
#include <assert.h>

#define portMAX_DELAY (-1)

#ifndef pdTRUE
#define pdTRUE  1
#define pdFALSE 0
#endif

#ifndef pdMS_TO_TICKS
#define pdMS_TO_TICKS(ms) (ms)
#endif

#ifndef configASSERT
#define configASSERT(x) assert(x)
#endif

/* 单线程 host 上没有真正的并发，临界区退化为空操作。 */
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux)      do { (void)(mux); } while (0)
#define portEXIT_CRITICAL(mux)       do { (void)(mux); } while (0)
#define portENTER_CRITICAL_ISR(mux)  do { (void)(mux); } while (0)
#define portEXIT_CRITICAL_ISR(mux)   do { (void)(mux); } while (0)
