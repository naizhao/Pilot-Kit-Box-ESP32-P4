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
 * 单通道根本不依赖链——重武装由 IRQ 软件完成，交接窗口 = IRQ 延迟。
 * RX FIFO 已开 join（edgecap_program_init 的 sm_config_set_fifo_join，
 * TX 并入 RX）达 8 深：0.5µs 最短边沿间隔下的缓冲预算 = 8 × 0.5µs =
 * 4µs，覆盖 IRQ 重武装窗口（完整时序预算合同见 edge_cap.h）；超出预算
 * 的极端背靠背突发仍由停机-重启语义兜底（丢沿如实计数，不静默）。
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
static uint s_offset;               /* edgecap 程序装入偏移（重初始化的 PC 起点）*/
static pio_sm_config s_cfg;         /* start 时保存的 sm_config（确定性重装用）*/
static uint s_dma_ch;               /* 单 DMA 通道（块队列的生产者）*/
static bool s_started;
static uint32_t s_blocks[EDGE_CAP_Q_N_BLOCKS][EDGE_CAP_Q_BLOCK_ITEMS];

/* 所有权协议状态（合同见 edge_cap_queue.h）。fill_idx/fill_done 唯一
 * 写者 = 本 IRQ（core0）；consume_idx 唯一写者 = drain（core1）。 */
static edgecap_q_t s_q;
static atomic_uint s_overruns;      /* drain/IRQ 写、health 只读（诊断）*/
static atomic_uint s_lost;          /* IRQ 满环停机置位（release）→ core0
                                     * service acquire 取走并尝试重启 */
static atomic_uint s_completions;   /* IRQ 完成计数：core0 service 判停摆 */

static void edge_cap_rearm(uint slot)
{
    /* 停机态重武装：先写地址（非触发），再以 TRANS_COUNT_TRIG 锁存计数
     * 并触发（见文件头"重武装寄存器序列"）。 */
    dma_channel_set_write_addr(s_dma_ch, s_blocks[slot], false);
    dma_channel_set_trans_count(s_dma_ch, EDGE_CAP_Q_BLOCK_ITEMS, true);
}

/*
 * PIO 确定性重初始化（gpt-5.6-sol round-2 audit；勘误 re-audit round-2）：
 * pio_sm_restart 只清 ISR/移位计数等执行暂存——**X/Y 与 PC 保留**、不重装
 * exec/shift 配置；单用 restart + 清 FIFO 后 SM 会带着旧 PC、可能残缺的 X
 * 从程序中段继续跑出垃圾流。完整序列必须等价冷启动：
 *     停用 → pio_sm_restart → 清 FIFO → pio_sm_init（重装配置、PC 回程序
 *     入口）+ 重载 X（程序头 set x,31 重做）→ 重新使能。
 * s_cfg 是 start 时 edgecap_program_init 返回并保存的同款配置。
 */
static void edge_cap_pio_flush(void)
{
    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_restart(s_pio, s_sm);
    pio_sm_clear_fifos(s_pio, s_sm);
    pio_sm_init(s_pio, s_sm, s_offset, &s_cfg);
    pio_sm_set_enabled(s_pio, s_sm, true);
}

/* __not_in_flash_func：handler 常驻 SRAM，XIP cache miss 不得给重武装
 * 窗口加延迟（RX FIFO join 后预算仅 4µs，见 edge_cap.h 时序预算）。
 * 优先级注记：DMA IRQ 与 USB 等共享 NVIC——本 handler 不得被遮蔽超过
 * FIFO 预算（4µs）；与 USB 中断的完整优先级整定是 Task 15 台架项，
 * 验收条款「持续边沿下 RXSTALL/overrun == 0」由台架实测裁决。 */
