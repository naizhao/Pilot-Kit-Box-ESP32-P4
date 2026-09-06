/* selftest_gen.h — 台架自检脉冲。台架跳线 GPIO24→GPIO19，串 ≥1k 电阻
 * （TLV3501 在 19 上是推挽输出，直连会输出争用；串阻后 GPIO 电平胜出且
 * 电流 ~3.3mA 安全），整链回环。GPIO18 是 SUBG_RESET（CC1312R），严禁用作跳线。
 *
 * 适用板型（audit round 3 裁定：代码保留，接受范围写清）：
 *   · V3 载板：GPIO24 在 TP7 有落点，可执行跳线回环；
 *   · 台架裸 RP2040 板：直接跳线，可执行。
 *   · V4 载板**无法执行跳线回环**——TP7 已删、GPIO24 无落点。V4 的整链
 *     验收改用真实 RF 或信号源注入 J6；P4 侧可用后续 debug 命令注入合成
 *     帧验证 modes_ingest 链（follow-up，本文件不实现）。 */
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
