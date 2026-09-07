/*
 * rp_cc13xx_codec.h — RP2040↔CC1312R Sub-GHz 链路 v1 的共享编解码器接口。
 *
 * 规范：firmware/PROTOCOL_RP2040_CC1312R_SPI.md（v1.0）。与 adsb_link_codec
 * 同模式：两侧固件编译同一份实现（RP2040: rp2040/CMakeLists.txt 直接引用；
 * CC1312R 侧同源复制），协议永不漂移。纯 C99，无平台依赖、无 malloc、
 * 无 FreeRTOS；host 测试直编（firmware/test/test_rp_cc13xx_codec.c）。
 *
 * 事务模型（规范 §2.1/§2.2 延迟应答）：一次事务 = CSN 低电平期间的定长
 * 512 B 交换；事务 N 的 MISO 携带事务 N−1 命令的应答（slave 单 pending 槽，
 * 空槽 = 全 0x00『无帧』）。本 codec 只产出/解析"帧本体"（≤512 B，帧尾之后
 * 的填充由 SPI 驱动补齐、接收方丢弃），不管理 CSN/IRQ/pending 装载时序
 * ——那是 WP-E 数据链路的职责。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* 帧格式常量（规范 §3，小端多字节） */
#define RP_CC13XX_MAGIC0        0x50   /* 'P'（§3，沿用 P0a 家族） */
#define RP_CC13XX_MAGIC1        0x4B   /* 'K' */
#define RP_CC13XX_VER           1      /* §3/§3.3：ver 仅 1 B major */
#define RP_CC13XX_HDR_LEN       8
#define RP_CC13XX_MAX_PAYLOAD   502    /* §3.1：payload 上限 */
#define RP_CC13XX_MAX_FRAME     (RP_CC13XX_HDR_LEN + RP_CC13XX_MAX_PAYLOAD + 2)
                                       /* = 512 = 事务长度（§2.1） */

/* msg_type（v1，规范 §4 表；0x00 禁用、0xFF 永不分配，其余值保留） */
#define RP_CC13XX_MSG_HELLO              0x01
#define RP_CC13XX_MSG_IRQ_ACK            0x02
#define RP_CC13XX_MSG_PING               0x03
#define RP_CC13XX_MSG_PONG               0x04
#define RP_CC13XX_MSG_RX_DESCRIPTOR      0x10
#define RP_CC13XX_MSG_RX_PAYLOAD_CHUNK   0x11
#define RP_CC13XX_MSG_QUEUE_FULL         0x12
#define RP_CC13XX_MSG_RF_CONFIG          0x20
#define RP_CC13XX_MSG_RF_CONFIG_STATUS   0x21
#define RP_CC13XX_MSG_RESET_STATUS_REQ   0x22
#define RP_CC13XX_MSG_RESET_STATUS       0x23
#define RP_CC13XX_MSG_UPGRADE_STATUS_REQ 0x24
#define RP_CC13XX_MSG_UPGRADE_STATUS     0x25
#define RP_CC13XX_MSG_ERROR              0x7F

/* payload 布局常量（规范 §4.x——每条均为 codec 断言的直接引用对象） */
#define RP_CC13XX_HELLO_LEN          8    /* §4.1 */
#define RP_CC13XX_RX_DESCRIPTOR_LEN  14   /* §4.3 */
#define RP_CC13XX_CHUNK_HDR_LEN      6    /* §4.4：desc_id+offset+total_len */
#define RP_CC13XX_CHUNK_MAX_DATA     496  /* §4.4：= 502 − 6 */
#define RP_CC13XX_QUEUE_FULL_LEN     4    /* §4.5 */
#define RP_CC13XX_RF_CONFIG_LEN      20   /* §4.6 */
#define RP_CC13XX_RESET_STATUS_LEN   8    /* §4.7 */
#define RP_CC13XX_UPGRADE_STATUS_LEN 8    /* §4.8 */
#define RP_CC13XX_ERROR_MSG_MAX      32   /* §4.9 */
#define RP_CC13XX_MAX_MSG_LEN        4096 /* §4.3：报文总长上限（超限截断） */

