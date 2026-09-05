/*
 * test_adsb_link_codec.c — P4↔RP2040 链路编解码器的 host 单测。
 *
 * 跑法（与 firmware/test/ 下其它测试同一套路）：
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/components/adsb_link_codec \
 *      -o /tmp/test_adsb_link_codec \
 *      firmware/test/test_adsb_link_codec.c \
 *      firmware/components/adsb_link_codec/adsb_link_codec.c \
 *   && /tmp/test_adsb_link_codec
 *
 * 判据来源：firmware/PROTOCOL_P4_RP2040_UART.md v1.0。
 * CRC 已知答案 + 独立参考实现（双实现互证）；恢复行为逐条对应规范 §3。
 */
#include "adsb_link.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("        at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

/* 独立参考实现：与被测代码分开写，避免同错互证。 */
static uint16_t ref_crc16(const uint8_t *d, size_t n)
{
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)((uint16_t)d[i] << 8);
        for (int b = 0; b < 8; b++)
            c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

static int g_msgs;
static adsb_link_msg_t g_last;
static void sink(void *user, const adsb_link_msg_t *m)
{
    (void)user; g_msgs++; g_last = *m;
}

static const uint8_t SAMPLE_FRAME[14] = {
    0x8D, 0x4C, 0xA1, 0xBD, 0x58, 0xBF, 0x34, 0x62,
    0x59, 0x2C, 0x69, 0x8A, 0xD5, 0x3B
};

static size_t build_modes_raw(uint8_t *out, uint8_t seq)
{
    uint8_t pl[6 + 14];
    pl[0] = ADSB_LINK_MODES_LONG;          /* 112-bit */
    pl[1] = 0xFF;                          /* rssi 无值 */
    pl[2] = 0x11; pl[3] = 0x22; pl[4] = 0x33; pl[5] = 0x44;  /* rp_ts_us LE */
    memcpy(pl + 6, SAMPLE_FRAME, 14);
    return adsb_link_encode(out, 512, ADSB_LINK_MSG_MODES_RAW, seq, pl, 20);
}

