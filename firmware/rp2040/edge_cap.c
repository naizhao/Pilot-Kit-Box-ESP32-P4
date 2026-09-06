/*
 * edge_cap.c — edge_cap.pio 的宿主：PIO RX FIFO --DREQ--> 单 DMA 通道按
 * 块写入 s_blocks（8 × 256 u32），CPU（core1）经 edge_cap_drain 整块取数。
 *
 * block-queue 重设计（gpt-5.6-sol 两轮审计收敛，替换旧 ping-pong 共享环）：
 * 旧设计两通道互链 + 写环连续搬运，发布粒度只能取整环，drain 靠
 * claim/window_ok 边界算术裁决"复制期间被下一圈写穿"（审计 C1/C2）。
 * 新模型把所有权做显式：单通道 ⇒ 同时至多一个在飞块；IRQ 完成 → 发布
 * FULL（release）→ guard 查 FREE → 立即重武装，环满（容量 N−1，恒保留
 * 1 槽贴着消费游标）则 **DMA 停机** + lost 标志 + overrun 计数；消费者
 * 整块 peek→换算→free（release），释放后用 arm_slot 的 guard 重启通道。
 * 一个块绝不同时被 DMA 写和被消费者读；逐条论证见 edge_cap_queue.h
 * 头注释（可靠性论证 1–6），所有权协议本体在该纯单元里、host 可测。
 *
 * 重武装寄存器序列（选定并文档化）：通道停机态下
 *     dma_channel_set_write_addr(ch, ptr, false)      — AL3 非触发写
 *     dma_channel_set_trans_count(ch, BLOCK_ITEMS, true) — AL2_TRIG：
 *       锁存 TRANS_COUNT 并触发（手册 §2.5.1.2：触发即把最近写入的
 *       计数重载进活动计数器）。一次触发同时装好地址与计数。
 * CTRL 配置（32bit、读定址 FIFO、写递增、DREQ、**无 ring、无 chain**）
 * 跨完成保持，无需重写；CHAIN_TO=自身即"无链"（手册明文），本设计
 * 单通道根本不依赖链——重武装由 IRQ 软件完成，交接窗口 = IRQ 延迟
 * （µs 级），RX FIFO 深 4 字兜不住的极端背靠背突发由停机-重启语义
 * 兜底（丢沿如实计数，不静默）。
 *
 * 初始化顺序（审计 C1-init）：状态清零（edgecap_q_init + 计数器）与
 * IRQ 安装先于 dma_channel_configure/触发；首块最后武装。producer
 * （IRQ）的存在先于第一个生产事件，不存在"数据已写、位置未记"的窗口。
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
static uint s_dma_ch;               /* 单 DMA 通道（块队列的生产者）*/
static bool s_started;
static uint32_t s_blocks[EDGE_CAP_Q_N_BLOCKS][EDGE_CAP_Q_BLOCK_ITEMS];

/* 所有权协议状态（合同见 edge_cap_queue.h）。fill_idx/fill_done 唯一
 * 写者 = 本 IRQ（core0）；consume_idx 唯一写者 = drain（core1）。 */
static edgecap_q_t s_q;
static atomic_uint s_overruns;      /* drain/IRQ 写、health 只读（诊断）*/
static atomic_uint s_lost;          /* IRQ 满环停机置位（release）→ drain
                                     * acquire 取走并尝试重启；单消费者
                                     * 独占，无并发取用 */

static void edge_cap_rearm(uint slot)
{
    /* 停机态重武装：先写地址（非触发），再以 TRANS_COUNT_TRIG 锁存计数
     * 并触发（见文件头"重武装寄存器序列"）。 */
    dma_channel_set_write_addr(s_dma_ch, s_blocks[slot], false);
    dma_channel_set_trans_count(s_dma_ch, EDGE_CAP_Q_BLOCK_ITEMS, true);
}

