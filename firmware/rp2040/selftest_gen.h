/* selftest_gen.h — 台架自检脉冲。跳线 GPIO24→GPIO19，串 ≥1k 电阻
 * （TLV3501 在 19 上是推挽输出，直连会输出争用；串阻后 GPIO 电平胜出且
 * 电流 ~3.3mA 安全），整链回环。GPIO18 是 SUBG_RESET（CC1312R），严禁用作跳线。
 *
 * 适用板型（audit round 4 勘误，推翻 adb83f0/audit round 3 的"V4 无落点"
 * 结论——PCB 实据：expansion-board-v4.kicad_pcb:38901 起的 footprint
 * Reference=TP7、net=RECOVERED_CLK）：
 *   · V4 载板：TP7/RECOVERED_CLK 在生产 PCB 上**存在**——跳线回环验收
 *     在 V4 **可执行**：GPIO24 经 TP7 落点引出，跳线 24→19 串 ≥1k
 *     （GPIO19 是 TLV3501 推挽输出）；
 *   · V3 载板：GPIO24 在 TP7 有落点，同样可执行；
 *   · 台架裸 RP2040 板：直接跳线，可执行。
 * 遗留不一致（硬件侧文档，固件仓库不改、已在审计报告登记待修）：
 * hardware/expansion-board-v4/PINMAP.md:41 与 ASSEMBLY-zh_CN.md:303 仍写
 * "V4 已删 TP7 / 无落点"，与 PCB 实际不符。 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SELFTEST_BITS_PER_US 16u
/* 704 word = 22528 slot ≈ 1.4ms：232µs 最长帧 + 台架 idle（100µs 前 + 1000µs 后）。
 * brief 原 192（384µs）容不下自身调用点的 idle_after，见 task-11-report.md。
 * 1µs/比特重定时（P0-2）+ P1-5 收尾脉冲后的最坏占用：idle_before 1600 slot
 * + 位流（preamble 末沿 72 slot，数据末沿 (8+111+0.5)×16 = 1912 slot，
 * 收尾脉冲止于 1912+96+4 = 2012 slot）+ idle_after 16000 slot = 19612 slot
 * = 613 word（19612/32 = 612.875 向上取整）≤ 704 ✓ */
#define SELFTEST_MAX_WORDS   704u

size_t selftest_build_bitstream(const uint8_t *frame, int msgbits,
                                uint32_t idle_bits_before,
                                uint32_t idle_bits_after,
                                uint32_t *out, size_t cap_words);

bool selftest_run(void);              /* 上板；host 编译时被宏隔离 */
