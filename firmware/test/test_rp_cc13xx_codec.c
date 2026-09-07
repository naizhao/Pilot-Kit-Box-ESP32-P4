/*
 * test_rp_cc13xx_codec.c — RP2040↔CC1312R Sub-GHz 链路编解码器的 host 单测。
 *
 * 跑法（与 firmware/test/ 下其它测试同一套路）：
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/components/rp_cc13xx_codec \
 *      -I firmware/components/adsb_link_codec \
 *      -o /tmp/test_rp_cc13xx_codec \
 *      firmware/test/test_rp_cc13xx_codec.c \
 *      firmware/components/rp_cc13xx_codec/rp_cc13xx_codec.c \
 *      firmware/components/adsb_link_codec/adsb_link_codec.c \
 *   && /tmp/test_rp_cc13xx_codec
 *
 * 判据来源：firmware/PROTOCOL_RP2040_CC1312R_SPI.md v1.0。
 *
 * golden 向量总表（向量 id = 规范附录 B 条目号；十六进制由规范 §3/§4 字段表
 * 首次具体化并已回填附录 B——向量唯一权威是规范附录 B，本文件是其可执行镜像）：
 *
 *   B1  CRC KAT crc16("123456789")=0x29B1              规范 §3.2
 *   B2  HELLO 正常                                      §4.1/§6.2
 *   B3  IRQ_ACK 正常                                    §4.2
 *   B4  PING 正常                                       §4 表
 *   B5  PONG 正常                                       §4 表
 *   B6  RX_DESCRIPTOR 正常                              §4.3
 *   B7  RX_PAYLOAD_CHUNK 分片重组（12+8=20 B）          §4.4/§7.1
 *   B8  RX_PAYLOAD_CHUNK 单片极限 496 B（整帧恰 512 B）  §3.1/§4.4
 *   B9  QUEUE_FULL 正常 + events_dropped 回绕           §4.5
 *   B10 RF_CONFIG 写（config_version=3）                §4.6
 *   B11 RF_CONFIG 只读查询（config_version=0）          §4.6
 *   B12 RF_CONFIG_STATUS 回读                           §4.6
 *   B13 RESET_STATUS_REQ 正常                           §4 表
 *   B14 RESET_STATUS 正常                               §4.7
 *   B15 UPGRADE_STATUS_REQ 正常                         §4 表
 *   B16 UPGRADE_STATUS 正常                             §4.8
 *   B17 ERROR 正常（code=0x03，msg="DENY"）             §4.9
 *   B18 N1 HELLO 异版本拒收（CRC 合法）                 §6.2/§5.1
 *   B19 N2 len>502 拒收（len 先于 CRC）                 §3.1/§5.2
 *   B20 N3 坏 CRC 拒收（ver=2+坏 CRC 只计 CRC）         §5.1/§5.2
 *   B21 N4 UPGRADE_STATUS reserved≠0 → len 违规         §4.8
 *   B22 N5 未知类型 0x55 容忍                           §3.3/§5.4
 *   B23 N6 全 0x00 事务 = 合法『无帧』                  §2.3 规则 5
 *   B24 seq 0xFFFF→0x0000 回绕不是 gap                  §3.4/§5.5
 *   B25 re-HELLO 清分片半交付态                         §6.5
 *   （B26 延迟握手走查 = §2.2/§6.2 会话时序，复用 case 2/16 的帧级断言）
 *   B27 N7 零长分片拒绝（encode/decode/reasm 三处）     §4.4
 *   B28 N8 descriptor total_len>4096 双侧拒绝           §4.3
 *   B29 N9 reset_reason 发送边界掩码 &0x0F              §4.1
 *   （B30/B31 = §6.2/§6.6/§2.3 会话级走查，规范附录 B.3；无独立十六进制）
 *   B31' N10 异步 ERROR code=0x04 seq=0x0000 哨兵        §3.5/§4.9（case 24）
 *
 * B8/B19/B23 的 512 B 全帧以「构造规则」收录于附录 B（帧头/CRC 字面 +
 * data 按规则/零填充），本文件同规则逐字节断言；其余向量在附录 B 与本文件
 * 均为逐字节十六进制字面量。B1 的 CRC 另有独立参考实现互证（避免同错互证）。
 */
#include "rp_cc13xx_codec.h"
#include "adsb_link.h"      /* 独立参考实现对照：规范 §3.2 唯一权威 */

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

/* ── golden 向量字面量（规范附录 B；生成方式见文件头总表）────────────── */

