/*
 * record_sink_uart.c — 非阻塞 UART/stdout sink（入队 + 后台排空）。
 *
 * 每条 CRC 有效的 Mode-S 报文输出一行 Pilot-Kit ts 格式：
 *
 *     1715432198765 *8D4840D6202CC371C32CE0576098;
 *
 * 这就是 Pilot-Kit/scripts/adsb_to_track.py 逐字节解析的线格式，所以用任何
 * 串口记录器（idf.py monitor / minicom / pyserial）把固件输出存成文件，就是一
 * 份可直接导入的 dump。也正因为要逐字节对得上，这里用裸 printf 而不是
 * ESP_LOGx——后者会加上 "I (12345) tag: " 前缀，把下游解析器打坏。
 *
 * 为什么不再在 write() 里直接打印（P1-D）
 * ---------------------------------------
 * record_dispatch() 由 adsb_link_task 在**每一帧**上同步调用，而那个任务是
 * 921600 波特链路唯一的 RX 消费者（RX 环只有 4096 字节 ≈ 44 ms 的数据量）。
 * 一行约 45 字节，115200 波特的控制台上要占 ≈ 3.9 ms。繁忙空域每秒几百帧
 * 时，RX 任务绝大部分时间在等控制台，UART 环溢出——帧丢在驱动里，**没有任何
 * 计数**能看出来。
 *
 * 所以生产者与消费者在这里被拆开：write() 只做一次定长 memcpy + 非阻塞入队
 * （xQueueSend 超时**必须**是 0），真正的打印全部移到 uart_sink_task 上。
 * 控制台吃不下时溢出的部分记进 s_dropped，而不是无声消失——1 Hz dashboard
 * 与诊断页 LOG 卡片都会读它（record_sink_uart_stats）。这跟
 * record_sink_rec_store.c / record_sink_file.c 是同一个模式。
 *
 * 账目恒等式（合同，见 test/test_record_sink_backpressure.c）：
 *
 *     written + dropped + pending == dispatch 次数
 *
 * 计数器用 C11 atomic 而不是 volatile：record_sink.h 写明 record_dispatch()
 * 可由任意任务调用，实际也确有第二个生产者（pk_rec_selftest.c）。volatile 只
 * 防编译器缓存、没有跨线程语义，两个生产者同时 s_dropped++ 会静默丢计数，恒
 * 等式就不成立了。relaxed 序足够：各计数单调，读侧是 1 Hz 的诊断口径，不做
 * 跨字段一致性承诺（与 adsb_link_task.c 的 s_stats 同一口径）。
 */

#include "record_sink.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "uart_sink";

/* 128 槽 × 40 字节 ≈ 5 KB。深度按"控制台一秒吐得动约 256 行"定：半秒的缓冲
 * 足够吸收突发，再深就只是把已经过时的报文攒在内存里——过载时新报文比旧报文
 * 有价值，攒着不如丢掉并计数。 */
#define UART_SINK_QUEUE_DEPTH  128
#define UART_SINK_TASK_STACK   3072
/* 排空任务一次最多连打多少行就让渡。持续过载时不限量会让这条任务独占 115200
 * 波特的控制台，把别的任务的日志全堵在后面——丢弃是设计内的，静默霸占控制台
 * 不是。 */
#define UART_SINK_DRAIN_BURST  16

/* 定长快照：record_t 里 icao24/df/hex_len 这三个字段打印时用不到，不进队列
 * （每槽省 8 字节，128 槽就是 1 KB）。 */
typedef struct {
    int64_t ts_ms;
    char    hex[RECORD_HEX_MAX_LEN + 1];
} uart_item_t;

static QueueHandle_t s_queue;
static atomic_uint   s_written;   /* 自启动累计已打印行数 */
static atomic_uint   s_dropped;   /* 自启动累计入队失败（队满）次数 */
static record_sink_t s_sink;

/*
 * 打印一行并记账。只在排空侧调用——生产侧（uart_write）绝不走到这里，那正是
 * 本次改造的全部意义。
 */
static void emit_line(const uart_item_t *item)
{
    printf("%lld *%s;\n", (long long)item->ts_ms, item->hex);
    atomic_fetch_add_explicit(&s_written, 1, memory_order_relaxed);
}

