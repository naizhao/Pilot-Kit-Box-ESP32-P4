/* edge_cap.h — PIO 双沿捕获 + DMA 块队列取数（block-queue 重设计）。 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "edge_cap_queue.h"

#define EDGE_CAP_SM_CLK_HZ 125000000u     /* RP2040 默认 sys clk，div=1 */
#define EDGE_CAP_TICK_HZ   (EDGE_CAP_SM_CLK_HZ / 2u)   /* 2 周期/迭代 */
/* 块缓冲总几何（u32 条目数）= 8 块 × 256 = 2048 条（8KB，含 1 个保留槽）。
 * **不是可用容量**：可同时 FULL 的只有 N−1=7 块 = 1792 条
 * （EDGE_CAP_Q_CAPACITY × EDGE_CAP_Q_BLOCK_ITEMS，满环判定恒保留 1 槽）。
 * 发布按块进行（256 条/块，IRQ 每块发布），旧"整环发布"语义不复存在；
 * 本宏仅描述 DMA 缓冲区总面积，勿再作容量/预算换算用。 */
#define EDGE_CAP_RING_ITEMS (EDGE_CAP_Q_N_BLOCKS * EDGE_CAP_Q_BLOCK_ITEMS)

/* PIO 推送的是递减计数器原值；换算成真实间隔 tick：
 * 设 D = PRELOAD − raw（两次沿捕获间的递减次数），对程序逐拍计数得
 * cycles = 2D+7（edge 块 4 + 高相位 2H + 变低转换 2 + 低相位 2L + 被命中的
 * 那拍 jmp pin edge 1），即真实间隔 = D+3.5 tick。tick 只能取整：+3 是该值
 * 的 floor，每个间隔带 ≤1 tick（16ns）的同号偏差；该偏差逐间隔累积、不会
 * 跨帧抵消，必须逐沿修正——这就是 +3 在每个沿上各自应用的原因。
 * 饱和（raw≈0，X 停在 0）得到 ≈PRELOAD+3 的超大值 = 空闲标记，与 >5µs gap 语义一致。 */
#define EDGE_CAP_PRELOAD 0xFFFFFFE0u
static inline uint32_t edgecap_raw_to_ticks(uint32_t raw)
{
    return (EDGE_CAP_PRELOAD - raw) + 3u;
}

/* tick→µs（PROTOCOL §2 勘误 2026-09-05 的回绕合同）：tick = 16ns 精确，
 * µs = tick×16/1000 = tick×2/125。先乘后除，×2 中间值在任何物理可达的运行
 * 时长内都装得下 u64：即便按旧式溢出边界 ~1.845e13 tick（≈82h）计，×2 后
 * 也仅 ≈2^45.1；×2 溢出需 tick ≥ 2^63 ≈ 4676 年，物理不可达。旧式
 * tick×1000000ull 在 ~1.845e13 tick（≈82h uptime）即溢出 u64，勿改回。
 * 整数除法 floor：单值截断误差 <1µs；返回值按 u32 截断 = 模 2^32 单调
 * （约 71.6 min 回绕，消费方用 (u32)(now−prev) 无符号差值解释时间差）；
 * 0 是合法回绕值，不是"无值"哨兵。 */
_Static_assert(EDGE_CAP_TICK_HZ == 62500000u,
               "2/125 us ratio pinned to the 62.5MHz tick");
static inline uint32_t edgecap_tick_to_us(uint64_t tick)
{
    return (uint32_t)(tick * 2u / 125u);
}

