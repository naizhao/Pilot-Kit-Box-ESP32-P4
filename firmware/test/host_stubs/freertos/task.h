#pragma once
/*
 * host_stubs/freertos/task.h — 任务创建/延时/退出的空壳 + 注入点。
 *
 * 两种用法，都在用：
 *   1. 不启动任务循环：测试直接调模块的公开入口（例如 pk_gps_feed_line）。
 *      xTaskCreatePinnedToCore 返回 pdTRUE 而不真起线程即可；
 *   2. **就在本线程里跑任务函数**（test_baro_bringup / test_imu_bringup）：
 *      被测的正是那个 while(1) 里的启动/自愈状态机，只有真跑它才算测到。
 *      这时 vTaskDelay 是任务唯一的让渡点，于是做成钩子——测试在钩子里推进
 *      模拟时钟、采样对外状态，跑够了再 longjmp 出来。
 *
 * 这里只替换调度原语，不替换任何被测判定。
 */
#include "FreeRTOS.h"

typedef int BaseType_t;
typedef unsigned int TickType_t;

/*
 * 注入点：非 0 时 xTaskCreatePinnedToCore() 返回 pdFALSE。
 *
 * "任务起不来"是真机上会发生、而且发生时内存已经很紧张的一条路径，模块在
 * 那条路径上该释放什么、该不该对外宣称自己活着，只有把失败注入进来才测得到。
 * 默认 0，不影响任何既有测试。
 *
 * weak：本头文件被生产 TU 与测试 TU 同时包含，weak 让两边看到同一个变量。
 */
__attribute__((weak)) int pk_host_task_create_fail;

/* 观察点：最近一次创建的任务函数/参数，以及创建成功的次数。测试据此跑
 * **生产代码真正注册的那个函数**，而不是自己另找入口——入口选错了，测的
 * 就不是产品在跑的东西。 */
__attribute__((weak)) void (*pk_host_task_last_fn)(void *);
__attribute__((weak)) void  *pk_host_task_last_arg;
__attribute__((weak)) int    pk_host_task_create_count;

/* 观察点：任务自己调 vTaskDelete 退出。默认 NULL = 空操作；测试装上自己的
 * 函数，用来断言"长期任务不得因为一次瞬态失败就退出"。
 *
 * 用函数指针而不是 weak 函数：weak 函数要靠链接器让测试的强定义压过生产 TU
 * 里的弱定义，跨平台行为微妙；函数指针是运行时装配，没有这层不确定性。 */
__attribute__((weak)) void (*pk_host_task_delete_hook)(void *handle);

/* 让渡点钩子：见文件头第 2 种用法。参数就是 vTaskDelay 收到的 tick 数
 * （pdMS_TO_TICKS 在 host 上是恒等，所以它同时就是毫秒数）。 */
__attribute__((weak)) void (*pk_host_task_delay_hook)(unsigned ms);

static inline BaseType_t xTaskCreatePinnedToCore(void (*fn)(void *),
                                                 const char *name,
                                                 unsigned stack, void *arg,
                                                 unsigned prio, void **handle,
                                                 int core)
{
    (void)name; (void)stack; (void)prio; (void)core;
    if (pk_host_task_create_fail) return pdFALSE;
    pk_host_task_last_fn  = fn;
    pk_host_task_last_arg = arg;
    pk_host_task_create_count++;
    if (handle) *handle = (void *)1;
    return pdTRUE;
}

static inline void vTaskDelay(TickType_t ticks)
{ if (pk_host_task_delay_hook) pk_host_task_delay_hook((unsigned)ticks); }

static inline void vTaskDelete(void *handle)
{ if (pk_host_task_delete_hook) pk_host_task_delete_hook(handle); }
