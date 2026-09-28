/*
 * test_uat_variable_transport.c — 978 MHz 30/48/552 B 可变帧合同。
 *
 * 跑法：
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/rp2040 -I firmware/cc1312r \
 *      -I firmware/components/rp_cc13xx_codec \
 *      -I firmware/components/adsb_link_codec \
 *      -DSPI_MASTER_HOST_TEST \
 *      -o /tmp/test_uat_variable_transport \
 *      firmware/test/test_uat_variable_transport.c \
 *      firmware/cc1312r/spi_slave.c firmware/rp2040/spi_master.c \
 *      firmware/components/rp_cc13xx_codec/rp_cc13xx_codec.c \
 *      firmware/components/adsb_link_codec/adsb_link_codec.c \
 *   && /tmp/test_uat_variable_transport
 *
 * 反例：旧实现把 CC 队列、descriptor、回调和 RP→P4 payload 全部写死为
 * 552 B。本测试直接走真实 slave/master/两套 codec，因此任何一处仍截断、
 * 混帧或把未知 kind 当合法都会失败。
 */
#include "spi_slave.h"
#include "spi_master.h"
#include "p4_link.h"
#include "rp_cc13xx_codec.h"
#include "adsb_link.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); \
    printf(" @%s:%d\n", __FILE__, __LINE__); fails++; } } while (0)

typedef struct {
    uint8_t bytes[3][CC13S_UAT_MAX_LEN];
    rp_cc13xx_rx_meta_t meta[3];
    size_t len[3];
    int n;
} capture_t;

static void capture(const uint8_t *frame, size_t len,
                    const rp_cc13xx_rx_meta_t *meta, void *user)
{
    capture_t *c = user;
    if (c->n >= 3 || len > sizeof(c->bytes[0])) return;
    memcpy(c->bytes[c->n], frame, len);
    c->len[c->n] = len;
    c->meta[c->n] = *meta;
    c->n++;
}

typedef struct {
    spi_master_t master;
    cc13s_slave_t slave;
    capture_t cap;
    uint32_t now;
} pair_t;

static void txn(pair_t *p, uint32_t dt)
{
    uint8_t mosi[CC13S_TXN_LEN], miso[CC13S_TXN_LEN];
    p->now += dt;
    if (spi_master_next_txn(&p->master, p->now, mosi,
                            cc13s_irq(&p->slave)) == 0) return;
    cc13s_pending(&p->slave, miso);
    cc13s_txn_done(&p->slave, mosi);
    spi_master_digest(&p->master, 512, miso);
}

static void link_up(pair_t *p)
{
    memset(p, 0, sizeof(*p));
    spi_master_init(&p->master, capture, &p->cap);
    cc13s_init(&p->slave, 2, 0);
    txn(p, 1000);
    txn(p, 500000);
    txn(p, 1000);
    txn(p, 1000000);
    CHECK(p->master.state == SPIM_LINKED && p->slave.state == CC13S_LINKED,
          "link did not reach LINKED");
}

static void fill(uint8_t *p, size_t n, uint8_t seed)
{
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i * 13u);
}

typedef struct {
    capture_t cap;
    int decode_errors;
} uart_capture_t;

static void uart_capture(void *user, const adsb_link_msg_t *msg)
{
    uart_capture_t *u = user;
    if (msg->type != ADSB_LINK_MSG_UAT_RX_FRAME || u->cap.n >= 3) {
        u->decode_errors++;
        return;
    }
    adsb_link_uat_rx_meta_t meta;
    const uint8_t *frame;
    uint16_t len;
    if (!adsb_link_uat_rx_decode(msg->payload, msg->payload_len, &meta,
                                 &frame, &len)) {
        u->decode_errors++;
        return;
    }
    const int i = u->cap.n++;
    memcpy(u->cap.bytes[i], frame, len);
    u->cap.len[i] = len;
    u->cap.meta[i].kind = meta.kind;
    u->cap.meta[i].flags = meta.flags;
    u->cap.meta[i].rssi_dbm = meta.rssi_dbm;
    u->cap.meta[i].frame_seq = meta.frame_seq;
    u->cap.meta[i].cc_time = meta.cc_time;
}

