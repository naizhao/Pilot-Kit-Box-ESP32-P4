/*
 * test_selftest_gen.c — 自检位流构造 + modes_edge 回环 + DF17 常量 CRC 校验。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/rp2040 -I firmware/main \
 *      -DSELFTEST_GEN_HOST_TEST -DRF_SAFETY_HOST_TEST \
 *      -o /tmp/test_selftest_gen \
 *      firmware/test/test_selftest_gen.c \
 *      firmware/rp2040/selftest_gen.c \
 *      firmware/rp2040/modes_edge.c -lm \
 *   && /tmp/test_selftest_gen
 *
 * 回环的时基：位流按 16 slot/µs 构造（PIO 16Mbps），模拟沿提取出的
 * deltas 即以 slot 为 tick、tick_hz = 16MHz；1 qus = 4 slot 整除，
 * ticks_to_qus 往返无舍入误差（对照 test_modes_edge.c 的 qus 合同）。
 * mode_s.c 直接 include 进本 TU（仓库惯例），给常量回填正确 parity。
 *
 * 硬件适用范围（audit round 5 勘误）：位流构造与回环判据在 host、V4/V3
 * 载板与台架裸 RP2040 板均有效。跳线 24→19 前**必须先拆下 R57**（33Ω，
 * PCB 上 PULSES_RAW→PULSES 的串阻，expansion-board-v4.kicad_pcb:7100 起、
 * 焊盘 net 见 :7216/:7224）：TLV3501 推挽输出经 33Ω 对 1k 串阻分支约
 * 30:1 占优，不拆 R57 则节点电平跟随比较器、旧"串 ≥1k 电平胜出"的
 * 说法物理不成立。拆下 R57 后 GPIO24 → R57 的 PULSES 侧焊盘（或 PULSES
 * 网络任一可达焊盘，V4 亦有 TP7 @ :38901）直连即可，无输出争用。V3
 * 载板 TP7 同样有落点，同理先隔离比较器输出。详见 selftest_gen.h；
 * PINMAP.md/ASSEMBLY 文档里"V4 已删 TP7"的旧说法与 PCB 不符，归硬件
 * 侧勘误。
 */
#include "selftest_gen.h"
#include "modes_edge.h"
#include "mode_s.c"          /* mode_s_checksum：给常量回填正确 parity */

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("        at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

extern uint8_t SELFTEST_DF17[14];   /* 定义在 selftest_gen.c */

static int g_frames;
static modes_edge_frame_t g_last;
static void cb(const modes_edge_frame_t *f, void *user)
{ (void)user; g_frames++; g_last = *f; }

int main(void)
{
    /* 1. 常量 parity 校验：非法则打印正确值，回填 selftest_gen.c 后转绿。 */
    {
        uint32_t c = mode_s_checksum(SELFTEST_DF17, 112);
        if (!(((uint32_t)SELFTEST_DF17[11] << 16) |
              ((uint32_t)SELFTEST_DF17[12] << 8) | SELFTEST_DF17[13]) ||
             SELFTEST_DF17[11] != (uint8_t)(c >> 16) ||
             SELFTEST_DF17[12] != (uint8_t)(c >> 8) ||
             SELFTEST_DF17[13] != (uint8_t)c) {
            printf("  [FIX] SELFTEST_DF17 parity 应为 %02X %02X %02X —— "
                   "回填 selftest_gen.c 后重跑\n",
                   (unsigned)(c >> 16) & 0xFF, (unsigned)(c >> 8) & 0xFF,
                   (unsigned)c & 0xFF);
            g_fail++;
        }
    }

    /* 2. 全回路：bitstream → 模拟沿提取 → modes_edge → 帧逐字节一致。 */
    {
        static uint32_t words[SELFTEST_MAX_WORDS];
        size_t nw = selftest_build_bitstream(SELFTEST_DF17, 112,
                                             16u * 50, 16u * 500,
                                             words, SELFTEST_MAX_WORDS);
        CHECK(nw > 0 && nw <= SELFTEST_MAX_WORDS, "nw=%zu\n", nw);

        /* 位流 → 全边沿间隔（slot 单位 = tick_hz 16MHz）；上升/下降都算沿，
         * 与 edgecap PIO 的双沿捕获一致（R11）。 */
        uint32_t deltas[512]; size_t nd = 0;
        int prev = 0; int64_t last_t = -1;
        for (size_t w = 0; w < nw && nd < 512; w++)
            for (int b = 0; b < 32; b++) {
                int bit = (words[w] >> b) & 1;
                if (bit != prev) {
                    if (last_t >= 0)
                        deltas[nd++] = (uint32_t)((int64_t)(w * 32 + b) - last_t);
                    last_t = w * 32 + b;
                    prev = bit;
                }
            }

        modes_edge_t m; modes_edge_init(&m, SELFTEST_BITS_PER_US * 1000000u,
                                        cb, NULL);
        g_frames = 0;
        /* 不追加手工终止长隔：位流末尾的收尾孤立脉冲（P1-5）与最后数据
         * 脉冲间隔 >5µs，其上升沿自身关闭帧 burst；收尾脉冲自身的
         * 上升+下降构成仅 2 边沿的新 burst，被 preamble 门径直丢弃。 */
        modes_edge_feed(&m, deltas, nd);
        CHECK(g_frames == 1, "frames=%d\n", g_frames);
        CHECK(g_last.nbits == 112, "nbits=%u\n", g_last.nbits);
        CHECK(memcmp(g_last.frame, SELFTEST_DF17, 14) == 0, "roundtrip bytes\n");
    }

    printf(g_fail ? "FAIL (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