/* reset_reason 位图（§4.1；保留位发送方必须置 0，接收方不解释未定义位） */
#define RP_CC13XX_RESET_POR      0x1  /* bit0：上电复位 */
#define RP_CC13XX_RESET_RESETN   0x2  /* bit1：RESET_N 引脚复位 */
#define RP_CC13XX_RESET_SOFTWARE 0x4  /* bit2：软件复位 */
#define RP_CC13XX_RESET_WDT      0x8  /* bit3：看门狗/时钟安全复位 */

/* RX_DESCRIPTOR flags（§4.3；bit2 以上保留必须为 0，接收方按 len 违规拒收） */
#define RP_CC13XX_DESC_F_HIGH_PRIO  0x01  /* bit0：高优先级 */
#define RP_CC13XX_DESC_F_TRUNCATED  0x02  /* bit1：超 4096 被截断 */

/* UPGRADE_STATUS.state（§4.8） */
#define RP_CC13XX_UPG_NORMAL      0x00
#define RP_CC13XX_UPG_BOOTLOADER  0x01
#define RP_CC13XX_UPG_UPGRADING   0x02
#define RP_CC13XX_UPG_FAILED      0x03

/*
 * 解码结果（与规范 §5.3 计数器集一一对应；事务级计数由调用方按返回值累加）：
 *   OK            帧合法且类型已知，*out 已填充
 *   NO_FRAME      整缓冲全 0x00 = 合法『无帧』（§2.3 规则 5），不计任何错误
 *   UNKNOWN_TYPE  CRC 合法但类型未知：容忍忽略（§3.3/§5.4），计 unknown_types；
 *                 *out 仍按原样填充供诊断，忽略与否由调用方决定
 *   ERR_ARG       入参非法（NULL / buflen > 512）——编程错误，不计协议计数
 *   ERR_SHORT     尚不足一个完整帧（§2.1 半事务），等待补齐，不计错
 *   ERR_MAGIC     magic 不符且缓冲含非 0 字节（§5.2），计 resyncs
 *   ERR_LEN       payload_len > 502（§3.1）或已知类型 payload 违规
 *                 （§4.5/§4.8/§4.9/§4.3 保留位与结构约束），计 len_errors
 *   ERR_CRC       CRC 不符（§5.1：CRC 先于 ver），计 crc_errors
 *   ERR_VERSION   ver ≠ 1（§6.2），计 version_mismatch，永不进入 LINKED
 */
typedef enum {
    RP_CC13XX_OK = 0,
    RP_CC13XX_NO_FRAME,
    RP_CC13XX_UNKNOWN_TYPE,
    RP_CC13XX_ERR_ARG,
    RP_CC13XX_ERR_SHORT,
    RP_CC13XX_ERR_MAGIC,
    RP_CC13XX_ERR_LEN,
    RP_CC13XX_ERR_CRC,
    RP_CC13XX_ERR_VERSION,
} rp_cc13xx_status_t;

/* CRC-16/CCITT-FALSE（§3.2）。实现唯一权威是 adsb_link_crc16
 * （adsb_link_codec.c:4-13），本组件直接复用、不另立参数。 */
uint16_t rp_cc13xx_crc16(const uint8_t *data, size_t len);

/* 通用帧编码（§3）：返回帧总长；payload 超限或 cap 不足返回 0。
 * 帧尾补 0x00 至 512 B 是 SPI 驱动的事（§2.1），不在本函数范围。 */
size_t rp_cc13xx_encode(uint8_t *out, size_t cap, uint8_t msg_type,
                        uint16_t seq, const void *payload, size_t payload_len);

/* 解出的通用消息（payload 原样携带，按 type 交给 decode_xxx 解析） */
typedef struct {
    uint8_t  type;
    uint16_t seq;
    uint16_t payload_len;
    uint8_t  payload[RP_CC13XX_MAX_PAYLOAD];
} rp_cc13xx_msg_t;

/* 帧级解码（§5.1 校验顺序：magic → len → CRC → ver → type 分发）。
 * buf/buflen 为整事务缓冲（buflen ≤ 512，§2.1）；帧尾之后的填充字节
 * 按规范丢弃、不校验。返回值语义见 rp_cc13xx_status_t 注释。 */
rp_cc13xx_status_t rp_cc13xx_decode_frame(const uint8_t *buf, size_t buflen,
                                          rp_cc13xx_msg_t *out);

/* seq 判定（§3.4/§5.5）：返回 1 = gap（(u16)(now − prev) != 1）。
 * 0xFFFF→0x0000 回绕是正常递增，不是 gap；会话基线重置（§6.5）由调用方
 * 在收到合法 HELLO 时自行重置基线、跳过一次判定。 */
