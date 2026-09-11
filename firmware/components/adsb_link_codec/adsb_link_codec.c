#include <string.h>
#include "adsb_link.h"

uint16_t adsb_link_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}

size_t adsb_link_encode(uint8_t *out, size_t cap, uint8_t msg_type,
                        uint8_t seq, const uint8_t *payload,
                        size_t payload_len)
{
    if (payload_len > ADSB_LINK_MAX_PAYLOAD) return 0;
    size_t total = ADSB_LINK_HDR_LEN + payload_len + 2;
    if (cap < total) return 0;

    out[0] = ADSB_LINK_MAGIC0;
    out[1] = ADSB_LINK_MAGIC1;
    out[2] = ADSB_LINK_VER_MAJOR;
    out[3] = ADSB_LINK_VER_MINOR;
    out[4] = msg_type;
    out[5] = seq;
    out[6] = (uint8_t)(payload_len & 0xFF);
    out[7] = (uint8_t)(payload_len >> 8);
    if (payload_len) memcpy(out + ADSB_LINK_HDR_LEN, payload, payload_len);
    uint16_t crc = adsb_link_crc16(out, total - 2);
    out[total - 2] = (uint8_t)(crc & 0xFF);        /* LE */
    out[total - 1] = (uint8_t)(crc >> 8);
    return total;
}

size_t adsb_link_uat_uplink_encode(uint8_t *out, size_t cap, uint8_t rssi,
                                   uint32_t rp_ts_us,
                                   const uint8_t frame[ADSB_LINK_UAT_FRAME_BYTES])
{
    if (cap < ADSB_LINK_UAT_PAYLOAD_LEN) return 0;
    out[0] = rssi;                                /* 0.5 dB/LSB，0xFF=无值 */
    out[1] = (uint8_t)rp_ts_us;                   /* 模 2^32 单调 µs，LE */
    out[2] = (uint8_t)(rp_ts_us >> 8);
    out[3] = (uint8_t)(rp_ts_us >> 16);
    out[4] = (uint8_t)(rp_ts_us >> 24);
    memcpy(out + ADSB_LINK_UAT_META_BYTES, frame, ADSB_LINK_UAT_FRAME_BYTES);
    return ADSB_LINK_UAT_PAYLOAD_LEN;
}

bool adsb_link_uat_uplink_decode(const uint8_t *payload, size_t len,
                                 uint8_t *rssi, uint32_t *rp_ts_us,
                                 const uint8_t **frame)
{
    /* 固定长度消息（规范 §6.1）：长度即完整性判据，不符整帧丢弃。 */
    if (len != ADSB_LINK_UAT_PAYLOAD_LEN) return false;
    *rssi = payload[0];
    *rp_ts_us = (uint32_t)payload[1] | ((uint32_t)payload[2] << 8) |
                ((uint32_t)payload[3] << 16) | ((uint32_t)payload[4] << 24);
    *frame = payload + ADSB_LINK_UAT_META_BYTES;
    return true;
}

static void dec_shift1(adsb_link_dec_t *d)
{
    d->fill--;
    memmove(d->buf, d->buf + 1, d->fill);
}

void adsb_link_dec_init(adsb_link_dec_t *d, adsb_link_on_msg_fn cb,
                        void *user)
{
    memset(d, 0, sizeof(*d));
    d->cb = cb;
    d->user = user;
}

