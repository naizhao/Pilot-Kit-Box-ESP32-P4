/*
 * rp_cc13xx_codec.c — RP2040↔CC1312R Sub-GHz 链路 v1 共享编解码器实现。
 *
 * 判据来源：firmware/PROTOCOL_RP2040_CC1312R_SPI.md（v1.0）§3/§4/§5。
 * CRC-16/CCITT-FALSE 直接复用 adsb_link_crc16（adsb_link_codec.c:4-13，
 * 规范 §3.2 指定的实现唯一权威，"一仓一族 CRC"）——不复制、不另立参数，
 * 由编译期链接保证本链路与 P4 UART 链路的 CRC 永不漂移。
 */
#include <string.h>
#include "rp_cc13xx_codec.h"
#include "adsb_link.h"

uint16_t rp_cc13xx_crc16(const uint8_t *data, size_t len)
{
    return adsb_link_crc16(data, len);   /* §3.2：唯一权威实现 */
}

/* 小端读写辅助（§3：帧内多字节字段一律小端） */
static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

size_t rp_cc13xx_encode(uint8_t *out, size_t cap, uint8_t msg_type,
                        uint16_t seq, const void *payload, size_t payload_len)
{
    if (!out || (payload_len && !payload)) return 0;
    if (payload_len > RP_CC13XX_MAX_PAYLOAD) return 0;      /* §3.1 */
    size_t total = RP_CC13XX_HDR_LEN + payload_len + 2;
    if (cap < total) return 0;

    out[0] = RP_CC13XX_MAGIC0;
    out[1] = RP_CC13XX_MAGIC1;
    out[2] = RP_CC13XX_VER;
    out[3] = msg_type;
    wr_le16(out + 4, seq);
    wr_le16(out + 6, (uint16_t)payload_len);
    if (payload_len) memcpy(out + RP_CC13XX_HDR_LEN, payload, payload_len);
    wr_le16(out + total - 2, rp_cc13xx_crc16(out, total - 2));
    return total;
}

int rp_cc13xx_seq_gap(uint16_t prev_seq, uint16_t now_seq)
{
    return (uint16_t)(now_seq - prev_seq) != 1;   /* §3.4：无符号差值 */
}

/* 已知类型的 payload 结构校验（§4.x；违规与 len 违规同计，§4.8/§5.2）。
 * 保留位纪律：UPGRADE_STATUS.reserved / QUEUE_FULL.reserved /
 * RX_DESCRIPTOR.flags 保留位"必须为 0"（§4.8/§4.5/§4.3），非 0 即
 * payload 违规计 len_errors；HELLO.reset_reason 例外——§4.1 明文
 * "接收方不解释未定义位"，故不做保留位检查。 */