int rp_cc13xx_seq_gap(uint16_t prev_seq, uint16_t now_seq);

/* ── 每条 v1 消息一对 encode/decode（payload 布局的 codec 断言，§4.x）────
 * encode_* 返回帧总长，0 = 参数/超限错误（发送方约束在此把守：保留位
 * 一律写 0，data/msg 长度超限拒绝）。decode_* 输入已解出的 msg，返回
 * OK 或 ERR_LEN（payload 长度/结构/保留位违规），可独立于 decode_frame 使用。 */

/* 空载荷命令共用：IRQ_ACK/PING/PONG/RESET_STATUS_REQ/UPGRADE_STATUS_REQ
 * （§4.2 与 §4 表）。msg_type 不属此五者返回 0（0x00 禁用也不可发）。 */
size_t rp_cc13xx_encode_empty(uint8_t *out, size_t cap, uint8_t msg_type,
                              uint16_t seq);

/* §4.1 HELLO */
typedef struct {
    uint16_t fw_ver_major;
    uint16_t fw_ver_minor;
    uint32_t reset_reason;      /* RP_CC13XX_RESET_* 位图 */
} rp_cc13xx_hello_t;
size_t rp_cc13xx_encode_hello(uint8_t *out, size_t cap, uint16_t seq,
                              const rp_cc13xx_hello_t *h);
rp_cc13xx_status_t rp_cc13xx_decode_hello(const rp_cc13xx_msg_t *m,
                                          rp_cc13xx_hello_t *h);

/* §4.3 RX_DESCRIPTOR */
typedef struct {
    uint16_t desc_id;           /* slave 侧单调递增，关联其分片 */
    uint32_t freq_hz;           /* 接收频率 Hz */
    uint32_t ts_us;             /* 模 2^32 单调 µs，回绕不是回跳 */
    uint16_t total_len;         /* ≤ 4096，超限被 slave 截断置 flags bit1 */
    uint8_t  rssi;              /* 0.5 dB/LSB 无符号，0xFF=无值 */
    uint8_t  flags;             /* RP_CC13XX_DESC_F_*，保留位必须为 0 */
} rp_cc13xx_rx_desc_t;
size_t rp_cc13xx_encode_rx_descriptor(uint8_t *out, size_t cap, uint16_t seq,
                                      const rp_cc13xx_rx_desc_t *d);
rp_cc13xx_status_t rp_cc13xx_decode_rx_descriptor(const rp_cc13xx_msg_t *m,
                                                  rp_cc13xx_rx_desc_t *d);

/* §4.4 RX_PAYLOAD_CHUNK */
typedef struct {
    uint16_t desc_id;
    uint16_t offset;            /* 本片在报文内的字节偏移 */
    uint16_t total_len;         /* 必须与对应 RX_DESCRIPTOR 一致 */
    uint16_t data_len;          /* = payload_len − 6，≤ 496 */
    uint8_t  data[RP_CC13XX_CHUNK_MAX_DATA];
} rp_cc13xx_chunk_t;
size_t rp_cc13xx_encode_rx_chunk(uint8_t *out, size_t cap, uint16_t seq,
                                 const rp_cc13xx_chunk_t *c);
rp_cc13xx_status_t rp_cc13xx_decode_rx_chunk(const rp_cc13xx_msg_t *m,
                                             rp_cc13xx_chunk_t *c);

/* §4.5 QUEUE_FULL（reserved 字段发送方写 0，接收方非 0 计 len 违规） */
typedef struct {
    uint16_t events_dropped;    /* 开机累计丢弃，模 2^16 单调 */
    uint8_t  queue_depth;       /* 当前队列占用（观测值） */
} rp_cc13xx_queue_full_t;
size_t rp_cc13xx_encode_queue_full(uint8_t *out, size_t cap, uint16_t seq,
                                   const rp_cc13xx_queue_full_t *q);
rp_cc13xx_status_t rp_cc13xx_decode_queue_full(const rp_cc13xx_msg_t *m,
                                               rp_cc13xx_queue_full_t *q);

