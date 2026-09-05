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

void adsb_link_dec_feed(adsb_link_dec_t *d, const uint8_t *bytes, size_t n)
{
    while (n--) {
        if (d->fill >= sizeof(d->buf)) {   /* 噪声洪泛防御：保证总能滑窗 */
            d->resyncs++;
            dec_shift1(d);
        }
        d->buf[d->fill++] = *bytes++;
        if (d->fill < ADSB_LINK_HDR_LEN) continue;

        if (d->buf[0] != ADSB_LINK_MAGIC0 || d->buf[1] != ADSB_LINK_MAGIC1) {
            d->resyncs++;
            dec_shift1(d);
            continue;
        }
        if (d->fill == ADSB_LINK_HDR_LEN) {
            if (d->buf[2] != ADSB_LINK_VER_MAJOR) {
                d->version_mismatch++;
                dec_shift1(d);
                continue;
            }
            uint16_t plen = (uint16_t)(d->buf[6] | ((uint16_t)d->buf[7] << 8));
            if (plen > ADSB_LINK_MAX_PAYLOAD) {
                d->len_errors++;
                dec_shift1(d);
                continue;
            }
        }
        uint16_t plen = (uint16_t)(d->buf[6] | ((uint16_t)d->buf[7] << 8));
        size_t total = ADSB_LINK_HDR_LEN + plen + 2;
        if (d->fill < total) continue;               /* 还没收满一帧 */

        uint16_t crc_want = (uint16_t)(d->buf[total - 2] |
                                       ((uint16_t)d->buf[total - 1] << 8));
        if (crc_want != adsb_link_crc16(d->buf, total - 2)) {
            d->crc_errors++;
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
