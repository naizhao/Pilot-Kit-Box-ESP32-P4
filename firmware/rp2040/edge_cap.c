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
 * 发布模型（gpt-5.6-sol re-audit，C1+C2 一并闭合）：producer_pos
 * （atomic_uint，单调、永不回退）的**唯一写者**是 DMA_IRQ_0——每认领一次
 * 通道完成 +EDGE_CAP_RING_ITEMS（一次完成 = 恰好一整环入环；交接瞬间双
 * 位置位按 popcount 计 2，同一块绝不双计），release 发布给 core1。
 * **CPU 侧不读任何 DMA 硬件进度**：完成事件先落硬件、后被 IRQ 记账，
 * 这段错位是"边界→TRANS_COUNT→重读边界"式重检在原理上闭合不了的
 * （C1 反例——重检环两次边界读之间没发生完成 ≠ 硬件没切换过通道），
 * 故细粒度进度整体废除，发布粒度 = 整环 2048 沿。代价是尾批不满块的
 * 突发要等后续流量补满才可见（trailing-burst latency，合同见
 * edge_cap.h ⚠ 段）：真实空中连续流量无感；台架单发自检由
 * selftest_gen 的 flush 脉冲串补环。消费侧裁决算术全部收敛到
 * edge_cap.h 的两个纯函数（host 单测覆盖）：claim（写穿重同步，目标
 * 可为 0）+ window_ok（复制后重校验，裕度 = 整环 − 已复制窗口——复制
 * 期间生产者跨过窗口起点 + 一环即本批作废，C2 的"消费停滞 > 一环时长
 * 被写穿"由它裁决，绝不发布可能被重写过的样本）。
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
     * handler 只做 掩码/清理/原子累加——不碰通道寄存器、不碰环数据，
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
 * drain（core1 独占）：两段裁决，全部只依赖 IRQ 记账的整环边界
 * producer_pos，绝不采样 DMA 硬件寄存器（理由见文件头）。
 *   claim：acquire 边界（与 IRQ 的 release 累加配对）→ 模差；写穿
 *          （滞后 > 一环）先在读取前重同步（目标可为 0，判定只看
 *          resync 标志）。
 *   复制 ≤cap 个样本后重校验（edgecap_window_ok，裕度 = 环 − n）：
 *          生产者跨过窗口起点 + 一环 = 窗口内槽位可能已被重写 →
 *          overruns++、重同步到当前边界 − 环、返回 0（本批作废）；
 *          否则 s_read 前进提交。裁决算术见 edge_cap.h（host 单测
 *          覆盖正常前进/exact-lap/写穿/重同步目标 0/回卷/窗口裕度）。
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

    edgecap_claim_t a = edgecap_claim(
        atomic_load_explicit(&s_producer_pos, memory_order_acquire),
        s_read, EDGE_CAP_RING_ITEMS);
    if (a.resync) {                           /* 被写穿：读取前重同步 */
        atomic_fetch_add_explicit(&s_overruns, 1, memory_order_relaxed);
        s_read = a.resync_to;                 /* 丢旧，保留最新一环（目标可为 0）*/
    }
    size_t n = a.avail < cap ? (size_t)a.avail : cap;
    for (size_t i = 0; i < n; i++)
        out[i] = edgecap_raw_to_ticks(          /* 原值 → 真实间隔 tick（edge_cap.h） */
            s_ring[(s_read + i) & (EDGE_CAP_RING_ITEMS - 1)]);

    /* 发布滞后重校验（C2）：复制期间 IRQ 若已把生产者推过本批窗口
     * 起点 + 一整环，窗口内槽位可能已被重写——本批作废，不发布。 */
    uint32_t now = atomic_load_explicit(&s_producer_pos, memory_order_acquire);
    if (!edgecap_window_ok(now, s_read, (uint32_t)n, EDGE_CAP_RING_ITEMS)) {
        atomic_fetch_add_explicit(&s_overruns, 1, memory_order_relaxed);
        s_read = now - EDGE_CAP_RING_ITEMS;   /* 丢旧，保留最新一环 */
        return 0;
    }
    s_read += (uint32_t)n;
    return n;
}

uint32_t edge_cap_overruns(void)
{
    return atomic_load_explicit(&s_overruns, memory_order_relaxed);
}

uint32_t edge_cap_pending(void)
{
    if (!s_started) return 0;
    return atomic_load_explicit(&s_producer_pos, memory_order_relaxed) -
           s_read;                            /* u32 模差（edge_cap.h 合同） */
}