static void __not_in_flash_func(edge_cap_dma_irq)(void)
{
    /* 只认领本通道的 intr 位（写 1 清零）；共享 IRQ 时不越权。handler
     * 只做 掩码/清理/发布/重武装（或停机记账）——不碰块数据、不做换算
     * （换算留在消费者，同旧设计）。 */
    uint32_t intr = dma_hw->intr & (1u << s_dma_ch);
    if (!intr)
        return;
    dma_hw->intr = intr;
    atomic_fetch_add_explicit(&s_completions, 1, memory_order_relaxed);

    uint32_t next;
    if (edgecap_q_push_full(&s_q, &next)) {
        edge_cap_rearm(next);        /* 下一 FREE 块，无缝续传 */
    } else {
        /* 环满：停机（完成即自停，无需寄存器操作），不重武装。停机窗口
         * 内 PIO 仍在跑、照常 push（DREQ 无消费方）：RX FIFO 塞满后
         * RXSTALL、push noblock 静默丢沿——这是**可接受的垃圾窗口**：
         * 残缺流在重启时被 edge_cap_pio_flush（完整确定性重初始化，见
         * 上）整体丢弃，且重启后首块带 disc 位，消费侧先 modes_edge_reset
         * 再喂（见 drain）。丢沿如实记 overrun + 置 lost，由 drain 重启。 */
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
    s_offset = offset;
    s_cfg = edgecap_program_init(s_pio, s_sm, offset, PIN_PULSES);

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
 * drain（core1 独占）：**每次调用至多取走一整块**（round-2 Fix 4）。
 * 弹出一块（acquire 看见 FULL 即整块完整）→ 全块换算 → free（release
 * 交还）；队列空或 cap 不足一块（256 条）时返回 0——跨断点的多块批次
 * 无法表达"断点在哪"，消费者要在块间 reset 解码器，逐块交接才让断点
 * 位置精确落在块边界（adsb1090 core1 循环反复调用，返回 0 让出）。
 *
 * 断点传播（gpt-5.6-sol re-audit Fix 1）：带 disc 位的块把
 * *discontinuity 置 true，消费侧（adsb1090 core1）必须先
 * modes_edge_reset 再喂——断点两侧的 delta 才不会被拼成假 burst。
 */
size_t edge_cap_drain(uint32_t *out, size_t cap, bool *discontinuity)
{
    if (!s_started || cap < EDGE_CAP_Q_BLOCK_ITEMS)
        return 0;
    if (discontinuity)
        *discontinuity = false;

    /* PIO 侧丢沿：push noblock 撞满 RX FIFO 会置本 SM 的 RXSTALL（写 1
     * 清除）。RXSTALL 只记 overrun、**不打 disc 位**：停机窗口外的单沿
     * 丢失是帧内损伤——奇偶/间距已乱，该帧由解码端自然判负（安全丢弃，
     * test_modes_edge 用例 6 锁定）。丢失时长不可知 ⇒ 断点后的时间基
     * **永久偏小**：单调性仍保持，但后续帧的 rp_ts_us 带轻微提前偏置
     * ——这是丢沿的固有结果，**不是可自愈项**（解码器只累积收到的
     * delta，丢失段无法回补；退化由 adsb1090 消费侧 mark_degraded 如实
     * 上报）。只有"停机→重启"这类结构性断点（时间轴整段缺失）才走
     * disc → modes_edge_reset 路径。 */
    uint32_t stall_bit = 1u << (PIO_FDEBUG_RXSTALL_LSB + s_sm);
    if (s_pio->fdebug & stall_bit) {
        s_pio->fdebug = stall_bit;
        atomic_fetch_add_explicit(&s_overruns, 1, memory_order_relaxed);
    }

    uint32_t idx;
    if (!edgecap_q_pop_full(&s_q, &idx))
        return 0;
    /* 断点位读清后随块上抛（本调用恰一块——断点位置精确到块边界；
     * 停机前发布的旧块先于 disc 块出队、不带位）。 */
    bool disc = edgecap_q_take_disc(&s_q, idx);
    for (size_t i = 0; i < EDGE_CAP_Q_BLOCK_ITEMS; i++)
        out[i] = edgecap_raw_to_ticks(s_blocks[idx][i]);
    edgecap_q_free(&s_q, idx);
    if (disc && discontinuity)
        *discontinuity = true;

    /* 满环停机的重启**不在这里做**：实测（2026-09-10）core1 侧调用
     * edge_cap_rearm 会返回 BUSY=1 但完成中断再不来，管道永久静默；
     * 同一条序列从 core0 调用则有效。故 DMA 重武装统一收归 core0 的
     * edge_cap_service()（见下），core1 只做消费。 */
    return EDGE_CAP_Q_BLOCK_ITEMS;
}

/* core0 唯一的 DMA 重武装/恢复入口（主循环每次调用）：
 *   · lost 置位（IRQ 满环停机）→ 立即尝试重启；
 *   · ≥5 ms 无完成中断且 DMA 已停 → 判定静默停摆，重启。
 * 重启卫生（round-2 Fix 3，确定性重初始化）：先 edge_cap_pio_flush
 * （停用 → restart → 清 FIFO → pio_sm_init 重装配置 + PC 回程序起点 →
 * 使能）丢弃停机窗口里的残缺流，再重武装；首块 mark disc，消费侧先
 * modes_edge_reset 再喂（时间基保留、断点后单调，见 modes_edge.h）。 */
void edge_cap_service(void)
{
    if (!s_started)
        return;
    static uint32_t last_seen, last_ms;
    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t c = atomic_load_explicit(&s_completions, memory_order_relaxed);
    if (c != last_seen) {
        last_seen = c;
        last_ms = now;
    }
    bool lost = atomic_load_explicit(&s_lost, memory_order_acquire);
    if (!lost && (uint32_t)(now - last_ms) < 5u)
        return;                       /* 刚完成过且无 lost：正常，不动 */
    if (dma_channel_is_busy(s_dma_ch))
        return;                       /* 仍在飞：正常，不动 */
    uint32_t slot;
    if (!edgecap_q_arm_slot(&s_q, &slot))
        return;                       /* 环满：保留 lost，下拍再试 */
    edge_cap_pio_flush();
    edge_cap_rearm(slot);
    edgecap_q_mark_disc(&s_q, slot);
    atomic_store_explicit(&s_lost, 0u, memory_order_relaxed);
    last_ms = now;
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