/*
 * record_dispatch() 的同步调用点，跑在 RX 任务上。
 *
 * 这个函数体里不允许出现任何输出或慢速 I/O——firmware/scripts/
 * test_rx_hot_path_logging.py 按函数体做结构守卫，printf/fwrite/ESP_LOG 一个
 * 都不能有，xQueueSend 的超时必须是字面 0。
 */
static bool uart_write(record_sink_t *self, const record_t *rec)
{
    (void)self;
    if (s_queue == NULL) return false;

    uart_item_t item;
    item.ts_ms = rec->ts_ms;
    /* 定长整段拷贝，不用 rec->hex_len 当长度：那是个 uint8_t，值来自 RF 解出
     * 的 msgbits，一旦有生产者填错就会越界读写。两边都是 RECORD_HEX_MAX_LEN+1
     * 的定长数组，常量长度的 memcpy 代价不比按长度拷高，却不依赖任何调用方
     * 把长度填对。末字节强制归零，保证下面 printf("%s") 一定有终止符。 */
    memcpy(item.hex, rec->hex, sizeof(item.hex));
    item.hex[sizeof(item.hex) - 1] = '\0';

    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        atomic_fetch_add_explicit(&s_dropped, 1, memory_order_relaxed);
        return false;
    }
    return true;
}

size_t record_sink_uart_drain(size_t max_lines)
{
    if (s_queue == NULL) return 0;

    size_t n = 0;
    uart_item_t item;
    /* 出队超时也是 0：排空侧空转一圈的代价远小于"睡在队列上错过让渡点"。
     * 真正的空闲等待在 uart_sink_task 里做。 */
    while (n < max_lines && xQueueReceive(s_queue, &item, 0) == pdTRUE) {
        emit_line(&item);
        ++n;
    }
    return n;
}

static void uart_sink_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* 空闲时睡在队列上而不是轮询：这条任务优先级低，但也不该白烧 CPU。 */
        uart_item_t item;
        if (xQueueReceive(s_queue, &item, portMAX_DELAY) != pdTRUE) continue;
        emit_line(&item);

        (void)record_sink_uart_drain(UART_SINK_DRAIN_BURST - 1);

        /* 必须是 vTaskDelay(1) 而不是 taskYIELD()：同优先级让渡不会把 CPU 交
         * 给 IDLE，持续过载下这条任务会把 IDLE 饿死（看门狗随即报警）。 */
        vTaskDelay(1);
    }
}

record_sink_t *record_sink_uart_create(void)
{
    s_queue = xQueueCreate(UART_SINK_QUEUE_DEPTH, sizeof(uart_item_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return NULL;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(uart_sink_task, "uart_sink",
                                            UART_SINK_TASK_STACK, NULL, 2, NULL, 0);
    if (ok != pdTRUE) {
        /* 没人排空就不能把 sink 注册上去——注册了只会灌满队列然后全部静默
         * 丢弃。队列必须还回去：128 槽 × 40 字节 ≈ 5 KB，而这条路径的典型触发
         * 原因恰恰就是内存紧张（P4 rev<v3 在调度器启动前只有 65 KB 内部堆）。
         * s_queue 归 NULL 还有第二重意义：record_sink_uart_stats() 据此对外
         * 承认这一路不存在，而不是报三个 0 谎称"活着且一条没丢"。 */
        ESP_LOGE(TAG, "xTaskCreatePinnedToCore(uart_sink) failed");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return NULL;
    }

    s_sink.name  = "uart";
    s_sink.write = uart_write;
    s_sink.priv  = NULL;
    return &s_sink;
}

bool record_sink_uart_stats(uint32_t *out_written, uint32_t *out_dropped,
                            uint32_t *out_pending)
{
    if (s_queue == NULL) return false;
    if (out_written)
        *out_written = atomic_load_explicit(&s_written, memory_order_relaxed);
    if (out_dropped)
        *out_dropped = atomic_load_explicit(&s_dropped, memory_order_relaxed);
    if (out_pending)
        *out_pending = (uint32_t)uxQueueMessagesWaiting(s_queue);
    return true;
}