static void verify_uart_chain(const capture_t *source)
{
    adsb_link_dec_t decoder;
    uart_capture_t sink = {0};
    adsb_link_dec_init(&decoder, uart_capture, &sink);
    for (int i = 0; i < source->n; ++i) {
        uint8_t payload[ADSB_LINK_MAX_PAYLOAD];
        uint8_t wire[ADSB_LINK_MAX_FRAME];
        const size_t pn = p4_link_pack_uat_rx(
            payload, sizeof(payload), source->bytes[i], source->len[i],
            &source->meta[i]);
        const size_t wn = adsb_link_encode(
            wire, sizeof(wire), ADSB_LINK_MSG_UAT_RX_FRAME, (uint8_t)i,
            payload, pn);
        CHECK(pn == ADSB_LINK_UAT_RX_META_BYTES + source->len[i] && wn > pn,
              "UART encode frame %d", i);
        /* No inter-frame padding, deliberately irregular boundaries. */
        for (size_t off = 0; off < wn;) {
            size_t chunk = 7u + ((off + (size_t)i) % 23u);
            if (chunk > wn - off) chunk = wn - off;
            adsb_link_dec_feed(&decoder, wire + off, chunk);
            off += chunk;
        }
    }
    CHECK(sink.decode_errors == 0 && sink.cap.n == source->n,
          "UART sink errors=%d frames=%d/%d", sink.decode_errors,
          sink.cap.n, source->n);
    for (int i = 0; i < sink.cap.n && i < source->n; ++i) {
        CHECK(sink.cap.len[i] == source->len[i] &&
              !memcmp(sink.cap.bytes[i], source->bytes[i], source->len[i]) &&
              sink.cap.meta[i].kind == source->meta[i].kind &&
              sink.cap.meta[i].flags == source->meta[i].flags &&
              sink.cap.meta[i].rssi_dbm == source->meta[i].rssi_dbm &&
              sink.cap.meta[i].frame_seq == source->meta[i].frame_seq &&
              sink.cap.meta[i].cc_time == source->meta[i].cc_time,
              "UART frame %d truncated/mixed", i);
    }
}

