/*
 * selftest_gen.c — bitstream 构造（纯函数）+ 上板单次回放。
 *
 * 波形定义与 modes_edge 判据严格互逆（外部锚点 mode-s.c:708-715）：
 * preamble 上升沿 0/1.0/3.5/4.5µs，数据 8.0+1.0k µs 处、bit1 在 +0µs /
 * bit0 在 +0.5µs。**脉冲宽 0.5µs（真实信号宽度）**：R11 双沿解码后融合
 * 波形（bit0→bit1 连续高电平）可被正确重建，自检必须用真实宽度才能作为
 * 验收依据——窄脉冲特例已随单沿架构一并废除。
 * SELFTEST_DF17 的 parity 初始为 0（故意非法）——由 host 测试
 * test_selftest_gen.c 用 mode_s_checksum 算出正确值回填后才算过；
 * 这保证上板回放的帧能穿过 P4 的 CRC 门（RP 自身不裁决 CRC）。
 */
#include <string.h>
#include "selftest_gen.h"
#include "board_pins.h"

#ifndef SELFTEST_GEN_HOST_TEST
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "selftest_gen.pio.h"
#endif

uint8_t SELFTEST_DF17[14] = {          /* parity 三字节已由 host 测试回填 */
    0x8D, 0x4C, 0xA1, 0xBD, 0x58, 0xBF, 0x34, 0x62,
    0x59, 0x2C, 0x69, 0x61, 0x6A, 0xBD
};

static void set_bit(uint32_t *w, uint32_t slot)
{
    w[slot / 32] |= (1u << (slot % 32));
}

size_t selftest_build_bitstream(const uint8_t *frame, int msgbits,
                                uint32_t idle_before, uint32_t idle_after,
                                uint32_t *out, size_t cap_words)
{
    /* 绝对上升时刻（slot 单位，1µs = 16 slot）*/
    uint32_t rise[128]; size_t nr = 0;
    rise[nr++] = 0;
    rise[nr++] = 1  * SELFTEST_BITS_PER_US;
    rise[nr++] = (uint32_t)(3.5 * SELFTEST_BITS_PER_US);
    rise[nr++] = (uint32_t)(4.5 * SELFTEST_BITS_PER_US);
    for (int k = 0; k < msgbits; k++) {
        int bit = (frame[k / 8] >> (7 - (k % 8))) & 1;
        uint32_t base = (uint32_t)((8.0 + 1.0 * k + (bit ? 0.0 : 0.5))
                                   * SELFTEST_BITS_PER_US);
        rise[nr++] = base;
    }
    /* P1-5：帧尾收尾孤立脉冲 —— 与最后数据脉冲上升沿间隔 6µs（96 slot）
     * > 5µs burst 阈值。静默台架上单次回放再无其它长隔，靠这个沿关闭
     * 帧 burst，否则帧永远不出。 */
    rise[nr] = rise[nr - 1] + 6 * SELFTEST_BITS_PER_US;
    nr++;
    uint32_t last_end = rise[nr - 1] + SELFTEST_BITS_PER_US / 2;

    /* Flush 脉冲串（块发布粒度的台架配套）：edge_cap 发布粒度 = 块
     * （256 条/块，IRQ 每块发布），台架单发突发（~240 沿）只占当前残块、
     * 自己永远凑不齐一块、不可见。收尾脉冲之后再隔 6µs（不并入帧
     * burst，先关掉收尾脉冲自己的 2 沿小 burst）追加 1µs 周期 / 0.5µs
     * 宽的脉冲串：填满残块 + 顶出一个整块（SELFTEST_FLUSH_EDGES）→
     * 帧随残块发布；串本身作为噪声洪流走 modes_edge 容量溢出路径被
     * 丢弃（不产帧，见 test_modes_edge 用例 12）。 */
    uint32_t flush_start = last_end + 6 * SELFTEST_BITS_PER_US;
    uint32_t pulses = (SELFTEST_FLUSH_EDGES + 1) / 2;   /* 每脉冲 = 升+降 2 沿 */
    uint32_t flush_end = flush_start + (pulses - 1) * SELFTEST_BITS_PER_US
                       + SELFTEST_BITS_PER_US / 2;
    uint32_t total = idle_before + flush_end + idle_after;

    size_t words = (total + 31) / 32;
    if (words > cap_words) return 0;
    memset(out, 0, words * sizeof(uint32_t));
    /* 每脉冲高电平 0.5µs = 8 slot：真实信号宽度。bit0→bit1 相邻时两个脉冲
     * 首尾相接（连续高 1µs）——这是有意为之的验收形态，双沿解码按宽度
     * 重建融合串（R11）。 */
    for (size_t i = 0; i < nr; i++)
        for (uint32_t s = 0; s < SELFTEST_BITS_PER_US / 2; s++)
            set_bit(out, idle_before + rise[i] + s);
    for (uint32_t p = 0; p < pulses; p++)
        for (uint32_t s = 0; s < SELFTEST_BITS_PER_US / 2; s++)
            set_bit(out, idle_before + flush_start +
                         p * SELFTEST_BITS_PER_US + s);
    return words;
}

#ifndef SELFTEST_GEN_HOST_TEST
bool selftest_run(void)
{
    static uint32_t words[SELFTEST_MAX_WORDS];
    size_t nw = selftest_build_bitstream(SELFTEST_DF17, 112,
                                         16u * 100, 16u * 1000,
                                         words, SELFTEST_MAX_WORDS);
    if (nw == 0) return false;

    static PIO p = pio1;
    static uint sm = 0, dma_ch;
    static bool inited;
    if (!inited) {
        uint off = pio_add_program(p, &pulsegen_program);
        pulsegen_program_init(p, sm, off, PIN_SELFTEST_OUT);
        dma_ch = dma_claim_unused_channel(true);
        inited = true;
    }
    dma_channel_config c = dma_channel_get_default_config(dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(p, sm, true));
    dma_channel_configure(dma_ch, &c, &p->txf[sm], words, nw, false);

    pio_sm_set_enabled(p, sm, false);
    pio_sm_clear_fifos(p, sm);
    pio_sm_restart(p, sm);
    pio_sm_set_enabled(p, sm, true);
    dma_channel_start(dma_ch);

    absolute_time_t deadline = make_timeout_time_ms(100);
    while (dma_channel_is_busy(dma_ch))
        if (absolute_time_diff_us(get_absolute_time(), deadline) < 0) return false;
    while (!pio_sm_is_tx_fifo_empty(p, sm))
        tight_loop_contents();
    pio_sm_set_enabled(p, sm, false);
    return true;
}
#endif