static void dec_drain(adsb_link_dec_t *d)
{
    for (;;) {
        if (d->fill < ADSB_LINK_HDR_LEN) return;
        if (d->buf[0] != ADSB_LINK_MAGIC0 || d->buf[1] != ADSB_LINK_MAGIC1) {
            d->resyncs++;
            dec_shift1(d);
            continue;
        }
        uint16_t plen = (uint16_t)(d->buf[6] | ((uint16_t)d->buf[7] << 8));
        if (plen > ADSB_LINK_MAX_PAYLOAD) {
            d->len_errors++;
            dec_shift1(d);
            continue;
        }
        size_t total = ADSB_LINK_HDR_LEN + plen + 2;
        if (d->fill < total) return;               /* 还没收满一帧 */

        /* 校验顺序（2026-09-05 勘误）：magic → len → CRC → version。
         * CRC 先于版本：只有 CRC 合法的异版本帧才计 version_mismatch，
         * 坏 CRC 一律计 crc_errors——否则对端可凭噪声伪造 version_mismatch，
         * 把 P4 的 PROTO_MISMATCH 锁进误判。 */
        uint16_t crc_want = (uint16_t)(d->buf[total - 2] |
                                       ((uint16_t)d->buf[total - 1] << 8));
        if (crc_want != adsb_link_crc16(d->buf, total - 2)) {
            d->crc_errors++;
            dec_shift1(d);
            continue;
        }
        if (d->buf[2] != ADSB_LINK_VER_MAJOR) {
            d->version_mismatch++;
            dec_shift1(d);
            continue;
        }

        if (d->have_seq && (uint8_t)(d->buf[5] - d->last_seq) != 1)
            d->seq_gaps++;
        d->last_seq = d->buf[5];
        d->have_seq = 1;

        if (d->cb) {
            adsb_link_msg_t m;
            m.type = d->buf[4];
            m.seq = d->buf[5];
            m.payload_len = plen;
            memcpy(m.payload, d->buf + ADSB_LINK_HDR_LEN, plen);
            d->cb(d->user, &m);
        }
        d->msgs++;
        d->fill = (uint16_t)(d->fill - total);
        memmove(d->buf, d->buf + total, d->fill);
    }
}

void adsb_link_dec_feed(adsb_link_dec_t *d, const uint8_t *bytes, size_t n)
{
    /* 两段式（audit P2）：先把输入全部入栈，再统一 drain。
     * 旧实现逐字节边收边判，坏帧被拒后必须等下一个输入字节到达才重查
     * 缓冲——已缓冲的完整帧可能因此被扣住。现在任何一次作废/滑窗后
     * 都立即基于当前缓冲重跑判据，推进不依赖新输入。
     *
     * 满容即判（audit round 3）：fill 恰好到达缓冲容量时立刻 drain——
     * 协议最长帧（586 B = 缓冲容量，v1.1 上限）恰好填满缓冲，若等本批
     * 入栈完再判，其后随字节
     * 会在入栈阶段触发洪泛防御把完整帧的头部滑掉（实测：v1.0 时代 474 B
     * 帧 + 后随
     * 帧按 256 B 分块喂入 → resyncs=474、首帧丢失）。满容时若内容不是
     * 完整帧，drain 只会滑窗后返回，洪泛防御语义不变。 */
    while (n--) {
        if (d->fill >= sizeof(d->buf)) {   /* 噪声洪泛防御：保证总能滑窗 */
            d->resyncs++;
            dec_shift1(d);
        }
        d->buf[d->fill++] = *bytes++;
        if (d->fill == sizeof(d->buf)) dec_drain(d);
    }
    dec_drain(d);
}

/* ── CONFIG_REQ payload（v1.2，规范 §7）─────────────────────────────── */

size_t adsb_link_config_encode(uint8_t *out, size_t cap,
                               const adsb_link_cfg_item_t *items, uint8_t n)
{
    if (!out || (!items && n)) return 0;
    if (n > ADSB_LINK_CFG_MAX_ITEMS) return 0;
    const size_t need = 1u + (size_t)n * 2u;
    if (cap < need) return 0;                 /* 失败不写输出 */
    out[0] = n;
    for (uint8_t i = 0; i < n; i++) {
        out[1 + i * 2] = items[i].key;
        out[2 + i * 2] = items[i].val;
    }
    return need;
}

bool adsb_link_config_decode(const uint8_t *payload, size_t len,
                             adsb_link_cfg_item_t *out, uint8_t cap,
                             uint8_t *n)
{
    if (!payload || len < 1) return false;
    const uint8_t cnt = payload[0];
    if (cnt > ADSB_LINK_CFG_MAX_ITEMS || cnt > cap) return false;
    /* 声明条数与实际长度必须严格相符——多出来的尾巴不是"宽容"，而是
     * 我们不知道对面到底想说什么，照单全收会把垃圾当配置应用到 RF 通路上。 */
    if (len != 1u + (size_t)cnt * 2u) return false;
    if (!out && cnt) return false;
    for (uint8_t i = 0; i < cnt; i++) {
        out[i].key = payload[1 + i * 2];
        out[i].val = payload[2 + i * 2];
    }
    if (n) *n = cnt;
    return true;
}