static void test_descriptor_codec(void)
{
    const struct { uint8_t kind; uint16_t len; } cases[] = {
        { RP_CC13XX_UAT_DOWNLINK_SHORT, 30 },
        { RP_CC13XX_UAT_DOWNLINK_LONG, 48 },
        { RP_CC13XX_UAT_UPLINK, 552 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        rp_cc13xx_rx_desc_t in = {
            .frame_seq = (uint16_t)(0xFFFEu + i),
            .freq_hz = 978000000u,
            .cc_time = 0xFFFFFFF0u + (uint32_t)i,
            .total_len = cases[i].len,
            .rssi_dbm = (int8_t)(-93 + (int)i),
            .kind = cases[i].kind,
            .flags = RP_CC13XX_DESC_F_SYNC36_OK | RP_CC13XX_DESC_F_RF_STATUS_OK,
        };
        uint8_t wire[512];
        size_t n = rp_cc13xx_encode_rx_descriptor(wire, sizeof(wire), 7, &in);
        CHECK(n != 0, "descriptor kind=%u encode", in.kind);
        rp_cc13xx_msg_t msg;
        CHECK(rp_cc13xx_decode_frame(wire, n, &msg) == RP_CC13XX_OK,
              "descriptor kind=%u frame decode", in.kind);
        rp_cc13xx_rx_desc_t out;
        CHECK(rp_cc13xx_decode_rx_descriptor(&msg, &out) == RP_CC13XX_OK,
              "descriptor kind=%u payload decode", in.kind);
        CHECK(out.frame_seq == in.frame_seq && out.freq_hz == in.freq_hz &&
              out.cc_time == in.cc_time && out.total_len == in.total_len &&
              out.rssi_dbm == in.rssi_dbm && out.kind == in.kind &&
              out.flags == in.flags && out.reserved == 0,
              "descriptor kind=%u roundtrip", in.kind);
    }

    rp_cc13xx_rx_desc_t bad = {
        .frame_seq = 1, .freq_hz = 978000000u, .cc_time = 2,
        .total_len = 31, .rssi_dbm = -80,
        .kind = RP_CC13XX_UAT_DOWNLINK_SHORT,
        .flags = RP_CC13XX_DESC_F_SYNC36_OK,
    };
    uint8_t wire[512];
    CHECK(rp_cc13xx_encode_rx_descriptor(wire, sizeof(wire), 1, &bad) == 0,
          "short kind with 31 bytes accepted");
    bad.total_len = 30; bad.kind = 0x7f;
    CHECK(rp_cc13xx_encode_rx_descriptor(wire, sizeof(wire), 1, &bad) == 0,
          "unknown kind accepted");
    bad.kind = RP_CC13XX_UAT_DOWNLINK_SHORT; bad.flags = 0x80;
    CHECK(rp_cc13xx_encode_rx_descriptor(wire, sizeof(wire), 1, &bad) == 0,
          "reserved flags accepted");
}

static void test_three_lengths_end_to_end(void)
{
    pair_t p; link_up(&p);
    uint8_t f30[30], f48[48], f552[552];
    fill(f30, sizeof f30, 0x10);
    fill(f48, sizeof f48, 0x40);
    fill(f552, sizeof f552, 0x80);
    CHECK(cc13s_push_rx(&p.slave, RP_CC13XX_UAT_DOWNLINK_SHORT,
                        f30, sizeof f30, -91, 0xFFFFFFF0u), "push 30");
    CHECK(cc13s_push_rx(&p.slave, RP_CC13XX_UAT_DOWNLINK_LONG,
                        f48, sizeof f48, -72, 0x00000010u), "push 48");
    CHECK(cc13s_push_rx(&p.slave, RP_CC13XX_UAT_UPLINK,
                        f552, sizeof f552, -61, 0x12345678u), "push 552");
    for (int guard = 0; p.cap.n < 3 && guard < 40; guard++) txn(&p, 1000);
    CHECK(p.cap.n == 3, "delivered %d/3", p.cap.n);
    if (p.cap.n == 3) {
        CHECK(p.cap.len[0] == 30 && !memcmp(p.cap.bytes[0], f30, 30), "30 truncated/mixed");
        CHECK(p.cap.len[1] == 48 && !memcmp(p.cap.bytes[1], f48, 48), "48 truncated/mixed");
        CHECK(p.cap.len[2] == 552 && !memcmp(p.cap.bytes[2], f552, 552), "552 truncated/mixed");
        CHECK(p.cap.meta[0].kind == RP_CC13XX_UAT_DOWNLINK_SHORT &&
              p.cap.meta[1].kind == RP_CC13XX_UAT_DOWNLINK_LONG &&
              p.cap.meta[2].kind == RP_CC13XX_UAT_UPLINK, "kind order");
        CHECK(p.cap.meta[0].frame_seq == 0 && p.cap.meta[1].frame_seq == 1 &&
              p.cap.meta[2].frame_seq == 2, "frame sequence");
        CHECK(p.cap.meta[0].cc_time == 0xFFFFFFF0u &&
              p.cap.meta[1].cc_time == 0x10u, "CC clock wrap changed");
        CHECK(p.cap.meta[0].rssi_dbm == -91 && p.cap.meta[2].rssi_dbm == -61,
              "signed RSSI changed");
        verify_uart_chain(&p.cap);
    }
}

static void test_queue_drop_and_sequence_wrap(void)
{
    pair_t p; link_up(&p);
    uint8_t f[30]; fill(f, sizeof f, 3);
    p.slave.next_frame_seq = 0xFFFEu;
    CHECK(cc13s_push_rx(&p.slave, RP_CC13XX_UAT_DOWNLINK_SHORT,
                        f, sizeof f, -80, 1), "push fffe");
    CHECK(cc13s_push_rx(&p.slave, RP_CC13XX_UAT_DOWNLINK_SHORT,
                        f, sizeof f, -80, 2), "push ffff");
    CHECK(cc13s_push_rx(&p.slave, RP_CC13XX_UAT_DOWNLINK_SHORT,
                        f, sizeof f, -80, 3), "push 0000");
    for (int guard = 0; p.cap.n < 3 && guard < 30; guard++) txn(&p, 1000);
    CHECK(p.cap.n == 3, "wrap delivery count=%d", p.cap.n);
    if (p.cap.n == 3)
        CHECK(p.cap.meta[0].frame_seq == 0xFFFEu &&
              p.cap.meta[1].frame_seq == 0xFFFFu && p.cap.meta[2].frame_seq == 0,
              "wrap sequence %04x/%04x/%04x", p.cap.meta[0].frame_seq,
              p.cap.meta[1].frame_seq, p.cap.meta[2].frame_seq);
    CHECK(p.master.stats.frame_seq_gaps == 0, "normal wrap counted as gap");

    pair_t q; link_up(&q);
    for (int i = 0; i < CC13S_MAXQ; i++)
        CHECK(cc13s_push_rx(&q.slave, RP_CC13XX_UAT_DOWNLINK_SHORT,
                            f, sizeof f, -80, (uint32_t)i), "queue fill %d", i);
    CHECK(!cc13s_push_rx(&q.slave, RP_CC13XX_UAT_DOWNLINK_SHORT,
                         f, sizeof f, -80, 9), "overflow accepted");
    CHECK(q.slave.dropped == 1 && q.slave.qfull, "overflow not observable");
    for (int guard = 0; q.slave.qn == CC13S_MAXQ && guard < 10; guard++) txn(&q, 1000);
    CHECK(cc13s_push_rx(&q.slave, RP_CC13XX_UAT_DOWNLINK_SHORT,
                        f, sizeof f, -80, 10), "post-drop push");
    for (int guard = 0; q.cap.n < CC13S_MAXQ + 1 && guard < 50; guard++) txn(&q, 1000);
    CHECK(q.master.stats.frame_seq_gaps == 1,
          "dropped RF frame gap not observable: %u", q.master.stats.frame_seq_gaps);

    CHECK(!cc13s_push_rx(&q.slave, 0x77, f, sizeof f, -80, 11),
          "unknown kind accepted");
    CHECK(!cc13s_push_rx(&q.slave, RP_CC13XX_UAT_DOWNLINK_SHORT,
                         f, sizeof f - 1, -80, 12), "bad length accepted");
    CHECK(q.slave.invalid_frames == 2, "invalid counter=%u", q.slave.invalid_frames);
}

static void test_rp_to_p4_v14_payload(void)
{
    const struct { uint8_t kind; uint16_t len; } cases[] = {
        { ADSB_LINK_UAT_DOWNLINK_SHORT, 30 },
        { ADSB_LINK_UAT_DOWNLINK_LONG, 48 },
        { ADSB_LINK_UAT_UPLINK, 552 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t frame[552], payload[ADSB_LINK_MAX_PAYLOAD];
        fill(frame, cases[i].len, (uint8_t)(0x21 + i));
        rp_cc13xx_rx_meta_t in = {
            .kind = cases[i].kind,
            .flags = ADSB_LINK_UAT_F_SYNC36_OK | ADSB_LINK_UAT_F_RF_STATUS_OK,
            .rssi_dbm = (int8_t)(-88 + (int)i),
            .frame_seq = (uint16_t)(0xFFFFu + i),
            .cc_time = 0xFFFFFFFEu + (uint32_t)i,
        };
        size_t n = p4_link_pack_uat_rx(payload, sizeof payload, frame,
                                       cases[i].len, &in);
        CHECK(n == ADSB_LINK_UAT_RX_META_BYTES + cases[i].len,
              "P4 payload len kind=%u got=%zu", in.kind, n);
        adsb_link_uat_rx_meta_t out = {0};
        const uint8_t *decoded = NULL; uint16_t decoded_len = 0;
        CHECK(adsb_link_uat_rx_decode(payload, n, &out, &decoded, &decoded_len),
              "P4 payload decode kind=%u", in.kind);
        CHECK(out.kind == in.kind && out.flags == in.flags &&
              out.rssi_dbm == in.rssi_dbm && out.frame_seq == in.frame_seq &&
              out.cc_time == in.cc_time && decoded_len == cases[i].len &&
              decoded == payload + ADSB_LINK_UAT_RX_META_BYTES &&
              !memcmp(decoded, frame, decoded_len),
              "P4 payload roundtrip kind=%u", in.kind);
    }
    uint8_t payload[ADSB_LINK_MAX_PAYLOAD] = {0};
    rp_cc13xx_rx_meta_t meta = {
        .kind = ADSB_LINK_UAT_DOWNLINK_SHORT,
        .flags = ADSB_LINK_UAT_F_SYNC36_OK,
        .rssi_dbm = -70,
    };
    uint8_t frame[31] = {0};
    CHECK(p4_link_pack_uat_rx(payload, sizeof payload, frame, 31, &meta) == 0,
          "P4 mismatched kind/len accepted");
}

static void test_legacy_552_uart_compatibility(void)
{
    uint8_t frame[552], payload[ADSB_LINK_MAX_PAYLOAD], type = 0;
    fill(frame, sizeof frame, 0x5a);
    /* RX_DESCRIPTOR v1 decodes with kind=0 and retains the legacy wire byte
     * aliases rssi/ts_us.  It must continue onto UART type 0x11, rather than
     * being rejected by the strict v1.4 kind gate. */
    rp_cc13xx_rx_meta_t legacy = {
        .frame_seq = 7,
        .ts_us = 0x12345678u,
        .total_len = 552,
        .rssi = 0x93,
        .kind = 0,
        .flags = 0,
    };
    const size_t n = p4_link_pack_uat_auto(
        payload, sizeof payload, &type, frame, sizeof frame, &legacy);
    CHECK(type == ADSB_LINK_MSG_UAT_UPLINK &&
          n == ADSB_LINK_UAT_PAYLOAD_LEN,
          "legacy descriptor did not select UART 0x11: type=%02x len=%zu",
          type, n);
    uint8_t rssi = 0; uint32_t ts = 0; const uint8_t *decoded = NULL;
    CHECK(adsb_link_uat_uplink_decode(payload, n, &rssi, &ts, &decoded) &&
          rssi == 0x93 && ts == 0x12345678u &&
          !memcmp(decoded, frame, sizeof frame),
          "legacy 552 metadata/payload changed");

    CHECK(p4_link_pack_uat_auto(payload, sizeof payload, &type,
                                frame, 48, &legacy) == 0,
          "legacy descriptor accepted non-552 frame");
}

int main(void)
{
    test_descriptor_codec();
    test_three_lengths_end_to_end();
    test_queue_drop_and_sequence_wrap();
    test_rp_to_p4_v14_payload();
    test_legacy_552_uart_compatibility();
    if (fails) { printf("%d failures\n", fails); return 1; }
    puts("PASS: variable UAT transport 30/48/552");
    return 0;
}