static rp_cc13xx_status_t payload_check(uint8_t type, const uint8_t *p,
                                        uint16_t plen)
{
    switch (type) {
    case RP_CC13XX_MSG_HELLO:
        return plen == RP_CC13XX_HELLO_LEN ? RP_CC13XX_OK : RP_CC13XX_ERR_LEN;
    case RP_CC13XX_MSG_IRQ_ACK:
    case RP_CC13XX_MSG_PING:
    case RP_CC13XX_MSG_PONG:
    case RP_CC13XX_MSG_RESET_STATUS_REQ:
    case RP_CC13XX_MSG_UPGRADE_STATUS_REQ:
        return plen == 0 ? RP_CC13XX_OK : RP_CC13XX_ERR_LEN;
    case RP_CC13XX_MSG_RX_DESCRIPTOR:
        /* §4.3：len=14、保留位 0、total_len ≤ 4096（编码/解码两侧强制，
         * 超限即 len 违规——线上不存在 4097+ 的合法描述符）。 */
        return (plen == RP_CC13XX_RX_DESCRIPTOR_LEN && (p[13] & 0xFC) == 0 &&
                rd_le16(p + 10) <= RP_CC13XX_MAX_MSG_LEN)
               ? RP_CC13XX_OK : RP_CC13XX_ERR_LEN;
    case RP_CC13XX_MSG_RX_PAYLOAD_CHUNK:
        /* data 长度 = plen − 6 ∈ [1, 496]（§4.4：空分片拒绝——无推进的
         * 合法片会令重组流水停滞） */
        return (plen > RP_CC13XX_CHUNK_HDR_LEN &&
                plen <= RP_CC13XX_MAX_PAYLOAD)
               ? RP_CC13XX_OK : RP_CC13XX_ERR_LEN;
    case RP_CC13XX_MSG_QUEUE_FULL:
        return (plen == RP_CC13XX_QUEUE_FULL_LEN && p[3] == 0)
               ? RP_CC13XX_OK : RP_CC13XX_ERR_LEN;
    case RP_CC13XX_MSG_RF_CONFIG:
    case RP_CC13XX_MSG_RF_CONFIG_STATUS:
        return plen == RP_CC13XX_RF_CONFIG_LEN
               ? RP_CC13XX_OK : RP_CC13XX_ERR_LEN;
    case RP_CC13XX_MSG_RESET_STATUS:
        return plen == RP_CC13XX_RESET_STATUS_LEN
               ? RP_CC13XX_OK : RP_CC13XX_ERR_LEN;
    case RP_CC13XX_MSG_UPGRADE_STATUS:
        return (plen == RP_CC13XX_UPGRADE_STATUS_LEN &&
                p[1] == 0 && p[2] == 0 && p[3] == 0)
               ? RP_CC13XX_OK : RP_CC13XX_ERR_LEN;   /* §4.8 */
    case RP_CC13XX_MSG_ERROR:
        return (plen >= 2 && plen <= 2 + RP_CC13XX_ERROR_MSG_MAX &&
                p[1] == plen - 2)
               ? RP_CC13XX_OK : RP_CC13XX_ERR_LEN;   /* §4.9：len 域定界 */
    default:
        return RP_CC13XX_OK;   /* 未知类型由调用方分发，不在此校验 */
    }
}

rp_cc13xx_status_t rp_cc13xx_decode_frame(const uint8_t *buf, size_t buflen,
                                          rp_cc13xx_msg_t *out)
{
    if (!buf || !out || buflen > RP_CC13XX_MAX_FRAME) return RP_CC13XX_ERR_ARG;
    if (buflen < RP_CC13XX_HDR_LEN + 2) return RP_CC13XX_ERR_SHORT;

    if (buf[0] != RP_CC13XX_MAGIC0 || buf[1] != RP_CC13XX_MAGIC1) {
        /* §2.3 规则 5：magic 不符且整缓冲全 0x00 = 合法『无帧』；
         * 含非 0 字节才计 resyncs（§5.2）。 */
        for (size_t i = 0; i < buflen; i++)
            if (buf[i]) return RP_CC13XX_ERR_MAGIC;
        return RP_CC13XX_NO_FRAME;
    }

    /* §5.1 校验顺序：magic → len → CRC → ver → type 分发。 */
    uint16_t plen = rd_le16(buf + 6);
    if (plen > RP_CC13XX_MAX_PAYLOAD) return RP_CC13XX_ERR_LEN;   /* §3.1 */
    size_t total = RP_CC13XX_HDR_LEN + plen + 2;
    if (buflen < total) return RP_CC13XX_ERR_SHORT;

    uint16_t crc_want = rd_le16(buf + total - 2);
    if (crc_want != rp_cc13xx_crc16(buf, total - 2))
        return RP_CC13XX_ERR_CRC;
    if (buf[2] != RP_CC13XX_VER) return RP_CC13XX_ERR_VERSION;   /* §6.2 */

    uint8_t type = buf[3];
    if (type != RP_CC13XX_MSG_HELLO && type != RP_CC13XX_MSG_IRQ_ACK &&
        type != RP_CC13XX_MSG_PING && type != RP_CC13XX_MSG_PONG &&
        type != RP_CC13XX_MSG_RX_DESCRIPTOR &&
        type != RP_CC13XX_MSG_RX_PAYLOAD_CHUNK &&
        type != RP_CC13XX_MSG_QUEUE_FULL &&
        type != RP_CC13XX_MSG_RF_CONFIG &&
        type != RP_CC13XX_MSG_RF_CONFIG_STATUS &&
        type != RP_CC13XX_MSG_RESET_STATUS_REQ &&
        type != RP_CC13XX_MSG_RESET_STATUS &&
        type != RP_CC13XX_MSG_UPGRADE_STATUS_REQ &&
        type != RP_CC13XX_MSG_UPGRADE_STATUS &&
        type != RP_CC13XX_MSG_ERROR) {
        /* §3.3/§5.4：容忍忽略、计 unknown_types；消息仍原样填给调用方
         * 供诊断呈现（忽略与否由调用方决定，codec 不改变其状态）。
         * 0x00/0xFF 同走此路："全 0 填充不得解析为帧"由 magic 检查的
         * 全 0 短路（NO_FRAME）保证；带合法 magic+CRC 的 0x00/0xFF 帧属
         * 伪造/悬空态，按未知类型计数忽略即不改变状态（§4 表注）。 */
        out->type = type;
        out->seq = rd_le16(buf + 4);
        out->payload_len = plen;
        if (plen) memcpy(out->payload, buf + RP_CC13XX_HDR_LEN, plen);
        return RP_CC13XX_UNKNOWN_TYPE;
    }

    rp_cc13xx_status_t st = payload_check(type, buf + RP_CC13XX_HDR_LEN, plen);
    if (st != RP_CC13XX_OK) return st;

    out->type = type;
    out->seq = rd_le16(buf + 4);
    out->payload_len = plen;
    if (plen) memcpy(out->payload, buf + RP_CC13XX_HDR_LEN, plen);
    return RP_CC13XX_OK;
}

