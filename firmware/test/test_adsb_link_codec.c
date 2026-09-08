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
 * 判据来源：firmware/PROTOCOL_P4_RP2040_UART.md（v1.0 + v1.1 §6：
 * UAT_UPLINK/payload 上限 576；case 15-21 为 v1.1 改写/新增）。
 * CRC 已知答案 + 独立参考实现（双实现互证）；恢复行为逐条对应规范 §3。
 */
#include "adsb_link.h"

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
static uint8_t g_seq_hist[8];
static int g_seq_hist_n;
static void sink(void *user, const adsb_link_msg_t *m)
{
    (void)user;
    if (g_seq_hist_n < (int)sizeof(g_seq_hist))
        g_seq_hist[g_seq_hist_n++] = m->seq;
    g_msgs++; g_last = *m;
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

    /* 10. 超限输入防御：payload 超 ADSB_LINK_MAX_PAYLOAD（v1.1=576）编码
        返回 0；cap 不足返回 0。 */
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

    /* 12. 校验顺序（audit P2，2026-09-05 勘误）：CRC 先于 version。
        ver_major=2 且 CRC **坏**的帧必须计 crc_errors、**不得**计入
        version_mismatch——否则噪声可把 P4 的 PROTO_MISMATCH 锁进误判。 */
    {
        g_msgs = 0;
        uint8_t buf[1024];
        size_t n1 = build_modes_raw(buf, 20);
        buf[2] = 2;                                    /* 篡改 major */
        buf[n1 - 1] ^= 0x01;                           /* 再破坏 CRC */
        size_t n2 = build_modes_raw(buf + n1, 21);     /* 随后正常帧仍可解 */
        adsb_link_dec_t d; adsb_link_dec_init(&d, sink, NULL);
        adsb_link_dec_feed(&d, buf, n1 + n2);
        CHECK(g_msgs == 1 && g_last.seq == 21, "msgs=%d seq=%u\n",
              g_msgs, g_last.seq);
        CHECK(d.crc_errors == 1, "crc_errors=%u\n", d.crc_errors);
        CHECK(d.version_mismatch == 0, "ver=%u\n", d.version_mismatch);
    }

    /* 13. 回归钉住（audit P2 重排判据时机）：坏帧 + 好帧在同一次 feed 里，
        好帧最后一字节即本次 feed 最后一字节——坏帧被拒（滑窗）时好帧已
        完整在栈，必须不依赖新输入就地解出。 */
    {
        g_msgs = 0;
        uint8_t buf[1024];
        size_t n1 = build_modes_raw(buf, 30);
        buf[n1 - 1] ^= 0x01;                           /* 坏 CRC：本帧作废 */
        size_t n2 = build_modes_raw(buf + n1, 31);
        adsb_link_dec_t d; adsb_link_dec_init(&d, sink, NULL);
        adsb_link_dec_feed(&d, buf, n1 + n2);          /* 单次 feed */
        CHECK(g_msgs == 1 && g_last.seq == 31, "msgs=%d seq=%u\n",
              g_msgs, g_last.seq);
        CHECK(d.crc_errors == 1, "crc_errors=%u\n", d.crc_errors);
    }

    /* 14. 回归钉住（rescan 重构判别用例）：**长**坏帧（plen=70，共 80 字节）
        被拒后在缓冲前部留下 79 字节垃圾，其后紧跟完整缓冲的短好帧（30 字节），
        三者在同一次 feed 里。旧逐字节实现每收到 1 个输入字节最多前滑 1 字节，
        30 个好帧字节滑不完 79 字节垃圾 → 好帧末字节到位时 buf[0] 仍是垃圾，
        msgs=0（对旧实现实测复现）；新两段式 drain 不依赖新输入就地重扫，
        必须解出短帧。case 13（短坏帧）恰好被旧代码逐字节判据覆盖，不判别。 */
    {
        g_msgs = 0;
        uint8_t buf[1024];
        uint8_t pl70[70];
        memset(pl70, 0x5A, sizeof(pl70));
        size_t n1 = adsb_link_encode(buf, 512, ADSB_LINK_MSG_HEALTH_STATS, 40,
                                     pl70, sizeof(pl70));   /* 8+70+2 = 80 */
        CHECK(n1 == 80, "long frame len got=%zu\n", n1);
        buf[n1 - 1] ^= 0x01;                           /* 坏 CRC：本帧作废 */
        size_t n2 = build_modes_raw(buf + n1, 41);     /* 30 字节，完整在后 */
        adsb_link_dec_t d; adsb_link_dec_init(&d, sink, NULL);
        adsb_link_dec_feed(&d, buf, n1 + n2);          /* 单次 feed */
        CHECK(g_msgs == 1 && g_last.seq == 41, "msgs=%d seq=%u\n",
              g_msgs, g_last.seq);
        CHECK(d.crc_errors == 1, "crc_errors=%u\n", d.crc_errors);
    }

    /* 15. 审计复现（round 3）：协议最长帧（plen=576，共 586 B = 缓冲容量，
        v1.1 扩容后的上限）后紧跟下一帧，按 P4 实际读长 256 B 分块喂入。
        两段式 drain 在第二块的入栈阶段不跑判据，紧随其后的字节触发洪泛
        防御把已完整的 586 B 帧头部滑掉 → 第一帧丢失（464 B 时代实测
        resyncs=474、msgs=1）。修复后必须 msgs=2 且按序（seq 60 → 61）、
        无多余 resync。 */
    {
        g_msgs = 0; g_seq_hist_n = 0;
        uint8_t stream[1024];
        uint8_t bigpl[ADSB_LINK_MAX_PAYLOAD];
        memset(bigpl, 0x77, sizeof bigpl);
        size_t n1 = adsb_link_encode(stream, sizeof stream, ADSB_LINK_MSG_HEALTH_STATS,
                                     60, bigpl, sizeof bigpl);
        CHECK(n1 == 586, "max frame len got=%zu\n", n1);
        size_t n2 = build_modes_raw(stream + n1, 61);
        adsb_link_dec_t d; adsb_link_dec_init(&d, sink, NULL);
        adsb_link_dec_feed(&d, stream, 256);                    /* 第一块 */
        adsb_link_dec_feed(&d, stream + 256, (n1 - 256) + n2);  /* 第二块 */
        CHECK(g_msgs == 2, "msgs=%d\n", g_msgs);
        CHECK(g_seq_hist_n == 2 && g_seq_hist[0] == 60 && g_seq_hist[1] == 61,
              "order %d: %u,%u\n", g_seq_hist_n,
              g_seq_hist[0], g_seq_hist[1]);
        CHECK(d.resyncs == 0, "resyncs=%u\n", d.resyncs);
    }

    /* 16. 边界：plen=576 帧单独一次喂入（恰满缓冲）→ 必须解出。 */
    {
        g_msgs = 0;
        uint8_t stream[640];
        uint8_t bigpl[ADSB_LINK_MAX_PAYLOAD];
        memset(bigpl, 0x33, sizeof bigpl);
        size_t n1 = adsb_link_encode(stream, sizeof stream, ADSB_LINK_MSG_HEALTH_STATS,
                                     62, bigpl, sizeof bigpl);
        CHECK(n1 == 586, "len got=%zu\n", n1);
        adsb_link_dec_t d; adsb_link_dec_init(&d, sink, NULL);
        adsb_link_dec_feed(&d, stream, n1);
        CHECK(g_msgs == 1 && g_last.seq == 62, "msgs=%d seq=%u\n",
              g_msgs, g_last.seq);
        CHECK(d.resyncs == 0, "resyncs=%u\n", d.resyncs);
    }

    /* 17. 边界：576 帧紧跟下一帧，单次 feed 全量入栈（无分块）→ msgs=2。
        与 case 15 同根：满容后随字节在旧代码里同样触发洪泛滑窗。 */
    {
        g_msgs = 0;
        uint8_t stream[1024];
        uint8_t bigpl[ADSB_LINK_MAX_PAYLOAD];
        memset(bigpl, 0x11, sizeof bigpl);
        size_t n1 = adsb_link_encode(stream, sizeof stream, ADSB_LINK_MSG_HEALTH_STATS,
                                     63, bigpl, sizeof bigpl);
        size_t n2 = build_modes_raw(stream + n1, 64);
        adsb_link_dec_t d; adsb_link_dec_init(&d, sink, NULL);
        adsb_link_dec_feed(&d, stream, n1 + n2);
        CHECK(g_msgs == 2 && g_last.seq == 64, "msgs=%d seq=%u\n",
              g_msgs, g_last.seq);
        CHECK(d.resyncs == 0, "resyncs=%u\n", d.resyncs);
    }

    /* 18. 边界：585 B 截断（差 1 字节）必须 hold 不产帧、不计数；
        补上末字节后立即解出。 */
    {
        g_msgs = 0;
        uint8_t stream[640];
        uint8_t bigpl[ADSB_LINK_MAX_PAYLOAD];
        memset(bigpl, 0x99, sizeof bigpl);
        size_t n1 = adsb_link_encode(stream, sizeof stream, ADSB_LINK_MSG_HEALTH_STATS,
                                     65, bigpl, sizeof bigpl);
        adsb_link_dec_t d; adsb_link_dec_init(&d, sink, NULL);
        adsb_link_dec_feed(&d, stream, n1 - 1);
        CHECK(g_msgs == 0 && d.resyncs == 0 && d.crc_errors == 0,
              "truncation leaked: msgs=%d resyncs=%u crc=%u\n",
              g_msgs, d.resyncs, d.crc_errors);
        adsb_link_dec_feed(&d, stream + n1 - 1, 1);
        CHECK(g_msgs == 1 && g_last.seq == 65, "msgs=%d seq=%u\n",
              g_msgs, g_last.seq);
    }

    /* 19. v1.1 UAT_UPLINK payload 助手（协议 §6.1）：roundtrip + 错误语义。
        合成 552 B 帧不依赖解码向量，golden 向量（UP-CLEAN + CRC 0x5FAD）
        在 test_uat_ingest.c 钉死。 */
    {
        uint8_t frame[ADSB_LINK_UAT_FRAME_BYTES];
        for (size_t i = 0; i < sizeof frame; i++) frame[i] = (uint8_t)(i * 7);

        uint8_t pl[ADSB_LINK_UAT_PAYLOAD_LEN];
        CHECK(adsb_link_uat_uplink_encode(pl, sizeof pl, 0x37, 0x11223344,
                                          frame) == ADSB_LINK_UAT_PAYLOAD_LEN,
              "payload len\n");
        CHECK(pl[0] == 0x37, "rssi byte\n");
        CHECK(pl[1] == 0x44 && pl[2] == 0x33 && pl[3] == 0x22 && pl[4] == 0x11,
              "rp_ts_us LE\n");
        CHECK(memcmp(pl + ADSB_LINK_UAT_META_BYTES, frame, sizeof frame) == 0,
              "frame verbatim\n");

        uint8_t rssi; uint32_t ts; const uint8_t *fr;
        CHECK(adsb_link_uat_uplink_decode(pl, sizeof pl, &rssi, &ts, &fr),
              "decode ok\n");
        CHECK(rssi == 0x37 && ts == 0x11223344 && fr == pl + ADSB_LINK_UAT_META_BYTES,
              "fields\n");
        CHECK(memcmp(fr, frame, sizeof frame) == 0, "frame ptr\n");

        /* 错误语义：长度不符（556/558）拒绝且不写输出；encode cap 不足返回 0
         * （与 adsb_link_encode 同款返回约定）。 */
        rssi = 1; ts = 1; fr = pl;
        CHECK(!adsb_link_uat_uplink_decode(pl, ADSB_LINK_UAT_PAYLOAD_LEN - 1,
                                           &rssi, &ts, &fr), "short plen\n");
        CHECK(!adsb_link_uat_uplink_decode(pl, ADSB_LINK_UAT_PAYLOAD_LEN + 1,
                                           &rssi, &ts, &fr), "long plen\n");
        CHECK(rssi == 1 && ts == 1 && fr == pl, "outputs untouched on fail\n");
        CHECK(adsb_link_uat_uplink_encode(pl, ADSB_LINK_UAT_PAYLOAD_LEN - 1,
                                          0x37, 1, frame) == 0, "small cap\n");
    }

    /* 20. v1.1 全线上走：UAT_UPLINK 整帧（567 B）经编码→分块喂入→递交，
        头部逐字节（ver=1.1/type/plen LE）+ CRC 用独立参考实现互证，
        payload 逐字节往返。 */
    {
        g_msgs = 0;
        uint8_t frame[ADSB_LINK_UAT_FRAME_BYTES];
        for (size_t i = 0; i < sizeof frame; i++) frame[i] = (uint8_t)(i * 3 + 1);
        uint8_t wire[ADSB_LINK_MAX_FRAME];
        uint8_t pl[ADSB_LINK_UAT_PAYLOAD_LEN];
        CHECK(adsb_link_uat_uplink_encode(pl, sizeof pl, 0xC4, 0xCAFEBABE,
                                          frame) == ADSB_LINK_UAT_PAYLOAD_LEN,
              "payload build\n");
        size_t n = adsb_link_encode(wire, sizeof wire, ADSB_LINK_MSG_UAT_UPLINK,
                                    9, pl, ADSB_LINK_UAT_PAYLOAD_LEN);
        CHECK(n == ADSB_LINK_HDR_LEN + ADSB_LINK_UAT_PAYLOAD_LEN + 2,
              "wire len got=%zu\n", n);
        static const uint8_t HDR[8] = { 0x50, 0x4B, 0x01, 0x01, 0x11, 0x09,
                                        0x2D, 0x02 };
        CHECK(memcmp(wire, HDR, 8) == 0, "header bytes\n");
        uint16_t c = ref_crc16(wire, n - 2);
        CHECK(wire[n - 2] == (uint8_t)c && wire[n - 1] == (uint8_t)(c >> 8),
              "crc16 LE placement\n");

        adsb_link_dec_t d; adsb_link_dec_init(&d, sink, NULL);
        for (size_t off = 0; off < n; off += 256)          /* 分块：模拟 P4 读长 */
            adsb_link_dec_feed(&d, wire + off,
                               (n - off < 256) ? n - off : 256);
        CHECK(g_msgs == 1 && g_last.type == ADSB_LINK_MSG_UAT_UPLINK &&
              g_last.seq == 9 && g_last.payload_len == ADSB_LINK_UAT_PAYLOAD_LEN,
              "deliver msgs=%d type=%02x plen=%u\n",
              g_msgs, g_last.type, g_last.payload_len);
        CHECK(memcmp(g_last.payload, pl, ADSB_LINK_UAT_PAYLOAD_LEN) == 0,
              "payload bytes\n");
        uint8_t rssi; uint32_t ts; const uint8_t *fr;
        CHECK(adsb_link_uat_uplink_decode(g_last.payload, g_last.payload_len,
                                          &rssi, &ts, &fr));
        CHECK(rssi == 0xC4 && ts == 0xCAFEBABE, "meta rssi=%02x ts=%08x\n",
              rssi, ts);
        CHECK(memcmp(fr, frame, sizeof frame) == 0, "frame roundtrip\n");
    }

    /* 21. v1.1 长度边界：plen=577（> 576 上限）伪头 → len_errors 作废，
        随后合法帧仍可解（§3.3/§3.7 在新上限下的行为）。 */
    {
        g_msgs = 0;
        uint8_t buf[1024];
        size_t k = 0;
        buf[k++] = ADSB_LINK_MAGIC0; buf[k++] = ADSB_LINK_MAGIC1;
        buf[k++] = 1; buf[k++] = 1; buf[k++] = 0x11; buf[k++] = 0;
        buf[k++] = 0x41; buf[k++] = 0x02;             /* plen=577：超 v1.1 上限 */
        for (int i = 0; i < 10; i++) buf[k++] = (uint8_t)i;
        size_t n2 = build_modes_raw(buf + k, 5);
        adsb_link_dec_t d; adsb_link_dec_init(&d, sink, NULL);
        adsb_link_dec_feed(&d, buf, k + n2);
        CHECK(g_msgs == 1, "msgs=%d\n", g_msgs);
        CHECK(d.len_errors == 1, "len_errors=%u\n", d.len_errors);
    }

    printf(g_fail ? "FAIL (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