static const uint8_t V_b2_hello[18] = {
    0x50, 0x4B, 0x01, 0x01, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x02, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x5A, 0xE6,
};
static const uint8_t V_b3_irq_ack[10] = {
    0x50, 0x4B, 0x01, 0x02, 0x02, 0x00, 0x00, 0x00, 0xD2, 0x80,
};
static const uint8_t V_b4_ping[10] = {
    0x50, 0x4B, 0x01, 0x03, 0x03, 0x00, 0x00, 0x00, 0x37, 0x5C,
};
static const uint8_t V_b5_pong[10] = {
    0x50, 0x4B, 0x01, 0x04, 0x01, 0x00, 0x00, 0x00, 0x8B, 0xD6,
};
static const uint8_t V_b6_rx_descriptor[24] = {
    0x50, 0x4B, 0x01, 0x10, 0x04, 0x00, 0x0E, 0x00, 0x01, 0x00, 0x80, 0x18,
    0x4B, 0x3A, 0xE8, 0x03, 0x00, 0x00, 0x14, 0x00, 0xC4, 0x01, 0x15, 0x70,
};
static const uint8_t V_b7_chunk0[28] = {
    0x50, 0x4B, 0x01, 0x11, 0x05, 0x00, 0x12, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x14, 0x00, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x41, 0x42, 0xC2, 0xC5,
};
static const uint8_t V_b7_chunk1[24] = {
    0x50, 0x4B, 0x01, 0x11, 0x06, 0x00, 0x0E, 0x00, 0x01, 0x00, 0x0C, 0x00,
    0x14, 0x00, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x12, 0x11,
};
/* B8 单片极限帧（512 B）：帧头 8 B 与 CRC 2 B 字面，data[k]=k&0xFF 规则生成 */
static const uint8_t V_b8_hdr[8] = {
    0x50, 0x4B, 0x01, 0x11, 0x07, 0x00, 0xF6, 0x01,
};
static const uint8_t V_b8_crc[2] = { 0x46, 0xF3 };
static const uint8_t V_b9_queue_full[14] = {
    0x50, 0x4B, 0x01, 0x12, 0x08, 0x00, 0x04, 0x00, 0xFE, 0xFF, 0x08, 0x00,
    0x32, 0x69,
};
static const uint8_t V_b9_queue_full_wrap[14] = {
    0x50, 0x4B, 0x01, 0x12, 0x09, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x88, 0x23,
};
static const uint8_t V_b10_rf_config[30] = {
    0x50, 0x4B, 0x01, 0x20, 0x0A, 0x00, 0x14, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB,
    0xCC, 0xDD, 0xEE, 0xFF, 0x47, 0xE7,
};
static const uint8_t V_b11_rf_config_query[30] = {
    0x50, 0x4B, 0x01, 0x20, 0x0B, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xD9, 0x9C,
};
static const uint8_t V_b12_rf_config_status[30] = {
    0x50, 0x4B, 0x01, 0x21, 0x01, 0x00, 0x14, 0x00, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB,
    0xCC, 0xDD, 0xEE, 0xFF, 0xCF, 0x53,
};
static const uint8_t V_b13_reset_status_req[10] = {
    0x50, 0x4B, 0x01, 0x22, 0x0C, 0x00, 0x00, 0x00, 0x3C, 0x2A,
};
static const uint8_t V_b14_reset_status[18] = {
    0x50, 0x4B, 0x01, 0x23, 0x02, 0x00, 0x08, 0x00, 0x02, 0x00, 0x00, 0x00,
    0x40, 0xE2, 0x01, 0x00, 0x16, 0xE2,
};
static const uint8_t V_b15_upgrade_status_req[10] = {
    0x50, 0x4B, 0x01, 0x24, 0x0D, 0x00, 0x00, 0x00, 0x0D, 0x91,
};
static const uint8_t V_b16_upgrade_status[18] = {
    0x50, 0x4B, 0x01, 0x25, 0x03, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02, 0x01, 0x00, 0x00, 0x0A, 0xA8,
};
static const uint8_t V_b17_error[16] = {
    0x50, 0x4B, 0x01, 0x7F, 0x0E, 0x00, 0x06, 0x00, 0x03, 0x04, 0x44, 0x45,
    0x4E, 0x59, 0x2F, 0xA0,
};
static const uint8_t V_b18_n1_ver2[18] = {
    0x50, 0x4B, 0x02, 0x01, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x02, 0x00,
    0x01, 0x00, 0x00, 0x00, 0xF9, 0x6B,
};
/* B19 len=503 帧：帧头 8 B 字面 + 0x00 填充至 512 B（附录 B 构造规则） */
static const uint8_t V_b19_hdr[8] = {
    0x50, 0x4B, 0x01, 0x01, 0x01, 0x00, 0xF7, 0x01,
};
static const uint8_t V_b20_n3_badcrc[18] = {
    0x50, 0x4B, 0x01, 0x01, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x02, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x5A, 0xE7,
};
static const uint8_t V_b20_n3_badcrc_ver2[18] = {
    0x50, 0x4B, 0x02, 0x01, 0x01, 0x00, 0x08, 0x00, 0x01, 0x00, 0x02, 0x00,
    0x01, 0x00, 0x00, 0x00, 0xF9, 0x6A,
};
static const uint8_t V_b21_n4_reserved[18] = {
    0x50, 0x4B, 0x01, 0x25, 0x03, 0x00, 0x08, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x02, 0x01, 0x00, 0x00, 0x6B, 0x10,
};
static const uint8_t V_b22_n5_unknown[12] = {
    0x50, 0x4B, 0x01, 0x55, 0x0F, 0x00, 0x02, 0x00, 0xCA, 0xFE, 0x11, 0x57,
};
static const uint8_t V_b24_ping_seq_ffff[10] = {
    0x50, 0x4B, 0x01, 0x03, 0xFF, 0xFF, 0x00, 0x00, 0x2B, 0x43,
};
static const uint8_t V_b24_ping_seq_0000[10] = {
    0x50, 0x4B, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0xEB, 0xC7,
};
static const uint8_t V_b27_n7_zerolen_chunk[16] = {
    0x50, 0x4B, 0x01, 0x11, 0x09, 0x00, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x14, 0x00, 0xDE, 0x7B,
};
static const uint8_t V_b28_n8_total_4097[24] = {
    0x50, 0x4B, 0x01, 0x10, 0x04, 0x00, 0x0E, 0x00, 0x01, 0x00, 0x80, 0x18,
    0x4B, 0x3A, 0xE8, 0x03, 0x00, 0x00, 0x01, 0x10, 0xC4, 0x01, 0x94, 0x94,
};
static const uint8_t V_b29_n9_masked_hello[18] = {
    0x50, 0x4B, 0x01, 0x01, 0x02, 0x00, 0x08, 0x00, 0x01, 0x00, 0x02, 0x00,
    0x0F, 0x00, 0x00, 0x00, 0x9F, 0x41,
};