/* ── 空载荷命令（§4.2 与 §4 表）────────────────────────────────────────── */

static int is_empty_type(uint8_t t)
{
    return t == RP_CC13XX_MSG_IRQ_ACK || t == RP_CC13XX_MSG_PING ||
           t == RP_CC13XX_MSG_PONG || t == RP_CC13XX_MSG_RESET_STATUS_REQ ||
           t == RP_CC13XX_MSG_UPGRADE_STATUS_REQ;
}

size_t rp_cc13xx_encode_empty(uint8_t *out, size_t cap, uint8_t msg_type,
                              uint16_t seq)
{
    if (!is_empty_type(msg_type)) return 0;   /* 0x00 禁用亦拒 */
    return rp_cc13xx_encode(out, cap, msg_type, seq, NULL, 0);
}

/* ── §4.1 HELLO ─────────────────────────────────────────────────────────── */

size_t rp_cc13xx_encode_hello(uint8_t *out, size_t cap, uint16_t seq,
                              const rp_cc13xx_hello_t *h)
{
    if (!h) return 0;
    uint8_t pl[RP_CC13XX_HELLO_LEN];
    wr_le16(pl, h->fw_ver_major);
    wr_le16(pl + 2, h->fw_ver_minor);
    /* §4.1：发送方在编码边界把 reset_reason 掩码到已定义 bit0–3——复位
     * 原因源可能带厂商/平台位，掩码即线上合同；接收方保持容忍（不解释
     * 未定义位），故对端无须预处理。 */
    wr_le32(pl + 4, h->reset_reason & 0x0F);
    return rp_cc13xx_encode(out, cap, RP_CC13XX_MSG_HELLO, seq, pl, sizeof pl);
}

rp_cc13xx_status_t rp_cc13xx_decode_hello(const rp_cc13xx_msg_t *m,
                                          rp_cc13xx_hello_t *h)
{
    if (!m || !h) return RP_CC13XX_ERR_ARG;
    if (m->payload_len != RP_CC13XX_HELLO_LEN) return RP_CC13XX_ERR_LEN;
    h->fw_ver_major = rd_le16(m->payload);
    h->fw_ver_minor = rd_le16(m->payload + 2);
    h->reset_reason = rd_le32(m->payload + 4);
    return RP_CC13XX_OK;
}

/* ── §4.3 RX_DESCRIPTOR ────────────────────────────────────────────────── */

size_t rp_cc13xx_encode_rx_descriptor(uint8_t *out, size_t cap, uint16_t seq,
                                      const rp_cc13xx_rx_desc_t *d)
{
    /* §4.3：保留位发送方置 0；total_len ≤ 4096 发送侧强制（P2-a：
     * 截断只发生在 slave 入队前并置 flags bit1，编码器不得透传超限值）。 */
    if (!d || (d->flags & 0xFC) || d->total_len > RP_CC13XX_MAX_MSG_LEN)
        return 0;
    uint8_t pl[RP_CC13XX_RX_DESCRIPTOR_LEN];
    wr_le16(pl, d->desc_id);
    wr_le32(pl + 2, d->freq_hz);
    wr_le32(pl + 6, d->ts_us);
    wr_le16(pl + 10, d->total_len);
    pl[12] = d->rssi;
    pl[13] = d->flags;
    return rp_cc13xx_encode(out, cap, RP_CC13XX_MSG_RX_DESCRIPTOR, seq,
                            pl, sizeof pl);
}