int main(void)
{
    /* 1. CRC 已知答案（规范 §1 的 0x29B1）——同时钉住两套实现。 */
    CHECK(ref_crc16((const uint8_t *)"123456789", 9) == 0x29B1, "ref crc16 KAT\n");
    CHECK(adsb_link_crc16((const uint8_t *)"123456789", 9) == 0x29B1,
          "codec crc16 KAT\n");

    /* 2. 基本往返：编码→整帧一次喂入→恰好 1 条消息、字段逐项一致。 */
    {
        uint8_t buf[512];
        size_t n = build_modes_raw(buf, 7);
        CHECK(n == 8 + 20 + 2, "frame len got=%zu\n", n);
        adsb_link_dec_t d; adsb_link_dec_init(&d, sink, NULL);
        adsb_link_dec_feed(&d, buf, n);
        CHECK(g_msgs == 1, "msgs=%d\n", g_msgs);
        CHECK(g_last.type == ADSB_LINK_MSG_MODES_RAW, "type=%02x\n", g_last.type);
        CHECK(g_last.seq == 7, "seq=%u\n", g_last.seq);
        CHECK(g_last.payload_len == 20, "plen=%u\n", g_last.payload_len);
        CHECK(g_last.payload[0] == ADSB_LINK_MODES_LONG, "flags\n");
        CHECK(memcmp(g_last.payload + 6, SAMPLE_FRAME, 14) == 0, "frame bytes\n");
        CHECK(d.msgs == 1 && d.crc_errors == 0 && d.resyncs == 0, "counters\n");
    }

    /* 3. 噪声前缀重同步：31 字节垃圾 + 合法帧 → 恰好 1 条、resyncs 计数 > 0。 */
    {
        g_msgs = 0;
        uint8_t noise[512];
        memset(noise, 0xA5, 31);
        size_t n = build_modes_raw(noise + 31, 1);
        adsb_link_dec_t d; adsb_link_dec_init(&d, sink, NULL);
        adsb_link_dec_feed(&d, noise, 31 + n);
        CHECK(g_msgs == 1, "msgs=%d\n", g_msgs);
        CHECK(d.resyncs > 0, "resyncs=%u\n", d.resyncs);
    }

    /* 4. 逐字节分片喂入（粘包/半包）。 */
    {
        g_msgs = 0;
        uint8_t buf[512];
        size_t n = build_modes_raw(buf, 2);
        adsb_link_dec_t d; adsb_link_dec_init(&d, sink, NULL);
        for (size_t i = 0; i < n; i++) adsb_link_dec_feed(&d, buf + i, 1);
        CHECK(g_msgs == 1, "msgs=%d\n", g_msgs);
    }

    /* 5. CRC 破坏：本帧作废（crc_errors==1、无消息），随后帧仍可解。 */
    {
        g_msgs = 0;
        uint8_t buf[1024];
        size_t n1 = build_modes_raw(buf, 3);
        buf[n1 - 1] ^= 0x01;
        size_t n2 = build_modes_raw(buf + n1, 4);
        adsb_link_dec_t d; adsb_link_dec_init(&d, sink, NULL);
        adsb_link_dec_feed(&d, buf, n1 + n2);
        CHECK(g_msgs == 1, "msgs=%d\n", g_msgs);
        CHECK(d.crc_errors == 1, "crc_errors=%u\n", d.crc_errors);
        CHECK(g_last.seq == 4, "recovered seq=%u\n", g_last.seq);
    }

    /* 6. 长度攻击：plen=0xFFFF 的伪头 + 随后合法帧。 */
    {
        g_msgs = 0;
        uint8_t buf[1024];
        size_t k = 0;
        buf[k++] = ADSB_LINK_MAGIC0; buf[k++] = ADSB_LINK_MAGIC1;
        buf[k++] = 1; buf[k++] = 0; buf[k++] = 0x10; buf[k++] = 0;
        buf[k++] = 0xFF; buf[k++] = 0xFF;             /* 非法 plen */
        for (int i = 0; i < 10; i++) buf[k++] = (uint8_t)i;
        size_t n2 = build_modes_raw(buf + k, 5);
        adsb_link_dec_t d; adsb_link_dec_init(&d, sink, NULL);
        adsb_link_dec_feed(&d, buf, k + n2);
        CHECK(g_msgs == 1, "msgs=%d\n", g_msgs);
        CHECK(d.len_errors == 1, "len_errors=%u\n", d.len_errors);
    }

    /* 7. 版本不匹配：ver_major=2 的伪帧作废且计数，随后合法帧可解。 */
    {
        g_msgs = 0;
        uint8_t buf[1024];
        size_t n1 = build_modes_raw(buf, 6);
        buf[2] = 2;                                    /* 篡改 major */
        /* 重新按参考实现补 CRC，保证它只死于版本而不是 CRC */
        uint16_t c = ref_crc16(buf, n1 - 2);
        buf[n1 - 2] = (uint8_t)c; buf[n1 - 1] = (uint8_t)(c >> 8);
        size_t n2 = build_modes_raw(buf + n1, 7);
        adsb_link_dec_t d; adsb_link_dec_init(&d, sink, NULL);
        adsb_link_dec_feed(&d, buf, n1 + n2);
        CHECK(g_msgs == 1, "msgs=%d\n", g_msgs);
        CHECK(d.version_mismatch == 1, "ver=%u\n", d.version_mismatch);
    }

    /* 8. 未知类型：CRC 合法则必须递交（规范 §3.5）。 */
    {
        g_msgs = 0;
        uint8_t buf[512];
        size_t n = adsb_link_encode(buf, 512, 0x42, 9,
                                    (const uint8_t *)"xy", 2);
        adsb_link_dec_t d; adsb_link_dec_init(&d, sink, NULL);
        adsb_link_dec_feed(&d, buf, n);
        CHECK(g_msgs == 1 && g_last.type == 0x42, "msgs=%d type=%02x\n",
              g_msgs, g_last.type);
    }

    /* 9. seq gap：seq 1→3 计 1 次，不丢帧。 */
    {
        g_msgs = 0;
        uint8_t buf[1024];
        size_t n1 = build_modes_raw(buf, 1);
        size_t n2 = build_modes_raw(buf + n1, 3);
        adsb_link_dec_t d; adsb_link_dec_init(&d, sink, NULL);
        adsb_link_dec_feed(&d, buf, n1 + n2);
        CHECK(g_msgs == 2 && d.seq_gaps == 1, "msgs=%d gaps=%u\n",
              g_msgs, d.seq_gaps);
    }

    /* 10. 超限输入防御：payload 超 464 编码返回 0；cap 不足返回 0。 */
    {
        uint8_t big[ADSB_LINK_MAX_PAYLOAD + 1] = {0};
        uint8_t out[600];
        CHECK(adsb_link_encode(out, sizeof(out), 0x10, 0, big,
                               ADSB_LINK_MAX_PAYLOAD + 1) == 0, "oversize\n");
        CHECK(adsb_link_encode(out, 8, 0x10, 0,
                               (const uint8_t *)"a", 1) == 0, "small cap\n");
    }

    /* 11. 回归（审查发现）：CRC 坏帧被拒后滑窗使 fill>8，此时到达的下一帧头
       仍必须做 ver/plen 校验（规范 §3.2/§3.3）。紧跟着的 ver_major=2 且
       CRC 正确的帧不得递交，只能作废并计数；随后正常帧仍可解。 */
    {
        g_msgs = 0;
        uint8_t buf[1024];
        size_t n1 = build_modes_raw(buf, 10);
        buf[n1 - 1] ^= 0x01;                           /* 坏 CRC：本帧作废 */
        uint8_t *f2 = buf + n1;
        size_t n2 = build_modes_raw(f2, 11);
        f2[2] = 2;                                     /* 篡改 major */
        /* 按参考实现重补 CRC：保证它只死于版本检查而不是 CRC */
        uint16_t c = ref_crc16(f2, n2 - 2);
        f2[n2 - 2] = (uint8_t)c; f2[n2 - 1] = (uint8_t)(c >> 8);
        adsb_link_dec_t d; adsb_link_dec_init(&d, sink, NULL);
        adsb_link_dec_feed(&d, buf, n1 + n2);
        CHECK(g_msgs == 0, "ver2 delivered after reject msgs=%d\n", g_msgs);
        CHECK(d.version_mismatch == 1, "ver=%u\n", d.version_mismatch);
        CHECK(d.crc_errors == 1, "crc_errors=%u\n", d.crc_errors);
        g_msgs = 0;
        size_t n3 = build_modes_raw(buf, 12);
        adsb_link_dec_feed(&d, buf, n3);
        CHECK(g_msgs == 1 && g_last.seq == 12, "recovered msgs=%d seq=%u\n",
              g_msgs, g_last.seq);
    }

    printf(g_fail ? "FAIL (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
