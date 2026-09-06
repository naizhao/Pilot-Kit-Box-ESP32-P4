/*
 * edge_cap.c — edge_cap.pio 的宿主：PIO RX FIFO --DREQ--> DMA 环写 s_ring，
 * CPU 轮询 edge_cap_drain 取数。
 *
 * DMA 连续搬运采用 RP2040 数据手册 §2.5.2.2 的 ping-pong 双通道互链
 * （s_dma[0] 完成→立即触发 s_dma[1]，反之亦然）。原方案的自链不可行：
 * 手册明文 "A channel can not chain to itself. Setting CHAIN_TO to a
 * channel's own index means no chaining will take place."（CHAIN_TO=自己
 * 即“无链”，一环写完通道就停）。互链的连续性由两条寄存器语义保证：
 *   · TRANS_COUNT：通道每次被触发都把最近写入值重载进活动计数器
 *     （§2.5.1.2），两通道都写 EDGE_CAP_RING_ITEMS，被触发即整环重载；
 *   · READ/WRITE_ADDR：不重编程则沿用当前值作下一轮起点（§2.5.1.1），
 *     配合写环（RING_SEL=write, RING_SIZE=13，仅低 13 位变化、2^13 字节
 *     边界回卷）写完一整环后地址恰好回到 s_ring[0]，两通道交替无缝。
 * 通道交接（硬件即时触发）只隔几个周期；未开 FIFO join 时 RX FIFO
 * 深 4 字，4 字 ≫ 交接窗口，足以兜住间隔样本。
 *
 * 写环用 channel_config_set_ring(&c, true, 13)（pico-sdk dma.h 的正规
 * 写法，等价于 CTRL.RING_SEL/RING_SIZE 字段）；不用手工 al1_write_addr_trig
 * 掩码——那只是改一次起始地址再触发，并不构成硬件回卷。
 *
 * 单调生产位置（audit round 4，单写者重设计）：生产位置不再由 CPU 采样
 * 拼接（s_blocks 先读 + TRANS_COUNT 后读的两步采样在交接瞬间可拼出
 * "陈旧块数 × 新通道偏移"的假位置，producer_pos < s_read 无符号下溢 →
 * 假 overrun → s_read 回滚——本类竞态由设计消除）。改为：DMA 完成中断
 * 是 producer_pos 的**唯一写者**，每认领一次通道完成 +EDGE_CAP_RING_ITEMS
 * （两通道交替写同一环，一次完成 = 恰好一整环；交接瞬间两位置位按 2 计，
 * 同一块绝不双计）。producer_pos 单调不减、按 2^32 回卷；drain 以
 * acquire 读之，与 IRQ 的 release 累加配对。裁决算术全部收进 edge_cap.h
 * 的纯函数 edgecap_avail()（host 单测覆盖，见该头注释）。
 */
#include <stdatomic.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "edge_cap.h"
#include "edge_cap.pio.h"
#include "board_pins.h"

static PIO  s_pio = pio0;
static uint s_sm;
static uint s_dma[2];       /* ping-pong 互链的两个通道 */
static bool s_started;
static uint32_t s_ring[EDGE_CAP_RING_ITEMS] __attribute__((aligned(4 * EDGE_CAP_RING_ITEMS)));
static uint32_t s_read;     /* 消费位置（单调，仅 core1 的 drain 写；
                             * 与 producer_pos 同为 u32 模差算术，见
                             * edge_cap.h 的合同）*/
static atomic_uint s_producer_pos;  /* 唯一写者 = DMA IRQ（core0）；
                             * drain（core1）acquire 读。回卷安全，见
                             * edge_cap.h。 */
static atomic_uint s_overruns;      /* drain 写 / core0 的 health_fill 读：
                             * 跨核、relaxed 即可（诊断计数）*/

static void ring_channel_setup(uint ch, uint other, bool trigger)
{
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);        /* FIFO 定读 */
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(s_pio, s_sm, false));
    channel_config_set_ring(&c, true, 13);               /* 写环 2^13=8KB（s_ring 已 8KB 对齐）*/
    channel_config_set_chain_to(&c, other);              /* 完成→触发对方（手册 §2.5.2.2）*/
    dma_channel_configure(ch, &c,
                          s_ring,
                          &s_pio->rxf[s_sm],
                          EDGE_CAP_RING_ITEMS,            /* 每轮一整环；触发即重载 */
                          trigger);
}

