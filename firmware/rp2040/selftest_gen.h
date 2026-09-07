/* selftest_gen.h — 台架自检脉冲。跳线 GPIO24→GPIO19 的正确步骤（audit
 * round 5 勘误，推翻"串 ≥1k 电阻 GPIO 电平胜出"的旧说法——物理不成立）：
 * GPIO19 上的 TLV3501 是推挽输出，经 R57（33Ω，PCB 上 PULSES_RAW→PULSES
 * 的串阻）驱动 PULSES 网络；33Ω 对 1k 分支的驱动能力约 30:1，只要 R57
 * 还在，节点电平跟随比较器，GPIO 分支既打不动电平又与比较器对驱。
 * **先拆下 R57（隔离 TLV3501 推挽输出），再跳线 GPIO24 → R57 的 PULSES
 * 侧焊盘（或 PULSES 网络任一可达焊盘），直连即可**——比较器已被隔离，
 * 无输出争用。GPIO18 是 SUBG_RESET（CC1312R），严禁用作跳线。
 *
 * 适用板型（PCB 实据：expansion-board-v4.kicad_pcb——R57 footprint
 * Reference=R57 @ :7100、Value=33R，焊盘 net PULSES_RAW @ :7216 /
 * PULSES @ :7224；TP7/RECOVERED_CLK @ :38901）：
 *   · V4 载板：R57 与 TP7 都在生产 PCB 上**存在**——按上述 ECO 拆下
 *     R57 后跳线回环**可执行**；
 *   · V3 载板：GPIO24 在 TP7 有落点，同样先隔离 TLV3501 推挽输出
 *     （拆除其到 PULSES 网络的串阻）再跳线；
 *   · 台架裸 RP2040 板：GPIO24/GPIO19 直连，可执行。
 * 遗留不一致（硬件侧文档，固件仓库不改、已在审计报告登记待修）：
 * hardware/expansion-board-v4/PINMAP.md:41 与 ASSEMBLY-zh_CN.md:303 仍写
 * "V4 已删 TP7 / 无落点"，与 PCB 实际不符。 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "edge_cap.h"

#define SELFTEST_BITS_PER_US 16u
/* Flush 脉冲串的边沿预算（块发布粒度的台架配套）：edge_cap 的发布粒度
 * = 块（256 条/块，IRQ 每凑满一块发布），台架单发位流的突发（~240 沿）
 * 只占当前残块、自己永远凑不齐一块 → 不可见。位流在收尾脉冲之后追加
 * 一串 1µs 周期 / 0.5µs 宽的 flush 脉冲：填满当前残块（≤255 沿）再顶出
 * 一个整块（256 沿）→ 2×256 = 512 沿即够，+64 沿裕量 = 576。帧位于
 * 残块内随该块发布；脉冲串自身作为噪声突发被解码端丢弃（容量溢出路径，
 * test_modes_edge 用例 12 覆盖）。发布粒度变化时经 EDGE_CAP_Q_BLOCK_ITEMS
 * 自动跟随。 */
#define SELFTEST_FLUSH_EDGES (2u * EDGE_CAP_Q_BLOCK_ITEMS + 64u)
/* 768 word = 24576 slot ≈ 1.5ms：112-bit 最长位流（收尾脉冲止于
 * 2016 slot）+ flush 脉冲串（576 边沿 = 288 脉冲 × 16 slot = 9216 slot，
 * 起点在收尾脉冲后 6µs → 止于 6712 slot）+ 台架 idle（100µs 前 +
 * 1000µs 后）。
 * 最坏占用：idle_before 1600 slot + 位流（收尾脉冲止于 2016 slot）+
 * flush 2112→flush_end 6712 slot + idle_after 16000 slot = 24312 slot
 * = 760 word（24312/32 = 759.75 向上取整）→ 取 768 留少量裕量。
 * 旧值 1216 对应整环发布粒度（2048+64 沿 flush），块发布后收缩。 */
#define SELFTEST_MAX_WORDS   768u

size_t selftest_build_bitstream(const uint8_t *frame, int msgbits,
                                uint32_t idle_bits_before,
                                uint32_t idle_bits_after,
                                uint32_t *out, size_t cap_words);

bool selftest_run(void);              /* 上板；host 编译时被宏隔离 */
