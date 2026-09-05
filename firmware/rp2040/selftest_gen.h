/* selftest_gen.h — 台架自检脉冲（跳线 GPIO18→19 后整链回环）。 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SELFTEST_BITS_PER_US 16u
/* 704 word = 22528 slot ≈ 1.4ms：232µs 最长帧 + 台架 idle（100µs 前 + 1000µs 后）。
 * brief 原 192（384µs）容不下自身调用点的 idle_after，见 task-11-report.md。 */
#define SELFTEST_MAX_WORDS   704u

size_t selftest_build_bitstream(const uint8_t *frame, int msgbits,
                                uint32_t idle_bits_before,
                                uint32_t idle_bits_after,
                                uint32_t *out, size_t cap_words);

bool selftest_run(void);              /* 上板；host 编译时被宏隔离 */
