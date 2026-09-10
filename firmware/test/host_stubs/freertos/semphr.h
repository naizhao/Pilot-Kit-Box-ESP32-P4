#pragma once
/*
 * host_stubs/freertos/semphr.h — 互斥量空壳 + 生命周期计数。
 *
 * host 单线程下加锁本身没有可测内容（退化为空操作是正确等价物），但
 * **建了几把、删了几把**是可测的：init 中途失败（add_device / 建任务失败）
 * 要求把已经建好的 mutex 还回去，否则每次重试都漏一把。
 */
#include <stdint.h>

typedef void *SemaphoreHandle_t;

/* 已创建未删除的 mutex 数。weak：生产 TU 与测试 TU 看同一个变量。 */
__attribute__((weak)) int pk_host_mutex_live;
/* 注入点：非 0 时 xSemaphoreCreateMutex() 返回 NULL（内存不足路径）。 */
__attribute__((weak)) int pk_host_mutex_create_fail;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    if (pk_host_mutex_create_fail) return (SemaphoreHandle_t)0;
    pk_host_mutex_live++;
    /* 句柄取一个可区分的非空值，方便测试断言"不是同一把被建了两次"。 */
    return (SemaphoreHandle_t)(intptr_t)(0x1000 + pk_host_mutex_live);
}
static inline void vSemaphoreDelete(SemaphoreHandle_t lock)
{ (void)lock; pk_host_mutex_live--; }
static inline int xSemaphoreTake(SemaphoreHandle_t lock, int delay)
{ (void)lock; (void)delay; return 1; }
static inline int xSemaphoreGive(SemaphoreHandle_t lock)
{ (void)lock; return 1; }