rp_cc13xx_status_t rp_cc13xx_decode_rx_descriptor(const rp_cc13xx_msg_t *m,
                                                  rp_cc13xx_rx_desc_t *d)
{
    if (!m || !d) return RP_CC13XX_ERR_ARG;
    if (m->payload_len != RP_CC13XX_RX_DESCRIPTOR_LEN || (m->payload[13] & 0xFC))
        return RP_CC13XX_ERR_LEN;
    if (rd_le16(m->payload + 10) > RP_CC13XX_MAX_MSG_LEN)
        return RP_CC13XX_ERR_LEN;   /* §4.3：total_len ≤ 4096 接收侧强制 */
    d->desc_id = rd_le16(m->payload);
    d->freq_hz = rd_le32(m->payload + 2);
    d->ts_us = rd_le32(m->payload + 6);
    d->total_len = rd_le16(m->payload + 10);
    d->rssi = m->payload[12];
    d->flags = m->payload[13];
    return RP_CC13XX_OK;
}

/* ── §4.4 RX_PAYLOAD_CHUNK ─────────────────────────────────────────────── */

size_t rp_cc13xx_encode_rx_chunk(uint8_t *out, size_t cap, uint16_t seq,
                                 const rp_cc13xx_chunk_t *c)
{
    /* §4.4：data_len ∈ [1, 496]——空分片发送侧拒绝（P1：无推进的合法片
     * 会令重组流水停滞） */
    if (!c || c->data_len == 0 || c->data_len > RP_CC13XX_CHUNK_MAX_DATA)
        return 0;
    uint8_t pl[RP_CC13XX_CHUNK_HDR_LEN + RP_CC13XX_CHUNK_MAX_DATA];
    wr_le16(pl, c->desc_id);
    wr_le16(pl + 2, c->offset);
    wr_le16(pl + 4, c->total_len);
    memcpy(pl + RP_CC13XX_CHUNK_HDR_LEN, c->data, c->data_len);
    return rp_cc13xx_encode(out, cap, RP_CC13XX_MSG_RX_PAYLOAD_CHUNK, seq,
                            pl, RP_CC13XX_CHUNK_HDR_LEN + c->data_len);
}

rp_cc13xx_status_t rp_cc13xx_decode_rx_chunk(const rp_cc13xx_msg_t *m,
                                             rp_cc13xx_chunk_t *c)
{
    if (!m || !c) return RP_CC13XX_ERR_ARG;
    if (m->payload_len <= RP_CC13XX_CHUNK_HDR_LEN ||   /* §4.4：data ≥ 1 */
        m->payload_len > RP_CC13XX_MAX_PAYLOAD)
        return RP_CC13XX_ERR_LEN;
    c->desc_id = rd_le16(m->payload);
    c->offset = rd_le16(m->payload + 2);
    c->total_len = rd_le16(m->payload + 4);
    c->data_len = (uint16_t)(m->payload_len - RP_CC13XX_CHUNK_HDR_LEN);
    memcpy(c->data, m->payload + RP_CC13XX_CHUNK_HDR_LEN, c->data_len);
    return RP_CC13XX_OK;
}

/* ── §4.5 QUEUE_FULL ───────────────────────────────────────────────────── */

size_t rp_cc13xx_encode_queue_full(uint8_t *out, size_t cap, uint16_t seq,
                                   const rp_cc13xx_queue_full_t *q)
{
    if (!q) return 0;
    uint8_t pl[RP_CC13XX_QUEUE_FULL_LEN];
    wr_le16(pl, q->events_dropped);
    pl[2] = q->queue_depth;
    pl[3] = 0;   /* reserved 必须为 0（§4.5） */
    return rp_cc13xx_encode(out, cap, RP_CC13XX_MSG_QUEUE_FULL, seq,
                            pl, sizeof pl);
}