int main(void)
{
    /* 1. B1（§3.2）：CRC 已知答案——同时钉住独立参考实现与复用的权威实现。 */
    CHECK(ref_crc16((const uint8_t *)"123456789", 9) == 0x29B1, "ref crc16 KAT\n");
    CHECK(rp_cc13xx_crc16((const uint8_t *)"123456789", 9) == 0x29B1,
          "codec crc16 KAT\n");
    CHECK(rp_cc13xx_crc16((const uint8_t *)"123456789", 9) ==
          adsb_link_crc16((const uint8_t *)"123456789", 9), "crc16 同源\n");

    /* 2. B2（§4.1/§6.2）：HELLO 编码逐字节 + 帧解码 + 字段解出。 */
    {
        rp_cc13xx_hello_t h = { .fw_ver_major = 1, .fw_ver_minor = 2,
                                .reset_reason = RP_CC13XX_RESET_POR };
        uint8_t buf[RP_CC13XX_MAX_FRAME];
        size_t n = rp_cc13xx_encode_hello(buf, sizeof(buf), 0x0001, &h);
        CHECK(n == sizeof(V_b2_hello), "hello len got=%zu\n", n);
        CHECK(memcmp(buf, V_b2_hello, n) == 0, "hello bytes\n");

        rp_cc13xx_msg_t m;
        CHECK(rp_cc13xx_decode_frame(V_b2_hello, sizeof(V_b2_hello), &m)
              == RP_CC13XX_OK, "hello decode\n");
        CHECK(m.type == RP_CC13XX_MSG_HELLO && m.seq == 0x0001 &&
              m.payload_len == 8, "hello hdr\n");
        rp_cc13xx_hello_t out;
        CHECK(rp_cc13xx_decode_hello(&m, &out) == RP_CC13XX_OK, "hello parse\n");
        CHECK(out.fw_ver_major == 1 && out.fw_ver_minor == 2 &&
              out.reset_reason == RP_CC13XX_RESET_POR, "hello fields\n");
    }

    /* 3. B3/B4/B5（§4.2/§4 表）：空载荷命令逐字节 + 解码。 */
    {
        uint8_t buf[16];
        size_t n = rp_cc13xx_encode_empty(buf, sizeof(buf),
                                          RP_CC13XX_MSG_IRQ_ACK, 0x0002);
        CHECK(n == sizeof(V_b3_irq_ack) &&
              memcmp(buf, V_b3_irq_ack, n) == 0, "irq_ack bytes\n");
        n = rp_cc13xx_encode_empty(buf, sizeof(buf), RP_CC13XX_MSG_PING, 0x0003);
        CHECK(n == sizeof(V_b4_ping) && memcmp(buf, V_b4_ping, n) == 0,
              "ping bytes\n");
        n = rp_cc13xx_encode_empty(buf, sizeof(buf), RP_CC13XX_MSG_PONG, 0x0001);
        CHECK(n == sizeof(V_b5_pong) && memcmp(buf, V_b5_pong, n) == 0,
              "pong bytes\n");

        rp_cc13xx_msg_t m;
        CHECK(rp_cc13xx_decode_frame(V_b3_irq_ack, sizeof(V_b3_irq_ack), &m)
              == RP_CC13XX_OK && m.payload_len == 0 &&
              m.type == RP_CC13XX_MSG_IRQ_ACK, "irq_ack decode\n");
        CHECK(rp_cc13xx_decode_frame(V_b4_ping, sizeof(V_b4_ping), &m)
              == RP_CC13XX_OK && m.type == RP_CC13XX_MSG_PING, "ping decode\n");
        CHECK(rp_cc13xx_decode_frame(V_b5_pong, sizeof(V_b5_pong), &m)
              == RP_CC13XX_OK && m.type == RP_CC13XX_MSG_PONG, "pong decode\n");
    }

    /* 4. B6（§4.3）：RX_DESCRIPTOR 逐字节 + 字段解出（含 978 MHz 小端验证）。 */
    {
        rp_cc13xx_rx_desc_t d = { .desc_id = 1, .freq_hz = 978000000u,
                                  .ts_us = 1000, .total_len = 20,
                                  .rssi = 0xC4,
                                  .flags = RP_CC13XX_DESC_F_HIGH_PRIO };
        uint8_t buf[RP_CC13XX_MAX_FRAME];
        size_t n = rp_cc13xx_encode_rx_descriptor(buf, sizeof(buf), 0x0004, &d);
        CHECK(n == sizeof(V_b6_rx_descriptor), "desc len got=%zu\n", n);
        CHECK(memcmp(buf, V_b6_rx_descriptor, n) == 0, "desc bytes\n");

        rp_cc13xx_msg_t m;
        CHECK(rp_cc13xx_decode_frame(V_b6_rx_descriptor,
                                     sizeof(V_b6_rx_descriptor), &m)
              == RP_CC13XX_OK, "desc decode\n");
        rp_cc13xx_rx_desc_t out;
        CHECK(rp_cc13xx_decode_rx_descriptor(&m, &out) == RP_CC13XX_OK,
              "desc parse\n");
        CHECK(out.desc_id == 1 && out.freq_hz == 978000000u &&
              out.ts_us == 1000 && out.total_len == 20 &&
              out.rssi == 0xC4 && out.flags == RP_CC13XX_DESC_F_HIGH_PRIO,
              "desc fields\n");
    }

    /* 5. B7（§4.4/§7.1）：两片 20 B 报文——逐字面向量 + offset 升序重组一致。 */
    {
        static const uint8_t MSG20[21] = "0123456789ABCDEFGHIJ";  /* 20 B + NUL */
        rp_cc13xx_chunk_t c0 = { .desc_id = 1, .offset = 0, .total_len = 20,
                                 .data_len = 12 };
        rp_cc13xx_chunk_t c1 = { .desc_id = 1, .offset = 12, .total_len = 20,
                                 .data_len = 8 };
        memcpy(c0.data, MSG20, 12);
        memcpy(c1.data, MSG20 + 12, 8);
        uint8_t buf[RP_CC13XX_MAX_FRAME];
        size_t n0 = rp_cc13xx_encode_rx_chunk(buf, sizeof(buf), 0x0005, &c0);
        CHECK(n0 == sizeof(V_b7_chunk0) &&
              memcmp(buf, V_b7_chunk0, n0) == 0, "chunk0 bytes\n");
        size_t n1 = rp_cc13xx_encode_rx_chunk(buf, sizeof(buf), 0x0006, &c1);
        CHECK(n1 == sizeof(V_b7_chunk1) &&
              memcmp(buf, V_b7_chunk1, n1) == 0, "chunk1 bytes\n");

        rp_cc13xx_reasm_t r;
        rp_cc13xx_reasm_init(&r);
        rp_cc13xx_msg_t m;
        rp_cc13xx_rx_desc_t d = { .desc_id = 1, .total_len = 20 };
        CHECK(rp_cc13xx_decode_frame(V_b6_rx_descriptor,
                                     sizeof(V_b6_rx_descriptor), &m) == RP_CC13XX_OK,
              "desc decode\n");
        CHECK(rp_cc13xx_decode_rx_descriptor(&m, &d) == RP_CC13XX_OK,
              "desc parse\n");
        CHECK(rp_cc13xx_reasm_start(&r, &d) == RP_CC13XX_OK, "reasm start\n");
        CHECK(rp_cc13xx_decode_frame(V_b7_chunk0, sizeof(V_b7_chunk0), &m)
              == RP_CC13XX_OK, "chunk0 decode\n");
        rp_cc13xx_chunk_t pc;
        CHECK(rp_cc13xx_decode_rx_chunk(&m, &pc) == RP_CC13XX_OK &&
              pc.offset == 0 && pc.data_len == 12, "chunk0 parse\n");
        CHECK(rp_cc13xx_reasm_feed(&r, &pc) == RP_CC13XX_OK && r.have == 12,
              "chunk0 feed\n");
        CHECK(rp_cc13xx_decode_frame(V_b7_chunk1, sizeof(V_b7_chunk1), &m)
              == RP_CC13XX_OK, "chunk1 decode\n");
        CHECK(rp_cc13xx_decode_rx_chunk(&m, &pc) == RP_CC13XX_OK &&
              pc.offset == 12 && pc.data_len == 8, "chunk1 parse\n");
        CHECK(rp_cc13xx_reasm_feed(&r, &pc) == RP_CC13XX_OK, "chunk1 feed\n");
        CHECK(r.active && r.have == r.total_len, "reasm complete\n");
        CHECK(memcmp(r.data, MSG20, sizeof(MSG20)) == 0, "reasm bytes\n");
    }

    /* 6. B8（§3.1/§4.4）：单片极限 data=496 → 整帧恰 512 B = 事务长度；
        帧头/CRC 字面 + data 规则（data[k]=k&0xFF）逐字节断言 + 往返。 */
    {
        rp_cc13xx_chunk_t c = { .desc_id = 2, .offset = 0, .total_len = 496,
                                .data_len = RP_CC13XX_CHUNK_MAX_DATA };
        for (int k = 0; k < RP_CC13XX_CHUNK_MAX_DATA; k++)
            c.data[k] = (uint8_t)k;
        uint8_t buf[RP_CC13XX_MAX_FRAME];
        size_t n = rp_cc13xx_encode_rx_chunk(buf, sizeof(buf), 0x0007, &c);
        CHECK(n == 512, "max frame len got=%zu\n", n);
        CHECK(memcmp(buf, V_b8_hdr, 8) == 0, "max hdr\n");
        CHECK(memcmp(buf + n - 2, V_b8_crc, 2) == 0, "max crc\n");
        for (int k = 0; k < RP_CC13XX_CHUNK_MAX_DATA; k++)
            if (buf[RP_CC13XX_HDR_LEN + RP_CC13XX_CHUNK_HDR_LEN + k] !=
                (uint8_t)k) {
                CHECK(0, "max data[%d]\n", k);
                break;
            }

        rp_cc13xx_msg_t m;
        CHECK(rp_cc13xx_decode_frame(buf, n, &m) == RP_CC13XX_OK,
              "max decode\n");
        rp_cc13xx_chunk_t out;
        CHECK(rp_cc13xx_decode_rx_chunk(&m, &out) == RP_CC13XX_OK &&
              out.desc_id == 2 && out.total_len == 496 &&
              out.data_len == 496, "max parse\n");
    }

    /* 7. B9（§4.5）：QUEUE_FULL 正常 + events_dropped 模 2^16 回绕（0xFFFE
        → 0x0001 = 单调 +3，无符号差值判回绕，§3.4 精神）。 */
    {
        rp_cc13xx_queue_full_t q = { .events_dropped = 0xFFFE, .queue_depth = 8 };
        uint8_t buf[RP_CC13XX_MAX_FRAME];
        size_t n = rp_cc13xx_encode_queue_full(buf, sizeof(buf), 0x0008, &q);
        CHECK(n == sizeof(V_b9_queue_full) &&
              memcmp(buf, V_b9_queue_full, n) == 0, "queue_full bytes\n");

        rp_cc13xx_msg_t m;
        CHECK(rp_cc13xx_decode_frame(V_b9_queue_full_wrap,
                                     sizeof(V_b9_queue_full_wrap), &m)
              == RP_CC13XX_OK, "queue_full decode\n");
        rp_cc13xx_queue_full_t out;
        CHECK(rp_cc13xx_decode_queue_full(&m, &out) == RP_CC13XX_OK,
              "queue_full parse\n");
        CHECK(out.events_dropped == 0x0001 && out.queue_depth == 0,
              "queue_full wrap fields\n");
        CHECK((uint16_t)(out.events_dropped - 0xFFFE) == 3, "wrap delta\n");
    }

    /* 8. B10/B11/B12（§4.6）：RF_CONFIG 写 / 只读查询 / STATUS 回读。 */
    {
        rp_cc13xx_rf_config_t cfg;
        cfg.config_version = 3;
        for (int i = 0; i < 16; i++) cfg.digest[i] = (uint8_t)(i * 0x11);
        uint8_t buf[RP_CC13XX_MAX_FRAME];
        size_t n = rp_cc13xx_encode_rf_config(buf, sizeof(buf),
                                              RP_CC13XX_MSG_RF_CONFIG,
                                              0x000A, &cfg);
        CHECK(n == sizeof(V_b10_rf_config) &&
              memcmp(buf, V_b10_rf_config, n) == 0, "rf_config bytes\n");

        rp_cc13xx_msg_t m;
        CHECK(rp_cc13xx_decode_frame(V_b11_rf_config_query,
                                     sizeof(V_b11_rf_config_query), &m)
              == RP_CC13XX_OK && m.type == RP_CC13XX_MSG_RF_CONFIG,
              "rf_query decode\n");
        rp_cc13xx_rf_config_t out;
        CHECK(rp_cc13xx_decode_rf_config(&m, &out) == RP_CC13XX_OK &&
              out.config_version == 0, "rf_query parse\n");

        CHECK(rp_cc13xx_decode_frame(V_b12_rf_config_status,
                                     sizeof(V_b12_rf_config_status), &m)
              == RP_CC13XX_OK && m.type == RP_CC13XX_MSG_RF_CONFIG_STATUS,
              "rf_status decode\n");
        CHECK(rp_cc13xx_decode_rf_config(&m, &out) == RP_CC13XX_OK &&
              out.config_version == 3 &&
              memcmp(out.digest, cfg.digest, 16) == 0, "rf_status parse\n");
    }

    /* 9. B13/B14/B15（§4 表/§4.7）：RESET_STATUS_REQ / RESET_STATUS /
        UPGRADE_STATUS_REQ。 */
    {
        uint8_t buf[16];
        size_t n = rp_cc13xx_encode_empty(buf, sizeof(buf),
                                          RP_CC13XX_MSG_RESET_STATUS_REQ,
                                          0x000C);
        CHECK(n == sizeof(V_b13_reset_status_req) &&
              memcmp(buf, V_b13_reset_status_req, n) == 0, "rs_req bytes\n");
        n = rp_cc13xx_encode_empty(buf, sizeof(buf),
                                   RP_CC13XX_MSG_UPGRADE_STATUS_REQ, 0x000D);
        CHECK(n == sizeof(V_b15_upgrade_status_req) &&
              memcmp(buf, V_b15_upgrade_status_req, n) == 0, "us_req bytes\n");

        rp_cc13xx_msg_t m;
        CHECK(rp_cc13xx_decode_frame(V_b14_reset_status,
                                     sizeof(V_b14_reset_status), &m)
              == RP_CC13XX_OK, "reset_status decode\n");
        rp_cc13xx_reset_status_t out;
        CHECK(rp_cc13xx_decode_reset_status(&m, &out) == RP_CC13XX_OK,
              "reset_status parse\n");
        CHECK(out.reset_reason == RP_CC13XX_RESET_RESETN &&
              out.uptime_ms == 123456, "reset_status fields\n");
    }

    /* 10. B16/B17（§4.8/§4.9）：UPGRADE_STATUS 与 ERROR（code=0x03、"DENY"）。 */
    {
        rp_cc13xx_msg_t m;
        CHECK(rp_cc13xx_decode_frame(V_b16_upgrade_status,
                                     sizeof(V_b16_upgrade_status), &m)
              == RP_CC13XX_OK, "upgrade decode\n");
        rp_cc13xx_upgrade_status_t u;
        CHECK(rp_cc13xx_decode_upgrade_status(&m, &u) == RP_CC13XX_OK &&
              u.state == RP_CC13XX_UPG_NORMAL &&
              u.running_image_version == 0x00000102, "upgrade fields\n");

        CHECK(rp_cc13xx_decode_frame(V_b17_error, sizeof(V_b17_error), &m)
              == RP_CC13XX_OK && m.type == RP_CC13XX_MSG_ERROR, "error decode\n");
        rp_cc13xx_error_t e;
        CHECK(rp_cc13xx_decode_error(&m, &e) == RP_CC13XX_OK &&
              e.code == 0x03 && e.msg_len == 4 &&
              memcmp(e.msg, "DENY", 4) == 0, "error fields\n");
    }

    /* 11. B18（§6.2/§5.1）：HELLO 异版本（CRC 合法）→ 拒收计 version_mismatch。 */
    {
        rp_cc13xx_msg_t m;
        CHECK(rp_cc13xx_decode_frame(V_b18_n1_ver2, sizeof(V_b18_n1_ver2), &m)
              == RP_CC13XX_ERR_VERSION, "ver2 status\n");
    }

    /* 12. B19（§3.1/§5.2）：len=503 > 502 → 整事务作废计 len_errors；
        len 判定先于 CRC（§5.1），后续字节全 0 也不得解出。 */
    {
        uint8_t buf[512];
        memset(buf, 0, sizeof(buf));
        memcpy(buf, V_b19_hdr, 8);
        rp_cc13xx_msg_t m;
        CHECK(rp_cc13xx_decode_frame(buf, sizeof(buf), &m) == RP_CC13XX_ERR_LEN,
              "len503 status\n");
    }

    /* 13. B20（§5.1/§5.2）：坏 CRC 一律计 crc_errors——含 ver=2 + 坏 CRC 的
        帧必须归 CRC 不得计 version_mismatch（防噪声伪造版本不符）。 */
    {
        rp_cc13xx_msg_t m;
        CHECK(rp_cc13xx_decode_frame(V_b20_n3_badcrc,
                                     sizeof(V_b20_n3_badcrc), &m)
              == RP_CC13XX_ERR_CRC, "badcrc status\n");
        CHECK(rp_cc13xx_decode_frame(V_b20_n3_badcrc_ver2,
                                     sizeof(V_b20_n3_badcrc_ver2), &m)
              == RP_CC13XX_ERR_CRC, "badcrc+ver2 must be CRC\n");
    }

    /* 14. B21（§4.8）：UPGRADE_STATUS reserved≠0 → payload 违规与 len 违规
        同计 len_errors、作废整事务。 */
    {
        rp_cc13xx_msg_t m;
        CHECK(rp_cc13xx_decode_frame(V_b21_n4_reserved,
                                     sizeof(V_b21_n4_reserved), &m)
              == RP_CC13XX_ERR_LEN, "reserved status\n");
    }

    /* 15. B22（§3.3/§5.4）：未知 msg_type（CRC 合法）必须容忍——返回
        UNKNOWN_TYPE 且消息原样可读，由调用方计数忽略。 */
    {
        rp_cc13xx_msg_t m;
        CHECK(rp_cc13xx_decode_frame(V_b22_n5_unknown,
                                     sizeof(V_b22_n5_unknown), &m)
              == RP_CC13XX_UNKNOWN_TYPE, "unknown status\n");
        CHECK(m.type == 0x55 && m.payload_len == 2 &&
              m.payload[0] == 0xCA && m.payload[1] == 0xFE, "unknown msg\n");
    }

    /* 16. B23（§2.3 规则 5）：全 0x00 事务 = 合法『无帧』，不计错误；
        附加行为钉住：半帧 → SHORT 不计错；buflen > 512 → ARG（§2.1 定长）。 */
    {
        uint8_t buf[512];
        memset(buf, 0, sizeof(buf));
        rp_cc13xx_msg_t m;
        CHECK(rp_cc13xx_decode_frame(buf, sizeof(buf), &m)
              == RP_CC13XX_NO_FRAME, "allzero status\n");

        memcpy(buf, V_b2_hello, sizeof(V_b2_hello) - 1);   /* 差 1 字节 */
        CHECK(rp_cc13xx_decode_frame(buf, sizeof(V_b2_hello) - 1, &m)
              == RP_CC13XX_ERR_SHORT, "short status\n");
        memcpy(buf, V_b2_hello, sizeof(V_b2_hello));       /* 帧尾 + 填充 */
        CHECK(rp_cc13xx_decode_frame(buf, sizeof(buf), &m) == RP_CC13XX_OK,
              "padding ignore\n");
        uint8_t big[513];
        memcpy(big, V_b2_hello, sizeof(V_b2_hello));
        CHECK(rp_cc13xx_decode_frame(big, sizeof(big), &m) == RP_CC13XX_ERR_ARG,
              "oversize buf status\n");
    }

    /* 17. B24（§3.4/§5.5）：seq 0xFFFF→0x0000 回绕不是 gap；真 gap 反例对照。 */
    {
        rp_cc13xx_msg_t m;
        CHECK(rp_cc13xx_decode_frame(V_b24_ping_seq_ffff,
                                     sizeof(V_b24_ping_seq_ffff), &m)
              == RP_CC13XX_OK && m.seq == 0xFFFF, "seq ffff decode\n");
        CHECK(rp_cc13xx_decode_frame(V_b24_ping_seq_0000,
                                     sizeof(V_b24_ping_seq_0000), &m)
              == RP_CC13XX_OK && m.seq == 0x0000, "seq 0000 decode\n");
        CHECK(rp_cc13xx_seq_gap(0xFFFF, 0x0000) == 0, "wrap is not gap\n");
        CHECK(rp_cc13xx_seq_gap(0x0000, 0x0001) == 0, "normal +1\n");
        CHECK(rp_cc13xx_seq_gap(0x0000, 0x0002) == 1, "skip is gap\n");
        CHECK(rp_cc13xx_seq_gap(0x0001, 0x0000) == 1, "backwards is gap\n");
    }

    /* 18. B25（§6.5）：收到合法 HELLO 即清理半交付态——已积累分片作废、
        后续旧分片不再命中；随后新 descriptor 可干净重组。 */
    {
        rp_cc13xx_reasm_t r;
        rp_cc13xx_reasm_init(&r);
        rp_cc13xx_rx_desc_t d = { .desc_id = 1, .total_len = 20 };
        rp_cc13xx_chunk_t c = { .desc_id = 1, .offset = 0, .total_len = 20,
                                .data_len = 12 };
        CHECK(rp_cc13xx_reasm_start(&r, &d) == RP_CC13XX_OK &&
              rp_cc13xx_reasm_feed(&r, &c) == RP_CC13XX_OK && r.have == 12,
              "pre-hello state\n");
        rp_cc13xx_reasm_on_hello(&r);
        CHECK(!r.active && r.have == 0, "hello cleared\n");
        CHECK(rp_cc13xx_reasm_feed(&r, &c) == RP_CC13XX_ERR_ARG,
              "stale chunk rejected\n");

        static const uint8_t MSG20[21] = "0123456789ABCDEFGHIJ";  /* 20 B + NUL */
        d.desc_id = 2;
        c.desc_id = 2;
        c.offset = 0;
        memcpy(c.data, MSG20, 12);
        CHECK(rp_cc13xx_reasm_start(&r, &d) == RP_CC13XX_OK &&
              rp_cc13xx_reasm_feed(&r, &c) == RP_CC13XX_OK, "restart chunk0\n");
        c.offset = 12;
        c.data_len = 8;
        memcpy(c.data, MSG20 + 12, 8);
        CHECK(rp_cc13xx_reasm_feed(&r, &c) == RP_CC13XX_OK &&
              r.have == 20 && memcmp(r.data, MSG20, 20) == 0,
              "restart complete\n");

        /* §4.3/§4.4 违约：total 超报文上限、desc_id 不一致、offset 不递进。 */
        rp_cc13xx_rx_desc_t bad = { .desc_id = 3, .total_len = 4097 };
        CHECK(rp_cc13xx_reasm_start(&r, &bad) == RP_CC13XX_ERR_LEN,
              "total>4096\n");
        rp_cc13xx_chunk_t stray = { .desc_id = 9, .offset = 0,
                                    .total_len = 20, .data_len = 4 };
        CHECK(rp_cc13xx_reasm_feed(&r, &stray) == RP_CC13XX_ERR_LEN,
              "desc_id mismatch\n");
        stray.desc_id = 2;
        stray.offset = 5;
        CHECK(rp_cc13xx_reasm_feed(&r, &stray) == RP_CC13XX_ERR_LEN,
              "offset not advancing\n");
    }

    /* 19. 编码边界与 typed decode 防御（§3.1/§4.4/§4.8/§4.9）。 */
    {
        uint8_t buf[RP_CC13XX_MAX_FRAME];
        uint8_t big[RP_CC13XX_MAX_PAYLOAD + 1] = { 0 };
        CHECK(rp_cc13xx_encode(buf, sizeof(buf), RP_CC13XX_MSG_PING, 0, big,
                               RP_CC13XX_MAX_PAYLOAD + 1) == 0, "plen>502\n");
        CHECK(rp_cc13xx_encode(buf, 9, RP_CC13XX_MSG_PING, 0, NULL, 0) == 0,
              "cap short\n");
        rp_cc13xx_chunk_t c = { .desc_id = 1, .offset = 0, .total_len = 497,
                                .data_len = 497 };
        CHECK(rp_cc13xx_encode_rx_chunk(buf, sizeof(buf), 0, &c) == 0,
              "chunk data>496\n");
        rp_cc13xx_error_t e = { .code = 1, .msg_len = 33 };
        CHECK(rp_cc13xx_encode_error(buf, sizeof(buf), 0, &e) == 0,
              "error msg>32\n");
        CHECK(rp_cc13xx_encode_empty(buf, sizeof(buf), 0x00, 0) == 0,
              "type 0x00 disabled\n");

        rp_cc13xx_msg_t m = { .type = RP_CC13XX_MSG_HELLO, .payload_len = 7 };
        rp_cc13xx_hello_t h;
        CHECK(rp_cc13xx_decode_hello(&m, &h) == RP_CC13XX_ERR_LEN,
              "hello plen!=8\n");
        m.type = RP_CC13XX_MSG_UPGRADE_STATUS;
        m.payload_len = 8;
        m.payload[1] = 1;
        rp_cc13xx_upgrade_status_t u;
        CHECK(rp_cc13xx_decode_upgrade_status(&m, &u) == RP_CC13XX_ERR_LEN,
              "upgrade reserved!=0\n");
    }

    /* 20. B27（§4.4）：零长分片三处拒绝——encode(0)=0、decode_frame ERR_LEN
        （CRC 合法、死于 data≥1）、typed decode ERR_LEN、reasm_feed ERR_LEN。
        反证：若接受零长片，reasm have 不变而流水继续，重组永久停滞。 */
    {
        uint8_t buf[RP_CC13XX_MAX_FRAME];
        rp_cc13xx_chunk_t c = { .desc_id = 1, .offset = 0, .total_len = 20,
                                .data_len = 0 };
        CHECK(rp_cc13xx_encode_rx_chunk(buf, sizeof(buf), 0x0009, &c) == 0,
              "zero-len encode\n");

        rp_cc13xx_msg_t m;
        CHECK(rp_cc13xx_decode_frame(V_b27_n7_zerolen_chunk,
                                     sizeof(V_b27_n7_zerolen_chunk), &m)
              == RP_CC13XX_ERR_LEN, "zero-len decode_frame\n");
        m.type = RP_CC13XX_MSG_RX_PAYLOAD_CHUNK;   /* typed decode 单测：
            payload = V_b27 帧本体前 6 B（data_len=0） */
        m.payload_len = 6;
        memcpy(m.payload, V_b27_n7_zerolen_chunk + RP_CC13XX_HDR_LEN, 6);
        CHECK(rp_cc13xx_decode_rx_chunk(&m, &c) == RP_CC13XX_ERR_LEN,
              "zero-len typed decode\n");

        rp_cc13xx_reasm_t r;
        rp_cc13xx_reasm_init(&r);
        rp_cc13xx_rx_desc_t d = { .desc_id = 1, .total_len = 20 };
        CHECK(rp_cc13xx_reasm_start(&r, &d) == RP_CC13XX_OK, "b27 start\n");
        c.data_len = 0;
        CHECK(rp_cc13xx_reasm_feed(&r, &c) == RP_CC13XX_ERR_LEN && r.have == 0,
              "zero-len reasm\n");
    }

    /* 21. B28（§4.3）：total_len > 4096 编码侧拒绝；解码侧（decode_frame 与
        typed decode）计 len 违规——此前仅 reasm_start 拒绝，边界不齐。 */
    {
        uint8_t buf[RP_CC13XX_MAX_FRAME];
        rp_cc13xx_rx_desc_t d = { .desc_id = 1, .total_len = 4097 };
        CHECK(rp_cc13xx_encode_rx_descriptor(buf, sizeof(buf), 0, &d) == 0,
              "total4097 encode\n");

        rp_cc13xx_msg_t m;
        CHECK(rp_cc13xx_decode_frame(V_b28_n8_total_4097,
                                     sizeof(V_b28_n8_total_4097), &m)
              == RP_CC13XX_ERR_LEN, "total4097 decode_frame\n");
        /* P2-b：decode_frame 拒收时不填 out——typed decode 必须用显式构造的
         * 合法 msg（total_len=4097、保留位合规）单独证明 typed 路径本身
         * 拒绝，不得复用未初始化对象（旧写法是假通过）。 */
        m.type = RP_CC13XX_MSG_RX_DESCRIPTOR;
        m.seq = 0x0004;
        m.payload_len = RP_CC13XX_RX_DESCRIPTOR_LEN;
        memcpy(m.payload, V_b28_n8_total_4097 + RP_CC13XX_HDR_LEN,
               RP_CC13XX_RX_DESCRIPTOR_LEN);
        CHECK(rp_cc13xx_decode_rx_descriptor(&m, &d) == RP_CC13XX_ERR_LEN,
              "total4097 typed decode\n");
    }

    /* 22. B29（§4.1）：reset_reason 源值带保留/厂商位（0x1F）→ 编码边界
        掩码为 0x0F 上线；解码端只见 0x0F（接收方容忍语义不受影响）。
        RESET_STATUS 同掩码。 */
    {
        rp_cc13xx_hello_t h = { .fw_ver_major = 1, .fw_ver_minor = 2,
                                .reset_reason = 0x1Fu };
        uint8_t buf[RP_CC13XX_MAX_FRAME];
        size_t n = rp_cc13xx_encode_hello(buf, sizeof(buf), 0x0002, &h);
        CHECK(n == sizeof(V_b29_n9_masked_hello) && n == 18,
              "masked hello len got=%zu\n", n);
        CHECK(memcmp(buf, V_b29_n9_masked_hello, n) == 0, "masked hello bytes\n");

        rp_cc13xx_msg_t m;
        CHECK(rp_cc13xx_decode_frame(buf, n, &m) == RP_CC13XX_OK,
              "masked hello decode\n");
        rp_cc13xx_hello_t out;
        CHECK(rp_cc13xx_decode_hello(&m, &out) == RP_CC13XX_OK &&
              out.reset_reason == 0x0Fu, "masked hello wire value\n");

        rp_cc13xx_reset_status_t s = { .reset_reason = 0x00000030u,
                                       .uptime_ms = 77 };
        n = rp_cc13xx_encode_reset_status(buf, sizeof(buf), 0x0003, &s);
        CHECK(n == 18 && memcmp(buf + 8, "\x00\x00\x00\x00", 4) == 0 &&
              buf[12] == 77,   /* reset_reason 在 payload[0..3]=帧偏移 8..11 */
              "masked reset_status wire\n");
    }

    /* 23. 审计 P1-c（§4.4）：reasm_feed 对 app 侧自组 chunk 必须先做
        data_len 上限防御——497 > data[496] 数组容量，旧实现直接 memcpy
        越界读（ASan 复现）。total_len=4096 使既有溢出判据不拦截，故须
        显式上限。 */
    {
        rp_cc13xx_reasm_t r;
        rp_cc13xx_reasm_init(&r);
        rp_cc13xx_rx_desc_t d = { .desc_id = 1, .total_len = 4096 };
        CHECK(rp_cc13xx_reasm_start(&r, &d) == RP_CC13XX_OK, "oob start\n");
        rp_cc13xx_chunk_t c = { .desc_id = 1, .offset = 0, .total_len = 4096,
                                .data_len = 497 };
        memset(c.data, 0xA5, sizeof(c.data));   /* 恰好填满 496 B 容量 */
        CHECK(rp_cc13xx_reasm_feed(&r, &c) == RP_CC13XX_ERR_LEN && r.have == 0,
              "oob chunk rejected\n");
    }

    /* 24. 审计 P2-a（§3.5/§4.9）：异步 ERROR（code 0x04/0x05）无对应命令，
        seq 恒 0x0000 哨兵——编码按此构造、解码原样接受，seq 对账跳过由
        调用方按 code 区分（codec 不解释语义，只钉住线格式可往返）。 */
    {
        uint8_t buf[RP_CC13XX_MAX_FRAME];
        rp_cc13xx_error_t e = { .code = 0x04, .msg_len = 0 };
        size_t n = rp_cc13xx_encode_error(buf, sizeof(buf), 0x0000, &e);
        CHECK(n == 12, "async err len got=%zu\n", n);

        rp_cc13xx_msg_t m;
        CHECK(rp_cc13xx_decode_frame(buf, n, &m) == RP_CC13XX_OK &&
              m.type == RP_CC13XX_MSG_ERROR && m.seq == 0x0000,
              "async err seq0\n");
        rp_cc13xx_error_t out;
        CHECK(rp_cc13xx_decode_error(&m, &out) == RP_CC13XX_OK &&
              out.code == 0x04 && out.msg_len == 0, "async err parse\n");
    }

    printf(g_fail ? "FAIL (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