/* §4.6 RF_CONFIG / RF_CONFIG_STATUS（同一 payload 布局，msg_type 二选一） */
typedef struct {
    uint32_t config_version;    /* 单调递增，0 = 无配置；0 的 0x20 为只读查询 */
    uint8_t  digest[16];        /* 配置映像 SHA-256 前 16 B，仅对账用 */
} rp_cc13xx_rf_config_t;
size_t rp_cc13xx_encode_rf_config(uint8_t *out, size_t cap, uint8_t msg_type,
                                  uint16_t seq,
                                  const rp_cc13xx_rf_config_t *c);
rp_cc13xx_status_t rp_cc13xx_decode_rf_config(const rp_cc13xx_msg_t *m,
                                              rp_cc13xx_rf_config_t *c);

/* §4.7 RESET_STATUS */
typedef struct {
    uint32_t reset_reason;      /* 位图同 §4.1 */
    uint32_t uptime_ms;         /* 开机累计毫秒，模 2^32 */
} rp_cc13xx_reset_status_t;
size_t rp_cc13xx_encode_reset_status(uint8_t *out, size_t cap, uint16_t seq,
                                     const rp_cc13xx_reset_status_t *s);
rp_cc13xx_status_t rp_cc13xx_decode_reset_status(const rp_cc13xx_msg_t *m,
                                                 rp_cc13xx_reset_status_t *s);

/* §4.8 UPGRADE_STATUS（reserved[3] 发送方写 0，接收方非 0 计 len 违规） */
typedef struct {
    uint8_t  state;             /* RP_CC13XX_UPG_* */
    uint32_t running_image_version;
} rp_cc13xx_upgrade_status_t;
size_t rp_cc13xx_encode_upgrade_status(uint8_t *out, size_t cap, uint16_t seq,
                                       const rp_cc13xx_upgrade_status_t *u);
rp_cc13xx_status_t rp_cc13xx_decode_upgrade_status(const rp_cc13xx_msg_t *m,
                                                   rp_cc13xx_upgrade_status_t *u);

/* §4.9 ERROR（msg 为 UTF-8 诊断串，非 NUL 结尾合法，以 len 为准） */
typedef struct {
    uint8_t  code;              /* 0x01..0x05，≥0x06 保留 */
    uint8_t  msg_len;           /* ≤ 32 */
    uint8_t  msg[RP_CC13XX_ERROR_MSG_MAX];
} rp_cc13xx_error_t;
size_t rp_cc13xx_encode_error(uint8_t *out, size_t cap, uint16_t seq,
                              const rp_cc13xx_error_t *e);
rp_cc13xx_status_t rp_cc13xx_decode_error(const rp_cc13xx_msg_t *m,
                                          rp_cc13xx_error_t *e);

/* ── master 侧分片重组（§4.4/§7.1）+ §6.5 re-HELLO 清态 ──────────────────
 * RP2040 按 descriptor → N×chunk 顺序取毕一个报文再取下一事件；
 * 静态容量、无 malloc。WP-E 数据链路接本原语（Task 3 无调用点）。 */
typedef struct {
    uint16_t desc_id;
    uint16_t total_len;
    uint16_t have;              /* 已积累字节数；have == total_len 即完整 */
    uint8_t  truncated;         /* 透传 descriptor flags bit1（§4.3） */
    uint8_t  active;
    uint8_t  data[RP_CC13XX_MAX_MSG_LEN];
} rp_cc13xx_reasm_t;

void rp_cc13xx_reasm_init(rp_cc13xx_reasm_t *r);

/* §6.5：收到合法 HELLO 即清理半交付态——已交付 descriptor 及已积累分片
 * 一并作废、不重新入队（对端重启即放弃该报文），后续旧分片不再命中。 */
void rp_cc13xx_reasm_on_hello(rp_cc13xx_reasm_t *r);

/* 以 RX_DESCRIPTOR 开启一个报文的重组；total_len > 4096 → ERR_LEN。 */
rp_cc13xx_status_t rp_cc13xx_reasm_start(rp_cc13xx_reasm_t *r,
                                         const rp_cc13xx_rx_desc_t *d);

/* 积累一片：非 active → ERR_ARG（调用序错误）；desc_id/total_len 不一致、
 * offset 未按 have 递进、data 溢出 → ERR_LEN（§4.4 约束）。 */
rp_cc13xx_status_t rp_cc13xx_reasm_feed(rp_cc13xx_reasm_t *r,
                                        const rp_cc13xx_chunk_t *c);
