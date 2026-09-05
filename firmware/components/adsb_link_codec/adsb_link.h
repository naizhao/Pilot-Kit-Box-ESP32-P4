/*
 * adsb_link.h — P4↔RP2040 链路 v1 的共享编解码器接口。
 *
 * 规范：firmware/PROTOCOL_P4_RP2040_UART.md（v1.0）。本文件被两侧固件编译
 * 同一份实现（P4: components/adsb_link_codec；RP2040: rp2040/CMakeLists.txt
 * 直接引用同一 .c），协议永不漂移。纯 C99，无平台依赖。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define ADSB_LINK_MAGIC0       0x50   /* 'P' */
#define ADSB_LINK_MAGIC1       0x4B   /* 'K' */
#define ADSB_LINK_VER_MAJOR    1
#define ADSB_LINK_VER_MINOR    0
#define ADSB_LINK_HDR_LEN      8
#define ADSB_LINK_MAX_PAYLOAD  464
#define ADSB_LINK_MAX_FRAME    (ADSB_LINK_HDR_LEN + ADSB_LINK_MAX_PAYLOAD + 2)

/* msg_type（v1，规范 §2） */
#define ADSB_LINK_MSG_HELLO         0x01
#define ADSB_LINK_MSG_CAPABILITIES  0x02
#define ADSB_LINK_MSG_HEALTH_STATS  0x03
#define ADSB_LINK_MSG_MODES_RAW     0x10
#define ADSB_LINK_MSG_CONFIG_REQ    0x20
#define ADSB_LINK_MSG_CONFIG_ACK    0x21
#define ADSB_LINK_MSG_ERROR         0x7F

/* MODES_RAW payload */
#define ADSB_LINK_MODES_LONG        0x01   /* flags bit0：112-bit 帧 */

uint16_t adsb_link_crc16(const uint8_t *data, size_t len);   /* CCITT-FALSE */

/* 返回帧总长；cap 不足或 payload_len 超限返回 0。 */
size_t adsb_link_encode(uint8_t *out, size_t cap, uint8_t msg_type,
                        uint8_t seq, const uint8_t *payload,
                        size_t payload_len);

typedef struct {
    uint8_t  type;
    uint8_t  seq;
    uint16_t payload_len;
    uint8_t  payload[ADSB_LINK_MAX_PAYLOAD];
} adsb_link_msg_t;

typedef void (*adsb_link_on_msg_fn)(void *user, const adsb_link_msg_t *msg);

typedef struct adsb_link_dec {
    uint8_t  buf[ADSB_LINK_MAX_FRAME];
    uint16_t fill;
    uint8_t  last_seq;
    uint8_t  have_seq;
    /* 计数器（规范 §3；诊断页/HEALTH_STATS 直接读这里） */
    uint32_t msgs;              /* 成功递交的消息数 */
    uint32_t crc_errors;
    uint32_t resyncs;           /* 头部扫描前进次数 */
    uint32_t len_errors;
    uint32_t version_mismatch;
    uint32_t seq_gaps;
    adsb_link_on_msg_fn cb;
    void    *user;
} adsb_link_dec_t;

void adsb_link_dec_init(adsb_link_dec_t *d, adsb_link_on_msg_fn cb,
                        void *user);
void adsb_link_dec_feed(adsb_link_dec_t *d, const uint8_t *bytes, size_t n);