rp_cc13xx_status_t rp_cc13xx_decode_queue_full(const rp_cc13xx_msg_t *m,
                                               rp_cc13xx_queue_full_t *q)
{
    if (!m || !q) return RP_CC13XX_ERR_ARG;
    if (m->payload_len != RP_CC13XX_QUEUE_FULL_LEN || m->payload[3] != 0)
        return RP_CC13XX_ERR_LEN;
    q->events_dropped = rd_le16(m->payload);
    q->queue_depth = m->payload[2];
    return RP_CC13XX_OK;
}

/* ── §4.6 RF_CONFIG / RF_CONFIG_STATUS ─────────────────────────────────── */

size_t rp_cc13xx_encode_rf_config(uint8_t *out, size_t cap, uint8_t msg_type,
                                  uint16_t seq,
                                  const rp_cc13xx_rf_config_t *c)
{
    if (!c || (msg_type != RP_CC13XX_MSG_RF_CONFIG &&
               msg_type != RP_CC13XX_MSG_RF_CONFIG_STATUS))
        return 0;
    uint8_t pl[RP_CC13XX_RF_CONFIG_LEN];
    wr_le32(pl, c->config_version);
    memcpy(pl + 4, c->digest, 16);
    return rp_cc13xx_encode(out, cap, msg_type, seq, pl, sizeof pl);
}

rp_cc13xx_status_t rp_cc13xx_decode_rf_config(const rp_cc13xx_msg_t *m,
                                              rp_cc13xx_rf_config_t *c)
{
    if (!m || !c) return RP_CC13XX_ERR_ARG;
    if (m->payload_len != RP_CC13XX_RF_CONFIG_LEN) return RP_CC13XX_ERR_LEN;
    c->config_version = rd_le32(m->payload);
    memcpy(c->digest, m->payload + 4, 16);
    return RP_CC13XX_OK;
}

/* ── §4.7 RESET_STATUS ─────────────────────────────────────────────────── */

size_t rp_cc13xx_encode_reset_status(uint8_t *out, size_t cap, uint16_t seq,
                                     const rp_cc13xx_reset_status_t *s)
{
    if (!s) return 0;
    uint8_t pl[RP_CC13XX_RESET_STATUS_LEN];
    wr_le32(pl, s->reset_reason & 0x0F);   /* §4.1：发送边界掩码，同 HELLO */
    wr_le32(pl + 4, s->uptime_ms);
    return rp_cc13xx_encode(out, cap, RP_CC13XX_MSG_RESET_STATUS, seq,
                            pl, sizeof pl);
}

rp_cc13xx_status_t rp_cc13xx_decode_reset_status(const rp_cc13xx_msg_t *m,
                                                 rp_cc13xx_reset_status_t *s)
{
    if (!m || !s) return RP_CC13XX_ERR_ARG;
    if (m->payload_len != RP_CC13XX_RESET_STATUS_LEN) return RP_CC13XX_ERR_LEN;
    s->reset_reason = rd_le32(m->payload);
    s->uptime_ms = rd_le32(m->payload + 4);
    return RP_CC13XX_OK;
}

/* ── §4.8 UPGRADE_STATUS ───────────────────────────────────────────────── */

size_t rp_cc13xx_encode_upgrade_status(uint8_t *out, size_t cap, uint16_t seq,
                                       const rp_cc13xx_upgrade_status_t *u)
{
    if (!u) return 0;
    uint8_t pl[RP_CC13XX_UPGRADE_STATUS_LEN];
    pl[0] = u->state;
    pl[1] = 0;   /* reserved[3] 发送方置 0（§4.8） */
    pl[2] = 0;
    pl[3] = 0;
    wr_le32(pl + 4, u->running_image_version);
    return rp_cc13xx_encode(out, cap, RP_CC13XX_MSG_UPGRADE_STATUS, seq,
                            pl, sizeof pl);
}

rp_cc13xx_status_t rp_cc13xx_decode_upgrade_status(const rp_cc13xx_msg_t *m,
                                                   rp_cc13xx_upgrade_status_t *u)
{
    if (!m || !u) return RP_CC13XX_ERR_ARG;
    if (m->payload_len != RP_CC13XX_UPGRADE_STATUS_LEN ||
        m->payload[1] || m->payload[2] || m->payload[3])
        return RP_CC13XX_ERR_LEN;   /* §4.8：reserved 非 0 计 len 违规 */
    u->state = m->payload[0];
    u->running_image_version = rd_le32(m->payload + 4);
    return RP_CC13XX_OK;
}