/* 块队列发布合同（gpt-5.6-sol 两轮审计收敛的最终模型）：
 *
 *   · 单 DMA 通道，N=8 块 × 256 条；IRQ 驱动重武装，**显式所有权**：
 *     每个块任一时刻只属于一方——FREE →（DMA 在飞写）→ FULL（IRQ 发布）
 *     →（消费者取空）→ FREE。块绝不同时被 DMA 写和被消费者读。旧共享环
 *     的"整环发布让下一圈盖掉未读数据"（审计 C1）与随之而来的
 *     claim/window_ok 写穿重检算术一并废除：所有权由构造排除写穿，
 *     重检不再存在，也勿加回。
 *
 *   · 生产者 = DMA IRQ（core0）：完成 → push_full 发布（release）→
 *     guard 放行则立即重武装下一 FREE 块（write_addr 重写 +
 *     TRANS_COUNT_TRIG 触发）；环满（容量 N−1，保留槽贴着消费游标）则
 *     **DMA 停机**：置 lost 标志、记 overrun。停机到消费侧重启之间的
 *     边沿数据丢失（overrun 语义，真实过载路径，诚实计数）。
 *
 *   · 消费者 = edge_cap_drain（core1 独占）：**每次调用至多取走一整块**
 *     （round-2 Fix 4）——弹出 → 逐条换算 → free（release 交还）；队列
 *     空或 cap 不足一块（256 条）时返回 0。理由：一次跨断点的多块批次
 *     无法表达"断点在哪"，消费者要在块间 reset 解码器——逐块交接让断
 *     点位置精确落在块边界。
 *     停机重启：drain 在释放过 ≥1 块后检查 lost 标志，用 arm_slot 的
 *     guard 重新武装（guard 失败 = 环仍满，保持停机等下一拍）。重启
 *     前对 PIO 做**完整确定性重初始化**（停用 → pio_sm_restart → 清
 *     FIFO → pio_sm_init 重装配置 + PC 回程序起点 → 重新使能；单用
 *     restart 不复位 PC/X，见 edge_cap.c edge_cap_pio_flush），丢弃停机
 *     窗口的残缺流，并把重武装的第一块 mark disc
 *     （edgecap_q_t::disc_bitmap）——消费侧见位先 modes_edge_reset 再喂，
 *     断点不拼接（时间基保留，断点后时间戳单调、见 modes_edge.h）。
 *     重启 guard 必须走 arm_slot——它复用满环判定，防止把保留槽填满
 *     发布出 fill_done == consume 的 8 块 FULL 态（host 测试 11 的
 *     canary 反例）。
 *
 *   · 跨核配对：fill_idx/fill_done 唯一写者 = IRQ（core0），consume_idx
 *     唯一写者 = drain（core1），发布/回收全走 release/acquire（同
 *     adsb1090 帧环的 SPSC 口径）。DMA 的 SRAM 写对两核一致可见，IRQ
 *     在传输完成后才触发，块数据无需额外屏障。所有权逐条论证见
 *     edge_cap_queue.h 头注释。
 *
 *   · 块边界重武装窗口的时序预算（2026-09-05 审计，DMA 时序缓解）：
 *     PIO RX FIFO 已开 join（edgecap_program_init）达 **8 深**；最短边沿
 *     间隔 0.5µs（R11 双沿，125MHz SM/2 tick）下缓冲预算 = 8 × 0.5µs =
 *     **4µs**，用于覆盖「块完成 IRQ → 重武装下一块」的窗口。IRQ handler
 *     已常驻 SRAM（__not_in_flash_func，防 XIP cache miss 加延迟），处理
 *     本体 ~1µs 量级；剩余预算要吸收高优先级中断（USB 等）对 core0 的
 *     抢占——DMA IRQ 不得被遮蔽超过 FIFO 预算。**审计验收条款：持续边沿
 *     下 RXSTALL/overrun == 0**，必须由 Task 15 台架在真实持续边沿流下
 *     实测验证（板未回，正式整定与验证延后到该任务）；超预算突发仍由
 *     停机-重启 + disc 语义兜底（如实丢沿，不静默）。
 */

void   edge_cap_start(void);
/* 取数（core1 独占）：**每次调用至多弹出一整块**——成功返回
 * EDGE_CAP_Q_BLOCK_ITEMS（256），队列空或 cap 不足一块返回 0；断点只能
 * 表达在块边界（跨断点的批次无法标记断点位置，消费方逐块 reset）。
 * discontinuity 可为 NULL；非 NULL 时接收**本块**的断点标记：返回的块
 * 是 lost 停机重武装后的第一块时置 true——该块数据之前有一段整段缺失
 * 的真实时间，消费侧必须先 modes_edge_reset（丢弃半截 burst；abs_tick
 * 时间基保留、断点后时间戳单调）再喂本块，否则断点前后 delta 拼成假
 * burst。RXSTALL 单沿丢失不置位（帧内损伤，该帧自然判负；见
 * edge_cap.c）。 */
size_t edge_cap_drain(uint32_t *out, size_t cap, bool *discontinuity);
/* DMA 重武装/恢复入口（**core0 独占，主循环每次调用**）：处理满环停机
 * （lost）与静默停摆（≥5 ms 无完成中断）。core1 侧调用 rearm 实测不生效
 * （2026-09-10），故重武装统一归 core0。 */
void edge_cap_service(void);
uint32_t edge_cap_overruns(void);
/* 诊断：等待消费的**FULL 块数**（0..7，不折算条目数；含消费者正在
 * 转换中的那块）。按块计——满块发布粒度下它以 256 条/块为台阶跳动。 */
uint32_t edge_cap_pending(void);
