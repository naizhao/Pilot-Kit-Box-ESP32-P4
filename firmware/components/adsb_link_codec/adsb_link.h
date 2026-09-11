/*
 * adsb_link.h — P4↔RP2040 链路 v1 的共享编解码器接口。
 *
 * 规范：firmware/PROTOCOL_P4_RP2040_UART.md（v1.1；v1.0 冻结于 2026-09-05，
 * v1.1 于 2026-09-08 向前兼容新增 UAT_UPLINK 与 payload 上限扩容，见规范
 * §6）。本文件被两侧固件编译同一份实现（P4: components/adsb_link_codec；
 * RP2040: rp2040/CMakeLists.txt 直接引用同一 .c），协议永不漂移。纯 C99，
 * 无平台依赖。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ADSB_LINK_MAGIC0       0x50   /* 'P' */
#define ADSB_LINK_MAGIC1       0x4B   /* 'K' */
#define ADSB_LINK_VER_MAJOR    1
#define ADSB_LINK_VER_MINOR    2      /* v1.2（v1.1=1、v1.0=0；接收方只看 major）*/
#define ADSB_LINK_HDR_LEN      8
#define ADSB_LINK_MAX_PAYLOAD  576    /* v1.0 为 464；v1.1 扩容，规范 §6 */
#define ADSB_LINK_MAX_FRAME    (ADSB_LINK_HDR_LEN + ADSB_LINK_MAX_PAYLOAD + 2)

/* msg_type（v1，规范 §2；0x11 为 v1.1 新增） */
#define ADSB_LINK_MSG_HELLO         0x01
#define ADSB_LINK_MSG_CAPABILITIES  0x02
#define ADSB_LINK_MSG_HEALTH_STATS  0x03
#define ADSB_LINK_MSG_MODES_RAW     0x10
#define ADSB_LINK_MSG_UAT_UPLINK    0x11   /* v1.1，规范 §6 */
#define ADSB_LINK_MSG_CONFIG_REQ    0x20   /* v1.2 起有定义，见下 */
#define ADSB_LINK_MSG_CONFIG_ACK    0x21
#define ADSB_LINK_MSG_ERROR         0x7F

/* ── CONFIG_REQ / CONFIG_ACK（v1.2，规范 §7）───────────────────────────
 * v1.0/v1.1 里这两个 type 是「预留不实现」，RP 侧收到一律回 ERROR code=1。
 * v1.2 起定义为一组 key/value：
 *   CONFIG_REQ (P4→RP): { u8 n; {u8 key; u8 val;} item[n] }
 *   CONFIG_ACK (RP→P4): { u8 applied; u8 rejected; }
 *
 * 键值编号刻意让**全 0 的 payload 等于上电默认**（与 rf_safety.c 的开机
 * 安全向量一致）。这样"配置没送到"和"配置是默认值"落到同一个状态，不会
 * 出现一半新一半旧的天线组合——RF 通路上半套配置是会让人查半天的。
 *
 * 未知 key 计入 rejected 但不影响其它项（minor 前向兼容，与 §3.5 未知
 * msg_type 必须递交同一条原则）。 */
#define ADSB_LINK_CFG_MAX_ITEMS   16
#define ADSB_LINK_CFG_ANT_1090    0x01   /* 0=板载 IFA（默认）/ 1=外接 J6 */
#define ADSB_LINK_CFG_ANT_GNSS    0x02   /* 0=外接 J2（默认）/ 1=板载 patch */

typedef struct { uint8_t key, val; } adsb_link_cfg_item_t;

/* 组 CONFIG_REQ payload。返回 payload 长度；cap 不足或 n 超限返回 0
 * （与 adsb_link_encode 同款约定：失败返回 0 且不写输出）。 */
size_t adsb_link_config_encode(uint8_t *out, size_t cap,
                               const adsb_link_cfg_item_t *items, uint8_t n);

/* 拆 CONFIG_REQ payload 到调用方数组。长度与声明的条数对不上、或 cap 装不下
 * 就返回 false 且**不写任何输出**（与 uat_uplink_decode 同款单元契约）。
 * 这里拷贝而不是像 UAT 那样回指 payload 内部：item 是结构体，回指要把
 * uint8_t* 转成结构体指针，白惹一个严格别名问题，而这里总共最多 32 字节。*/
bool adsb_link_config_decode(const uint8_t *payload, size_t len,
                             adsb_link_cfg_item_t *out, uint8_t cap,
                             uint8_t *n);

/* MODES_RAW payload */
#define ADSB_LINK_MODES_LONG        0x01   /* flags bit0：112-bit 帧 */

/* UAT_UPLINK payload（v1.1 规范 §6.1）：552 B 交织帧原样 + 5 B 元数据，
 * 固定 557 B、无长度/flags 字段（上行单帧长，事实卡 §1/§6.1）。帧内容
 * 语义（解交织/RS）不属 codec——两侧共用的是这里的字节布局合同。 */
#define ADSB_LINK_UAT_FRAME_BYTES   552
#define ADSB_LINK_UAT_META_BYTES    5      /* rssi(1) + rp_ts_us(4) */
#define ADSB_LINK_UAT_PAYLOAD_LEN \
    (ADSB_LINK_UAT_META_BYTES + ADSB_LINK_UAT_FRAME_BYTES)   /* 557 */

uint16_t adsb_link_crc16(const uint8_t *data, size_t len);   /* CCITT-FALSE */

/* 返回帧总长；cap 不足或 payload_len 超限返回 0。 */
size_t adsb_link_encode(uint8_t *out, size_t cap, uint8_t msg_type,
                        uint8_t seq, const uint8_t *payload,
                        size_t payload_len);

/* 组 UAT_UPLINK payload 到 out（cap ≥ ADSB_LINK_UAT_PAYLOAD_LEN）。
 * 返回 payload 长度 557；cap 不足返回 0（与 adsb_link_encode 同款约定）。
 * rssi 单位 0.5 dB、0xFF=无值；rp_ts_us 模 2^32 单调 µs（规范 §6.1）。 */
size_t adsb_link_uat_uplink_encode(uint8_t *out, size_t cap, uint8_t rssi,
                                   uint32_t rp_ts_us,
                                   const uint8_t frame[ADSB_LINK_UAT_FRAME_BYTES]);

/* 拆 UAT_UPLINK payload。len ≠ 557 返回 false 且不写任何输出（失败不
 * 改写输出，与 uat_decode 纯单元契约同款）；*frame 指向 payload 内部
 * 偏移 5，随调用方缓冲存活。 */
bool adsb_link_uat_uplink_decode(const uint8_t *payload, size_t len,
                                 uint8_t *rssi, uint32_t *rp_ts_us,
                                 const uint8_t **frame);

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