static void __not_in_flash_func(edge_cap_dma_irq)(void)
{
    /* 只认领本通道的 intr 位（写 1 清零）；共享 IRQ 时不越权。handler
     * 只做 掩码/清理/发布/重武装（或停机记账）——不碰块数据、不做换算
     * （换算留在消费者，同旧设计）。 */
    uint32_t intr = dma_hw->intr & (1u << s_dma_ch);
    if (!intr)
        return;
    dma_hw->intr = intr;

    uint32_t next;
    if (edgecap_q_push_full(&s_q, &next)) {
        edge_cap_rearm(next);        /* 下一 FREE 块，无缝续传 */
    } else {
        /* 环满：停机（完成即自停，无需寄存器操作），不重武装。停机窗口
         * 内 PIO 仍在跑、照常 push（DREQ 无消费方）：RX FIFO 塞满后
         * RXSTALL、push noblock 静默丢沿——这是**可接受的垃圾窗口**：
         * 残缺流在重启时被 pio_sm_restart + clear_fifos 整体丢弃，且重启
         * 后首块带 disc 位，消费侧先 modes_edge_reset 再喂（见 drain）。
         * 丢沿如实记 overrun + 置 lost，由 drain 重启。 */
        atomic_fetch_add_explicit(&s_overruns, 1, memory_order_relaxed);
        atomic_store_explicit(&s_lost, 1u, memory_order_release);
    }
}

void edge_cap_start(void)
{
    if (s_started)
        return;

    uint offset = pio_add_program(s_pio, &edgecap_program);
    s_sm = pio_claim_unused_sm(s_pio, true);
    edgecap_program_init(s_pio, s_sm, offset, PIN_PULSES);

    s_dma_ch = dma_claim_unused_channel(true);

    /* 审计 C1-init：全部状态清零 + IRQ 就绪，先于通道配置/触发。
     * SDK 惯例顺序：handler → 通道 IRQ0 使能 → NVIC。 */
    edgecap_q_init(&s_q);
    atomic_store_explicit(&s_overruns, 0, memory_order_relaxed);
    atomic_store_explicit(&s_lost, 0, memory_order_relaxed);

    irq_set_exclusive_handler(DMA_IRQ_0, edge_cap_dma_irq);
    dma_channel_set_irq0_enabled(s_dma_ch, true);
    irq_set_enabled(DMA_IRQ_0, true);

    /* 首块最后武装：guard 在空环恒放行（consume=0）；配置无 ring、
     * 无 chain（CHAIN_TO=自身 = 无链），单通道不依赖硬件回卷。 */
    uint32_t first;
    if (!edgecap_q_arm_slot(&s_q, &first))
        return;                      /* 不可达（init 态空环）；防御性停摆 */

    dma_channel_config c = dma_channel_get_default_config(s_dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);        /* FIFO 定读 */
    channel_config_set_write_increment(&c, true);        /* 块内线性写 */
    channel_config_set_dreq(&c, pio_get_dreq(s_pio, s_sm, false));
    channel_config_set_ring(&c, false, 0);               /* 无写环：IRQ 换块 */
    channel_config_set_chain_to(&c, s_dma_ch);           /* 自身 = 无链 */
    dma_channel_configure(s_dma_ch, &c,
                          s_blocks[first],
                          &s_pio->rxf[s_sm],
                          EDGE_CAP_Q_BLOCK_ITEMS,
                          true);

    s_started = true;
}

/*
 * drain（core1 独占）：块粒度消费。逐块 pop（acquire 看见 FULL 即整块
 * 完整）→ 全块换算 → free（release 交还）。cap 按块取整：剩余容量不足
 * 一块时不弹块（半块交接会破坏"整块 FREE"的所有权语义）。退出前处理
 * 满环停机：本调用释放过块（腾出了槽位）才尝试重启；重启必须走
 * arm_slot 的 guard（环仍满则保持停机，见 edge_cap.h 合同）。
 *
 * 断点传播（gpt-5.6-sol re-audit Fix 1）：带 disc 位的批次把
 * *discontinuity 置 true，消费侧（adsb1090 core1）必须先
 * modes_edge_reset 再喂——断点两侧的 delta 才不会被拼成假 burst。
 */
