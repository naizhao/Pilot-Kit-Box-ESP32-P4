/* edge_consume.h — edge_cap → modes_edge 之间的断点决策。
 *
 * 为什么单独成文件：这段判断原来内联在 adsb1090.c 的 core1 循环里，周围
 * 全是硬件调用，host 测不到——而它本身是个纯逻辑判断，且判错的代价是
 * 「解码器长时间在错误极性下空转」这种没有直接症状的慢性失效，正是最
 * 需要单测盯住的那类。
 *
 * ── 合同 ──────────────────────────────────────────────────────────
 * 边沿流出现**任何**不连续，都必须 modes_edge_reset：
 *   · disc      结构性断点（DMA 停机→重启，时间轴整段缺失）
 *   · overruns 增加  RXSTALL 单沿丢失（PIO push noblock 撞满 RX FIFO）
 *
 * 理由：丢沿数量不可知。丢**奇数**个就让后续边沿的上升/下降极性相对
 * modes_edge 的假设（偶下标=上升沿，见 modes_edge.c:58）整体反相。反相
 * 之后候选滑窗只扫偶数下标（modes_edge.c:86 `r += 2`），preamble 永远落
 * 在下降沿上、脉宽检查测成间隔，于是**从丢沿一直错到下一次 >5µs 长隔**
 * 才自然重锚——噪声密集时长隔极其稀少。
 *
 * ── 为什么推翻旧实现 ──────────────────────────────────────────────
 * 旧实现只在 disc 时 reset，依据是 edge_cap.c:177 的「单沿丢失是帧内损伤，
 * 该帧由解码端自然判负（安全丢弃）」。这个假设低估了损伤范围：错的不是
 * 一帧，是**从丢沿到下一次静默之间的全部帧**。
 *
 * test_modes_edge.c 用例 15 早就锁定了「断点必须 reset」这条性质，但生产
 * 代码的 RXSTALL 路径没有用上它；用例 6 只喂单帧，丢沿后 0 帧被当成正确
 * 行为，没有人验证过「后续的帧还能不能解出」——bug 就活在这个缝里。
 *
 * 实测（2026-09-13，V4.4 实板，室外）：板载 IFA 下 overruns 达 47 万/15s，
 * 全量程 9 个门限点收帧恒为 0；同板外置天线 overruns 低两个数量级，
 * 同样门限下能稳定收帧。
 *
 * ── 反向约束（别把 reset 加激进了）────────────────────────────────
 * 没有丢沿时**不得** reset。edge_cap 的发布粒度是块（256 条/块），一帧的
 * 边沿完全可能跨块；每次 drain 都 reset 会把所有跨块帧砍掉。这条由
 * test_edge_consume.c 的对照用例锁定。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool degrade;   /* 置 sticky 退化标志：丢失时长不可知，时间基永久偏小 */
    bool reset;     /* 丢弃半截 burst，让下一个 burst 重新锚定奇偶 */
} edge_consume_act_t;

static inline edge_consume_act_t
edge_consume_decide(bool disc, uint32_t overruns, uint32_t last_overruns)
{
    const bool lost = (overruns != last_overruns);
    edge_consume_act_t a;
    a.degrade = disc || lost;
    a.reset   = disc || lost;
    return a;
}