/* ── §4.9 ERROR ────────────────────────────────────────────────────────── */

size_t rp_cc13xx_encode_error(uint8_t *out, size_t cap, uint16_t seq,
                              const rp_cc13xx_error_t *e)
{
    if (!e || e->msg_len > RP_CC13XX_ERROR_MSG_MAX) return 0;   /* §4.9 */
    uint8_t pl[2 + RP_CC13XX_ERROR_MSG_MAX];
    pl[0] = e->code;
    pl[1] = e->msg_len;
    memcpy(pl + 2, e->msg, e->msg_len);
    return rp_cc13xx_encode(out, cap, RP_CC13XX_MSG_ERROR, seq, pl,
                            (size_t)2 + e->msg_len);
}

rp_cc13xx_status_t rp_cc13xx_decode_error(const rp_cc13xx_msg_t *m,
                                          rp_cc13xx_error_t *e)
{
    if (!m || !e) return RP_CC13XX_ERR_ARG;
    if (m->payload_len < 2 ||
        m->payload_len > 2 + RP_CC13XX_ERROR_MSG_MAX ||
        m->payload[1] != m->payload_len - 2)
        return RP_CC13XX_ERR_LEN;
    e->code = m->payload[0];
    e->msg_len = m->payload[1];
    memcpy(e->msg, m->payload + 2, e->msg_len);
    return RP_CC13XX_OK;
}

/* ── master 侧分片重组（§4.4/§7.1）+ §6.5 re-HELLO 清态 ────────────────── */

void rp_cc13xx_reasm_init(rp_cc13xx_reasm_t *r)
{
    memset(r, 0, sizeof(*r));
}

void rp_cc13xx_reasm_on_hello(rp_cc13xx_reasm_t *r)
{
    /* §6.5：合法 HELLO 即作废半交付态（descriptor + 已积累分片），
     * 后续旧分片不再命中；事件队列与 RF 配置不在本原语范围。
     * data[] 内容不必清零——active=0 使其不可达，下次 start 重新覆盖。 */
    r->active = 0;
    r->have = 0;
    r->desc_id = 0;
    r->total_len = 0;
    r->truncated = 0;
}

rp_cc13xx_status_t rp_cc13xx_reasm_start(rp_cc13xx_reasm_t *r,
                                         const rp_cc13xx_rx_desc_t *d)
{
    if (!r || !d) return RP_CC13XX_ERR_ARG;
    if (d->total_len > RP_CC13XX_MAX_MSG_LEN) return RP_CC13XX_ERR_LEN;
    r->desc_id = d->desc_id;
    r->total_len = d->total_len;
    r->have = 0;
    r->truncated = (d->flags & RP_CC13XX_DESC_F_TRUNCATED) ? 1 : 0;
    r->active = 1;
    return RP_CC13XX_OK;
}

rp_cc13xx_status_t rp_cc13xx_reasm_feed(rp_cc13xx_reasm_t *r,
                                        const rp_cc13xx_chunk_t *c)
{
    if (!r || !c) return RP_CC13XX_ERR_ARG;
    if (!r->active) return RP_CC13XX_ERR_ARG;   /* §6.5 清态后旧分片不命中 */
    if (c->data_len == 0 || c->data_len > RP_CC13XX_CHUNK_MAX_DATA)
        return RP_CC13XX_ERR_LEN;   /* §4.4：空片停滞防御；data_len > 496 会
                                     * 越界读调用方 data[496] 数组（先于
                                     * memcpy 拒绝——审计 P1-c ASan 复现项） */
    if (c->desc_id != r->desc_id || c->total_len != r->total_len ||
        c->offset != r->have ||
        (uint32_t)c->offset + c->data_len > r->total_len)
        return RP_CC13XX_ERR_LEN;   /* §4.4 约束 */
    memcpy(r->data + c->offset, c->data, c->data_len);
    r->have = (uint16_t)(r->have + c->data_len);
    return RP_CC13XX_OK;
}