size_t edge_cap_drain(uint32_t *out, size_t cap, bool *discontinuity)
{
    if (!s_started)
        return 0;
    if (discontinuity)
        *discontinuity = false;

    /* PIO 侧丢沿：push noblock 撞满 RX FIFO 会置本 SM 的 RXSTALL（写 1
     * 清除）。RXSTALL 只记 overrun、**不打 disc 位**：停机窗口外的单沿
     * 丢失是帧内损伤——奇偶/间距已乱，该帧由解码端自然判负（安全丢弃，
     * test_modes_edge 用例 6 锁定）；abs_tick 的少量偏移在下一个 >5µs
     * 帧间长隔处随 burst 重开自愈。只有"停机→重启"这类结构性断点（时间
     * 轴整段缺失）才走 disc → modes_edge_reset 路径。 */
    uint32_t stall_bit = 1u << (PIO_FDEBUG_RXSTALL_LSB + s_sm);
    if (s_pio->fdebug & stall_bit) {
        s_pio->fdebug = stall_bit;
        atomic_fetch_add_explicit(&s_overruns, 1, memory_order_relaxed);
    }

    size_t n = 0;
    uint32_t idx;
    while (cap - n >= EDGE_CAP_Q_BLOCK_ITEMS &&
           edgecap_q_pop_full(&s_q, &idx)) {
        /* 断点位读清后随批上抛（本消费者每批恰一块——adsb1090 的 buf
         * 深度 = 一块；停机前发布的旧块先于 disc 块出队、不带位）。 */
        bool disc = edgecap_q_take_disc(&s_q, idx);
        for (size_t i = 0; i < EDGE_CAP_Q_BLOCK_ITEMS; i++)
            out[n + i] = edgecap_raw_to_ticks(s_blocks[idx][i]);
        edgecap_q_free(&s_q, idx);
        n += EDGE_CAP_Q_BLOCK_ITEMS;
        if (disc && discontinuity)
            *discontinuity = true;
    }

    /* 满环停机的重启：lost 由 IRQ release 置位；取走（acquire）后尝试
     * 重武装。arm_slot 失败 = 环仍满（保留槽未释放）→ 恢复标志等下一拍
     * （arm_slot 失败无副作用，fill_idx 未动）。
     *
     * 重启卫生（断点传播，Fix 1c）：
     *   1. pio_sm_restart + pio_sm_clear_fifos 在武装**之前**执行——停机
     *      窗口里 PIO 塞进 RX FIFO 的残缺值整体丢弃，新块从下一真实沿
     *      干净起录（首条间隔仍以真实上一沿为基准：X 在每个沿无条件
     *      重装，丢的只是 push，不是计数基准；接缝处若恰好落在 burst
     *      中间，那一帧奇偶已乱、由解码端自然判负，见上方 RXSTALL 注释）。
     *   2. 重启武装的第一块 mark disc——该块数据之前有一段整段缺失的
     *      真实时间，消费侧必须先 modes_edge_reset 丢弃半截 burst 再喂，
     *      否则断点前后 delta 拼成假 burst、abs_tick 把永久偏移带进
     *      start_tick。 */
    if (atomic_exchange_explicit(&s_lost, 0u, memory_order_acq_rel)) {
        uint32_t slot;
        if (edgecap_q_arm_slot(&s_q, &slot)) {
            pio_sm_restart(s_pio, s_sm);
            pio_sm_clear_fifos(s_pio, s_sm);
            edge_cap_rearm(slot);
            edgecap_q_mark_disc(&s_q, slot);
        } else {
            atomic_store_explicit(&s_lost, 1u, memory_order_relaxed);
        }
    }

    return n;
}

uint32_t edge_cap_overruns(void)
{
    return atomic_load_explicit(&s_overruns, memory_order_relaxed);
}

uint32_t edge_cap_pending(void)
{
    if (!s_started)
        return 0;
    return edgecap_q_pending(&s_q);  /* FULL 块数（edge_cap.h 合同） */
}