static void __not_in_flash_func(edge_cap_dma_irq)(void)
{
    /* 只认领并清理本组两通道的 intr 位（写 1 清零）；其它通道的位原样
     * 保留，共享 IRQ 时不越权。每认领一次完成事件 = 恰好一整环入环
     * （popcount 兼顾交接瞬间双位置位），release 发布给 core1 的 drain。
     * handler 只做 掩码/清理/原子累加——不碰 TRANS_COUNT、不碰环数据，
     * 最小窗口。 */
    uint32_t intr = dma_hw->intr & ((1u << s_dma[0]) | (1u << s_dma[1]));
    if (intr) {
        dma_hw->intr = intr;
        atomic_fetch_add_explicit(&s_producer_pos,
                                  (uint32_t)__builtin_popcount(intr)
                                      * EDGE_CAP_RING_ITEMS,
                                  memory_order_release);
    }
}

void edge_cap_start(void)
{
    if (s_started) return;

    uint offset = pio_add_program(s_pio, &edgecap_program);
    s_sm = pio_claim_unused_sm(s_pio, true);
    edgecap_program_init(s_pio, s_sm, offset, PIN_PULSES);

    s_dma[0] = dma_claim_unused_channel(true);
    s_dma[1] = dma_claim_unused_channel(true);

    /* SDK 惯例顺序：先装 handler，再开通道的 IRQ0 使能，最后开 NVIC——
     * 使能与装载之间不得留有中断可触发的窗口。IRQ 在通道启动（下方
     * ring_channel_setup 触发）之前即已就绪：producer_pos 的写者先于
     * 第一个生产者存在，不存在"数据已写、位置未记"的窗口。 */
    irq_set_exclusive_handler(DMA_IRQ_0, edge_cap_dma_irq);
    dma_channel_set_irq0_enabled(s_dma[0], true);
    dma_channel_set_irq0_enabled(s_dma[1], true);
    irq_set_enabled(DMA_IRQ_0, true);

    ring_channel_setup(s_dma[1], s_dma[0], false);       /* 先备好待触发 */
    ring_channel_setup(s_dma[0], s_dma[1], true);        /* 再开 0；整环后自动交给 1 */

    s_read = 0;
    atomic_store_explicit(&s_producer_pos, 0, memory_order_relaxed);
    atomic_store_explicit(&s_overruns, 0, memory_order_relaxed);
    s_started = true;
}

/*
 * drain（core1 独占）：以 acquire 读生产位置（与 IRQ 的 release 累加
 * 配对，拿到的是"已完整入环"的样本数），其余裁决交给 edgecap_avail()
 * 纯函数（edge_cap.h；host 单测覆盖正常前进/exact-lap/写穿重同步/u32
 * 回卷无下溢）。数据下标 = s_read 的环内掩码。
 */
size_t edge_cap_drain(uint32_t *out, size_t cap)
{
    if (!s_started) return 0;

    /* PIO 侧丢沿：push noblock 撞满 RX FIFO 会置本 SM 的 RXSTALL（写 1 清除）*/
    uint32_t stall_bit = 1u << (PIO_FDEBUG_RXSTALL_LSB + s_sm);
    if (s_pio->fdebug & stall_bit) {
        s_pio->fdebug = stall_bit;
        atomic_fetch_add_explicit(&s_overruns, 1, memory_order_relaxed);
    }

    uint32_t producer = atomic_load_explicit(&s_producer_pos,
                                             memory_order_acquire);
    edgecap_avail_t a = edgecap_avail(producer, s_read, EDGE_CAP_RING_ITEMS);
    if (a.overrun_resync_to) {                    /* 被写穿：跳到头重同步 */
        atomic_fetch_add_explicit(&s_overruns, 1, memory_order_relaxed);
        s_read = a.overrun_resync_to;
    }
    size_t n = a.avail < cap ? (size_t)a.avail : cap;
    for (size_t i = 0; i < n; i++)
        out[i] = edgecap_raw_to_ticks(          /* 原值 → 真实间隔 tick（edge_cap.h） */
            s_ring[(s_read + i) & (EDGE_CAP_RING_ITEMS - 1)]);
    s_read += (uint32_t)n;
    return n;
}

uint32_t edge_cap_overruns(void)
{
    return atomic_load_explicit(&s_overruns, memory_order_relaxed);
}
