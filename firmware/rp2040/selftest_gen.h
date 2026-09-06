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
/* Flush 脉冲串的边沿预算（gpt-5.6-sol Fix 1 的台架配套）：edge_cap 的
 * 发布粒度 = 整环 EDGE_CAP_RING_ITEMS 条边沿，台架单发位流的突发
 * （~240 沿）填不满一环 → 尾批永远不可见。位流在收尾脉冲之后追加
 * 一串 1µs 周期 / 0.5µs 宽的 flush 脉冲，把生产者推过一整环（+64 边沿
 * 裕量），帧本身位于环首随之发布；脉冲串自身作为噪声突发被解码端
 * 丢弃（容量溢出路径，test_modes_edge 用例 12 覆盖）。改环深度时此值
 * 经 edge_cap.h 自动跟随。 */
#define SELFTEST_FLUSH_EDGES (EDGE_CAP_RING_ITEMS + 64u)
/* 1216 word = 38912 slot ≈ 2.4ms：232µs 最长帧 + 收尾脉冲 + flush 脉冲串
 * （2112 边沿 = 1056 脉冲 × 16 slot = 16896 slot，起点在收尾脉冲后 6µs）
 * + 台架 idle（100µs 前 + 1000µs 后）。
 * 最坏占用：idle_before 1600 slot + 位流（收尾脉冲止于 2016 slot）+
 * flush 2112→flush_end 19000 slot + idle_after 16000 slot = 36600 slot
 * = 1144 word（36600/32 = 1143.75 向上取整）≤ 1216 ✓。
 * brief 原 704 容不下 flush 脉冲串（gpt-5.6-sol Fix 1 起位流加长）。 */
#define SELFTEST_MAX_WORDS   1216u

size_t selftest_build_bitstream(const uint8_t *frame, int msgbits,
                                uint32_t idle_bits_before,
                                uint32_t idle_bits_after,
                                uint32_t *out, size_t cap_words);

bool selftest_run(void);              /* 上板；host 编译时被宏隔离 */
