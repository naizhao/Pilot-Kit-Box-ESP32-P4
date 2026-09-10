#pragma once
/*
 * host_stubs/freertos/queue.h — **真实**的有界队列，不是空壳。
 *
 * 为什么这一个必须是真的：被测的是 sink 在队列之上的策略（满了立刻返回并
 * 计数、drain 按 FIFO 出、格式不变、多生产者下账目不丢），不是队列本身。
 * 把 xQueueSend 桩成恒成功，等于把"满了会怎样"这条判据一起桩掉，测试就只
 * 剩绿灯不带信息。
 *
 * 互斥用 pthread：FreeRTOS 的队列内部是临界区保护的、多生产者安全，host 上
 * 用互斥量复现同一条契约，测试才能真的开多个线程去压它。
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"

typedef unsigned int UBaseType_t;

typedef struct pk_host_queue {
    pthread_mutex_t lock;
    unsigned char  *buf;
    size_t          item_size;
    unsigned        capacity;
    unsigned        head;     /* 入队总数 */
    unsigned        tail;     /* 出队总数 */
} pk_host_queue_t;

typedef pk_host_queue_t *QueueHandle_t;

static inline QueueHandle_t xQueueCreate(unsigned length, size_t item_size)
{
    pk_host_queue_t *q = (pk_host_queue_t *)calloc(1, sizeof(*q));
    if (q == NULL) return NULL;
    q->buf = (unsigned char *)calloc(length, item_size);
    if (q->buf == NULL) { free(q); return NULL; }
    pthread_mutex_init(&q->lock, NULL);
    q->item_size = item_size;
    q->capacity  = length;
    return q;
}

static inline void vQueueDelete(QueueHandle_t q)
{
    if (q == NULL) return;
    pthread_mutex_destroy(&q->lock);
    free(q->buf);
    free(q);
}

/*
 * "本该阻塞的入队"次数：队列已满、且调用方给了非零超时。
 *
 * 为什么要计数而不是真的阻塞：被测契约是"RX 热路径永不等消费者"，也就是
 * 入队必须是 xQueueSend(q, item, 0)。如果桩真的实现阻塞，一旦有人把超时
 * 改回非零，测试会**挂死**——挂死既跑不出退出码，也会被 CI 当成超时而不是
 * 断言失败。所以这里照旧立刻返回 pdFALSE，只把这件事记下来，测试拿它当红灯。
 *
 * 定义成 weak：本头文件会被生产 TU 与测试 TU 同时包含，weak 让链接器只留
 * 一份，测试读到的就是生产代码递增的那一个。
 */
__attribute__((weak)) unsigned pk_host_queue_blocking_sends;

/* ticks 不真的等待：见上面 pk_host_queue_blocking_sends 的说明。 */
static inline int xQueueSend(QueueHandle_t q, const void *item, unsigned ticks)
{
    if (q == NULL) return pdFALSE;
    int ok = pdFALSE;
    pthread_mutex_lock(&q->lock);
    if (q->head - q->tail < q->capacity) {
        memcpy(q->buf + (size_t)(q->head % q->capacity) * q->item_size,
               item, q->item_size);
        q->head++;
        ok = pdTRUE;
    } else if (ticks != 0) {
        /* 满 + 非零超时 = 真机上会在这里睡。记一笔，让测试红。 */
        pk_host_queue_blocking_sends++;
    }
    pthread_mutex_unlock(&q->lock);
    return ok;
}

static inline int xQueueReceive(QueueHandle_t q, void *out, unsigned ticks)
{
    (void)ticks;
    if (q == NULL) return pdFALSE;
    int ok = pdFALSE;
    pthread_mutex_lock(&q->lock);
    if (q->head != q->tail) {
        memcpy(out, q->buf + (size_t)(q->tail % q->capacity) * q->item_size,
               q->item_size);
        q->tail++;
        ok = pdTRUE;
    }
    pthread_mutex_unlock(&q->lock);
    return ok;
}

static inline UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q)
{
    if (q == NULL) return 0;
    pthread_mutex_lock(&q->lock);
    UBaseType_t n = (UBaseType_t)(q->head - q->tail);
    pthread_mutex_unlock(&q->lock);
    return n;
}

static inline void xQueueReset(QueueHandle_t q)
{
    if (q == NULL) return;
    pthread_mutex_lock(&q->lock);
    q->tail = q->head;
    pthread_mutex_unlock(&q->lock);
}
