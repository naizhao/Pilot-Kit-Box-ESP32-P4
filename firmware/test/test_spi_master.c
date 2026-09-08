/*
 * test_spi_master.c — RP2040 SPI master 状态机的 host 单测（WP-E T3）。
 *
 * 跑法：
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/rp2040 \
 *      -I firmware/components/rp_cc13xx_codec \
 *      -I firmware/components/adsb_link_codec \
 *      -DSPI_MASTER_HOST_TEST \
 *      -o /tmp/test_spi_master \
 *      firmware/test/test_spi_master.c \
 *      firmware/rp2040/spi_master.c \
 *      firmware/components/rp_cc13xx_codec/rp_cc13xx_codec.c \
 *      firmware/components/adsb_link_codec/adsb_link_codec.c \
 *   && /tmp/test_spi_master
 *
 * 判据来源：firmware/PROTOCOL_RP2040_CC1312R_SPI.md v1.0。本文件把规范
 * 附录 B.3 的走查场景（B30 分裂脑、B34 队满重填、B35 异步竞争……）全部
 * 变成可执行测试——十轮审计"文档走查不可执行"的正式回应。
 *
 * mock slave 严格实现规范的 slave 侧行为：§2.3 装载规则（五步链）、
 * §1 挂起源生命周期（**交付完成才清**——装载沿只装不清，与 round-9
 * 审计的清除时点语义一致）、§6.2 WAIT_HELLO 应答策略（HELLO 应答 /
 * PING 静默 / 其余作废）、§3.5 事件帧取 slave 事件计数器。
 */
#include "spi_master.h"
#include "rp_cc13xx_codec.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("        at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

/* ── mock slave（规范 §2.3/§1/§6.2 的最小忠实实现）────────────────── */

#define MS_MAXQ 4
#define UAT_LEN 552

typedef struct {
    bool     linked;                     /* slave LINKED（否则 WAIT_HELLO） */
    uint8_t  q[MS_MAXQ][UAT_LEN];        /* 事件队列：UAT 报文 */
    uint8_t  q_rssi[MS_MAXQ];
    uint32_t q_ts[MS_MAXQ];
    int      qn;
    uint16_t ev_seq;                     /* slave 事件计数器（§3.5） */
    bool     qfull;                      /* queue_full_pending（§4.5） */
    uint16_t dropped;
    bool     aerr;                       /* async_error_pending（§2.3 规则 4） */

    /* 交付中的报文 */
    int      cur;                        /* 队列下标；-1 = 无 */
    uint16_t cur_desc_id, cur_sent, cur_total;
    uint16_t last_desc_id;

    /* pending 槽（事务 N 装载、事务 N+1 交付，§2.2 延迟应答）。所有
     * 源的清除都发生在**交付沿**（§1）——记录交付时需要的伴随量。 */
    uint8_t  pend[512];
    int      pend_kind;                  /* RP_CC13XX_MSG_*；0 = 空（全 0） */
    uint16_t pend_dlen;                  /* pend 为 CHUNK 时的 data_len */
    bool     pend_final;                 /* pend 为末片 */

    /* 测试注入 */
    bool     bad_hello_ver;              /* HELLO 应答 ver=2（场景 6） */
} ms_t;

static void ms_pwr_reset(ms_t *s)
{
    memset(s, 0, sizeof(*s));
    s->cur = -1;
}

/* §1：IRQ = 四源之或（事件队列非空 ∨ 分片未取完 ∨ qfull ∨ aerr）。
 * 已装载未交付的帧仍算置位（装载不清除）。 */
static bool ms_irq(const ms_t *s)
{
    if (!s->linked) return false;        /* WAIT_HELLO 不驱动事件流 */
    if (s->qfull || s->aerr) return true;
    if (s->cur >= 0 && s->cur_sent < s->cur_total) return true;
    return s->qn > 0;
}

/* 装载一帧到 pending 槽（§2.3 各规则的公共出口；只装不清）。 */
static void ms_load(ms_t *s, const uint8_t *frame, size_t n, int kind)
{
    memset(s->pend, 0, sizeof(s->pend));
    if (frame && n) memcpy(s->pend, frame, n < 512 ? n : 512);
    s->pend_kind  = kind;
    s->pend_dlen  = 0;
    s->pend_final = false;
}

static void ms_pop_queue(ms_t *s)
{
    if (s->qn <= 0) { s->cur = -1; return; }
    memmove(s->q, s->q + 1, sizeof(s->q[0]) * (size_t)(s->qn - 1));
    memmove(s->q_rssi, s->q_rssi + 1, sizeof(s->q_rssi[0]) * (size_t)(s->qn - 1));
    memmove(s->q_ts, s->q_ts + 1, sizeof(s->q_ts[0]) * (size_t)(s->qn - 1));
    s->qn--;
    s->cur = -1;
}

/* 一次事务：MISO = 上一事务装载的 pending（交付）；CSN↑ 处理 MOSI 并
 * 按 §2.3 五步链装载下一帧。交付沿清除对应源（§1）。 */
static void ms_txn(ms_t *s, const uint8_t *mosi, uint8_t *miso)
{
    /* 1) MISO 交付（延迟应答：上一事务装载的内容） */
    memcpy(miso, s->pend, 512);
    const int  delivered = s->pend_kind;
    const uint16_t ddlen = s->pend_dlen;
    const bool dfinal   = s->pend_final;
    s->pend_kind = 0;
    memset(s->pend, 0, sizeof(s->pend));

    /* 2) 交付完成清源（§1：各源于携带其帧的事务 CSN 上升沿清除） */
    if (delivered == RP_CC13XX_MSG_QUEUE_FULL)  s->qfull = false;
    if (delivered == RP_CC13XX_MSG_ERROR)       s->aerr  = false;
    if (delivered == RP_CC13XX_MSG_RX_PAYLOAD_CHUNK) {
        s->cur_sent = (uint16_t)(s->cur_sent + ddlen);
        if (dfinal) ms_pop_queue(s);     /* 末片交付 → 报文出队 */
    }

    /* 3) 处理 MOSI 命令 */
    rp_cc13xx_msg_t in;
    const rp_cc13xx_status_t st = rp_cc13xx_decode_frame(mosi, 512, &in);
    const bool cmd_ok = (st == RP_CC13XX_OK);

    if (!s->linked) {
        /* §6.2 WAIT_HELLO 应答策略：HELLO 应答 / PING 静默 / 其余作废 */
        if (cmd_ok && in.type == RP_CC13XX_MSG_HELLO) {
            rp_cc13xx_hello_t h = { .fw_ver_major = 9, .fw_ver_minor = 1,
                                    .reset_reason = RP_CC13XX_RESET_POR };
            uint8_t f[512];
            size_t n = rp_cc13xx_encode_hello(f, sizeof(f), in.seq, &h);
            if (s->bad_hello_ver) {
                f[2] = 2;                 /* 注入 ver=2 并重算 CRC */
                const uint16_t crc = rp_cc13xx_crc16(f, n - 2);
                f[n - 2] = (uint8_t)(crc & 0xFF);
                f[n - 1] = (uint8_t)(crc >> 8);
            }
            ms_load(s, f, n, RP_CC13XX_MSG_HELLO);
            s->linked = true;            /* 握手段不删（§6.2） */
        } else {
            ms_load(s, NULL, 0, 0);
        }
        return;
    }

    /* 4) §2.3 五步装载链（命令性 ERROR 顺延由测试不触发——规则 2 略） */
    if (cmd_ok && in.type == RP_CC13XX_MSG_IRQ_ACK &&
        s->cur >= 0 && s->cur_sent < s->cur_total) {
        /* 规则 1：取片是 IRQ_ACK 读的延续（命令非 IRQ_ACK 时不适用，
         * 分片保持已装载不丢失——§2.3 规则 1 括号）。 */
        rp_cc13xx_chunk_t c = {
            .desc_id   = s->cur_desc_id,
            .offset    = s->cur_sent,
            .total_len = s->cur_total,
        };
        const uint16_t left = (uint16_t)(s->cur_total - s->cur_sent);
        c.data_len = left > RP_CC13XX_CHUNK_MAX_DATA
                   ? RP_CC13XX_CHUNK_MAX_DATA : left;
        memcpy(c.data, s->q[s->cur] + s->cur_sent, c.data_len);
        uint8_t f[512];
        size_t n = rp_cc13xx_encode_rx_chunk(f, sizeof(f), s->ev_seq++, &c);
        ms_load(s, f, n, RP_CC13XX_MSG_RX_PAYLOAD_CHUNK);
        s->pend_dlen  = c.data_len;      /* 交付沿推进 cur_sent 用 */
        s->pend_final = (uint16_t)(s->cur_sent + c.data_len) >= s->cur_total;
        return;
    }
    if (cmd_ok && in.type == RP_CC13XX_MSG_HELLO) {
        rp_cc13xx_hello_t h = { .fw_ver_major = 9, .fw_ver_minor = 1,
                                .reset_reason = RP_CC13XX_RESET_POR };
        uint8_t f[512];
        size_t n = rp_cc13xx_encode_hello(f, sizeof(f), in.seq, &h);
        if (s->bad_hello_ver) {
            f[2] = 2;                       /* 注入一致（两分支同源） */
            const uint16_t crc = rp_cc13xx_crc16(f, n - 2);
            f[n - 2] = (uint8_t)(crc & 0xFF);
            f[n - 1] = (uint8_t)(crc >> 8);
        }
        ms_load(s, f, n, RP_CC13XX_MSG_HELLO);
        return;
    }
    if (cmd_ok && in.type == RP_CC13XX_MSG_PING) {
        uint8_t f[512];
        size_t n = rp_cc13xx_encode_empty(f, sizeof(f),
                                          RP_CC13XX_MSG_PONG, in.seq);
        ms_load(s, f, n, RP_CC13XX_MSG_PONG);   /* 规则 3：直接应答 */
        return;
    }
    if (cmd_ok && in.type == RP_CC13XX_MSG_RF_CONFIG) {
        rp_cc13xx_rf_config_t c = { .config_version = 7 };
        uint8_t f[512];
        size_t n = rp_cc13xx_encode_rf_config(f, sizeof(f),
                                              RP_CC13XX_MSG_RF_CONFIG_STATUS,
                                              in.seq, &c);
        ms_load(s, f, n, RP_CC13XX_MSG_RF_CONFIG_STATUS);
        return;
    }
    if (s->qfull) {                       /* 规则 4a：通知优先（§4.5） */
        rp_cc13xx_queue_full_t q = { .events_dropped = s->dropped,
                                     .queue_depth = (uint8_t)s->qn };
        uint8_t f[512];
        size_t n = rp_cc13xx_encode_queue_full(f, sizeof(f),
                                               s->ev_seq++, &q);
        ms_load(s, f, n, RP_CC13XX_MSG_QUEUE_FULL);
        return;
    }
    if (s->qn > 0 && s->cur < 0) {        /* 规则 4b：队头事件 → descriptor */
        rp_cc13xx_rx_desc_t d = {
            .desc_id   = (uint16_t)(s->last_desc_id + 1),
            .freq_hz   = 978000000u,
            .ts_us     = s->q_ts[0],
            .total_len = UAT_LEN,
            .rssi      = s->q_rssi[0],
        };
        uint8_t f[512];
        size_t n = rp_cc13xx_encode_rx_descriptor(f, sizeof(f),
                                                  s->ev_seq++, &d);
        ms_load(s, f, n, RP_CC13XX_MSG_RX_DESCRIPTOR);
        s->last_desc_id = d.desc_id;
        s->cur          = 0;              /* 开启交付（末片交付沿出队） */
        s->cur_desc_id  = d.desc_id;
        s->cur_sent     = 0;
        s->cur_total    = UAT_LEN;
        return;
    }
    if (s->aerr) {                        /* 规则 4c：异步 ERROR（seq 哨兵） */
        rp_cc13xx_error_t e = { .code = 0x04, .msg_len = 4,
                                .msg = { 'F','W','E','R' } };
        uint8_t f[512];
        size_t n = rp_cc13xx_encode_error(f, sizeof(f), 0x0000, &e);
        ms_load(s, f, n, RP_CC13XX_MSG_ERROR);
        return;                           /* 清除在交付沿（§1） */
    }
    ms_load(s, NULL, 0, 0);               /* 规则 5：全 0 */
}

/* ── 测试驱动（与目标端 spim_poll 同构）────────────────────────────── */

static uint32_t g_now;
static uint8_t  cap_frame[UAT_LEN];
static size_t   cap_len;
static uint8_t  cap_rssi;
static uint32_t cap_ts;
static int      cap_n;

static void uat_capture(const uint8_t *frame, size_t len,
                        uint8_t rssi, uint32_t ts_us, void *user)
{
    (void)user;
    if (len <= UAT_LEN) memcpy(cap_frame, frame, len);
    cap_len  = len;
    cap_rssi = rssi;
    cap_ts   = ts_us;
    cap_n++;
}

static void step(spi_master_t *m, ms_t *s, uint32_t dt)
{
    g_now += dt;
    if (spi_master_reset_requested(m)) {
        ms_pwr_reset(s);                  /* RESET 清 slave 全部会话态 */
        spi_master_reset_done(m, g_now);  /* 胶水已执行脉冲 + 100 ms 等待 */
        return;
    }
    uint8_t mosi[512], miso[512];
    const bool irq = ms_irq(s);
    if (spi_master_next_txn(m, g_now, mosi, irq) == 0) return;
    ms_txn(s, mosi, miso);
    spi_master_digest(m, 512, miso);
}

/* 完整握手（延迟应答的真实节奏）：
 *   T1(1ms) HELLO#1 → MISO 全 0（pending 空）
 *   T2(500ms) HELLO#2 → MISO = HELLO 应答#1 → LINKED + rf_pending
 *   T3(1ms) RF_CONFIG → MISO = HELLO 回声#2（仅刷新 peer 版本）
 *   T4(1s) PING → MISO = RF_CONFIG_STATUS → peer_rf 就绪
 * 任何命令的应答都要"下一个事务"取走（§2.2 延迟应答），空闲时 1 Hz
 * PING 是取件兜底（§6.6）。 */
static void do_handshake(spi_master_t *m, ms_t *s)
{
    step(m, s, 1000);
    step(m, s, 500000);
    step(m, s, 1000);
    step(m, s, 1000000);
}

static void push_frame(ms_t *s, uint8_t seed)
{
    for (int i = 0; i < UAT_LEN; i++) s->q[s->qn][i] = (uint8_t)(seed + i);
    s->q_rssi[s->qn] = 0x40;
    s->q_ts[s->qn]   = 1000000u + (uint32_t)s->qn;
    s->qn++;
}

/* ── 场景（规范 B.3 走查的可执行化）───────────────────────────────── */

static void sc1_handshake(void)
{
    spi_master_t m; ms_t s;
    spi_master_init(&m, uat_capture, NULL);
    ms_pwr_reset(&s);
    g_now = 0;

    step(&m, &s, 1000);                   /* T1 HELLO → 全 0 */
    CHECK(m.state == SPIM_WAIT_HELLO, "sc1 T1 state\n");
    CHECK(m.stats.no_frame_txns == 1, "sc1 T1 no-frame\n");
    step(&m, &s, 500000);                 /* T2 → LINKED */
    CHECK(m.state == SPIM_LINKED, "sc1 T2 linked\n");
    CHECK(m.peer_hello.fw_ver_major == 9, "sc1 peer fw\n");
    CHECK(m.rf_pending, "sc1 rf pending\n");
    step(&m, &s, 1000);                   /* T3 RF_CONFIG（MISO=HELLO 回声） */
    CHECK(!m.rf_pending, "sc1 rf sent\n");
    step(&m, &s, 1000000);                /* T4 PING → 取走 RF_STATUS */
    CHECK(m.peer_rf.config_version == 7, "sc1 peer rf\n");
    step(&m, &s, 1000);                   /* 空闲（PING 未到期）不发 */
    step(&m, &s, 1000000);                /* PING#2 → MISO = PONG#1 */
    CHECK(m.stats.txns == 5, "sc1 txns %u\n", m.stats.txns);
    CHECK(m.stats.legal_frames == 4, "sc1 legal %u\n", m.stats.legal_frames);
}

static void sc2_split_brain(void)
{
    spi_master_t m; ms_t s;
    spi_master_init(&m, uat_capture, NULL);
    ms_pwr_reset(&s);
    g_now = 0;
    do_handshake(&m, &s);
    CHECK(m.state == SPIM_LINKED, "sc2 linked\n");

    ms_pwr_reset(&s);                     /* slave 自复位 → WAIT_HELLO */
    /* master 1 Hz PING 落空（slave 静默，§6.2）× >3 s → RECOVERY（B30） */
    for (int i = 0; i < 5 && m.state == SPIM_LINKED; i++) {
        step(&m, &s, 1000000);
    }
    CHECK(m.state == SPIM_RECOVERY || m.reset_req, "sc2 recovery\n");
    step(&m, &s, 1000);                   /* RESET 脉冲 + 100 ms 等待 */
    CHECK(m.state == SPIM_WAIT_HELLO, "sc2 wait_hello\n");
    do_handshake(&m, &s);                 /* 重握手 + RF_CONFIG 重发 */
    CHECK(m.state == SPIM_LINKED, "sc2 relinked\n");
    CHECK(m.peer_rf.config_version == 7, "sc2 rf resent\n");
    CHECK(m.stats.recoveries == 1, "sc2 recoveries %u\n", m.stats.recoveries);
}

static void sc3_event_flow(void)
{
    spi_master_t m; ms_t s;
    spi_master_init(&m, uat_capture, NULL);
    ms_pwr_reset(&s);
    g_now = 0;
    cap_n = 0;
    do_handshake(&m, &s);

    push_frame(&s, 0);
    const uint8_t expect0 = s.q[0][0];

    /* S1 IRQ_ACK：MISO=PONG（T4 应答取走），mock 装 DESC
     * S2 IRQ_ACK：MISO=DESC，装 chunk1
     * S3 IRQ_ACK：MISO=chunk1，装 chunk2
     * S4 IRQ_ACK：MISO=chunk2 → 552 B 完成 */
    step(&m, &s, 1000);
    step(&m, &s, 1000);
    step(&m, &s, 1000);
    step(&m, &s, 1000);
    CHECK(cap_n == 1, "sc3 uat cb %d\n", cap_n);
    CHECK(cap_len == UAT_LEN, "sc3 len %u\n", (unsigned)cap_len);
    CHECK(cap_rssi == 0x40 && cap_ts == 1000000u, "sc3 meta\n");
    CHECK(cap_frame[0] == expect0 && cap_frame[551] == (uint8_t)(0 + 551),
          "sc3 bytes\n");
    CHECK(m.stats.uat_complete == 1, "sc3 complete %u\n", m.stats.uat_complete);
    CHECK(m.stats.seq_gaps == 0, "sc3 gaps %u\n", m.stats.seq_gaps);
    step(&m, &s, 1000);                   /* 队列空、IRQ 低 → 空闲 */
    CHECK(s.qn == 0, "sc3 drained\n");
    CHECK(m.state == SPIM_LINKED, "sc3 linked\n");
}

static void sc4_queue_full_refill(void)
{
    spi_master_t m; ms_t s;
    spi_master_init(&m, uat_capture, NULL);
    ms_pwr_reset(&s);
    g_now = 0;
    cap_n = 0;
    do_handshake(&m, &s);

    push_frame(&s, 10);
    push_frame(&s, 60);
    s.qfull = true;  s.dropped = 5;

    /* drain（每事务取走上一拍装载）：QFULL 装(S1) → 交+DESC(S2) →
     * 交+chunk1(S3) → 交+chunk2(S4) → 帧 A 完成(S5)。 */
    step(&m, &s, 1000);                   /* S1：MISO=PONG，装 QFULL */
    step(&m, &s, 1000);                   /* S2：QFULL 交付，装 DESC */
    step(&m, &s, 1000);                   /* S3：DESC 交付，装 chunk1 */
    step(&m, &s, 1000);                   /* S4：chunk1 交付，装 chunk2 */
    step(&m, &s, 1000);                   /* S5：chunk2 交付 → 帧 A 完成 */
    CHECK(cap_n == 1, "sc4 fA %d\n", cap_n);
    CHECK(cap_frame[0] == 10, "sc4 fA bytes\n");

    push_frame(&s, 200);                  /* drain 中重填：新帧到达 */
    s.qfull = true;  s.dropped = 6;       /* 队列再次满（§4.5 再通知） */

    step(&m, &s, 1000);                   /* S6：装 QFULL（重填再通知） */
    step(&m, &s, 1000);                   /* S7：QFULL 交+DESC */
    step(&m, &s, 1000);
    step(&m, &s, 1000);
    step(&m, &s, 1000);                   /* S10：帧 B 完成 */
    CHECK(cap_n == 2, "sc4 fB %d\n", cap_n);
    CHECK(m.stats.recoveries == 0, "sc4 no recovery\n");
    CHECK(m.stats.seq_gaps == 0, "sc4 gaps %u\n", m.stats.seq_gaps);
    step(&m, &s, 1000);
    CHECK(s.qn == 1, "sc4 residual q=%d（重填的第三帧仍在）\n", s.qn);
    CHECK(m.state == SPIM_LINKED, "sc4 linked\n");
}

static void sc5_async_ping_race(void)
{
    spi_master_t m; ms_t s;
    spi_master_init(&m, uat_capture, NULL);
    ms_pwr_reset(&s);
    g_now = 0;
    do_handshake(&m, &s);

    step(&m, &s, 1000000);                /* PING 发出（此刻 IRQ 低） */
    s.aerr = true;                        /* 事务中异步错误到达 → IRQ 升高 */
    step(&m, &s, 1000);                   /* IRQ_ACK：MISO = PONG（上拍应答） */
    step(&m, &s, 1000);                   /* IRQ_ACK：MISO = ERROR 0x04 交付 */
    CHECK(!s.aerr, "sc5 aerr cleared（交付沿）\n");
    step(&m, &s, 1000);                   /* IRQ 低 → 空闲（无 RECOVERY） */
    CHECK(m.state == SPIM_LINKED && m.stats.recoveries == 0,
          "sc5 still linked\n");
    CHECK(m.stats.legal_frames == 6, "sc5 legal %u\n", m.stats.legal_frames);

    /* 哨兵污染回归（审查 Important-1 的证伪断言）：异步 ERROR 之后
     * 交付一帧完整事件——§3.5 明文异步 ERROR 不入 gap 链，基线不得
     * 被 0x0000 哨兵污染；旧实现（track_event_seq(0)）下此处必红：
     * ERROR 记 1 假 gap 且基线污染后再记 1，seq_gaps == 2。 */
    CHECK(m.stats.seq_gaps == 0, "sc5 no sentinel gaps %u\n", m.stats.seq_gaps);
    push_frame(&s, 0x33);
    step(&m, &s, 1000);                   /* IRQ_ACK：MISO=全 0，装 DESC */
    step(&m, &s, 1000);                   /* DESC 交付 */
    step(&m, &s, 1000);                   /* chunk1 */
    step(&m, &s, 1000);                   /* chunk2 → 完整事件闭环 */
    CHECK(m.stats.seq_gaps == 0, "sc5 post-error gaps %u\n", m.stats.seq_gaps);
    CHECK(m.stats.uat_complete >= 1, "sc5 post-error uat\n");
}

static void sc6_version_mismatch(void)
{
    spi_master_t m; ms_t s;
    spi_master_init(&m, uat_capture, NULL);
    ms_pwr_reset(&s);
    s.bad_hello_ver = true;
    g_now = 0;

    step(&m, &s, 1000);
    step(&m, &s, 500000);                 /* HELLO(ver=2) → ERR_VERSION */
    CHECK(m.state == SPIM_WAIT_HELLO, "sc6 not linked\n");
    CHECK(m.stats.version_mismatch == 1, "sc6 vm %u\n",
          m.stats.version_mismatch);
    step(&m, &s, 500000);                 /* 500 ms 后重试 */
    CHECK(m.state == SPIM_WAIT_HELLO, "sc6 still waiting\n");
}

static void sc7_seq_gap(void)
{
    spi_master_t m; ms_t s;
    spi_master_init(&m, uat_capture, NULL);
    ms_pwr_reset(&s);
    g_now = 0;
    do_handshake(&m, &s);

    push_frame(&s, 1);                    /* 帧 1：建立事件基线（跳一次） */
    step(&m, &s, 1000);
    step(&m, &s, 1000);
    step(&m, &s, 1000);
    step(&m, &s, 1000);
    CHECK(m.stats.seq_gaps == 0, "sc7 baseline skip\n");

    push_frame(&s, 2);
    s.ev_seq = (uint16_t)(s.ev_seq + 5);  /* 注入跳变（slave 内部丢弃） */
    step(&m, &s, 1000);                   /* DESC（seq 跳 +5）→ 1 gap */
    step(&m, &s, 1000);
    step(&m, &s, 1000);
    step(&m, &s, 1000);
    CHECK(m.stats.seq_gaps == 1, "sc7 gaps %u\n", m.stats.seq_gaps);
}

static void sc8_chunk_interrupted(void)
{
    spi_master_t m; ms_t s;
    spi_master_init(&m, uat_capture, NULL);
    ms_pwr_reset(&s);
    g_now = 0;
    cap_n = 0;

    /* LINKED 瞬间队列已有帧：RF_CONFIG（§6.4 首务优先于 IRQ_ACK，§6.7
     * "至多完成在途应答"窗口）打断取片流，分片保持不丢失
     * （§2.3 规则 1 括号：命令非 IRQ_ACK 时不适用）。 */
    step(&m, &s, 1000);                   /* T1 HELLO（MISO 全 0） */
    push_frame(&s, 0x5A);                 /* T1 与 T2 之间事件到达 */
    step(&m, &s, 500000);                 /* T2 → LINKED（rf_pending 置位） */
    step(&m, &s, 1000);                   /* T3 RF_CONFIG（在途应答窗口） */
    step(&m, &s, 1000);                   /* T4 IRQ_ACK：MISO=RF_STATUS，装 DESC */
    step(&m, &s, 1000);                   /* S1 IRQ_ACK：MISO=DESC */
    step(&m, &s, 1000);                   /* S2 chunk1 */
    step(&m, &s, 1000);                   /* S3 chunk2 → 完成 */
    step(&m, &s, 1000);                   /* S4 队列空、IRQ 低 → 空闲 */
    CHECK(cap_n == 1 && cap_len == UAT_LEN, "sc8 uat %d\n", cap_n);
    CHECK(cap_frame[0] == 0x5A && cap_frame[551] == (uint8_t)(0x5A + 551),
          "sc8 bytes\n");
    CHECK(m.stats.seq_gaps == 0, "sc8 gaps\n");
}

int main(void)
{
    sc1_handshake();
    sc2_split_brain();
    sc3_event_flow();
    sc4_queue_full_refill();
    sc5_async_ping_race();
    sc6_version_mismatch();
    sc7_seq_gap();
    sc8_chunk_interrupted();
    if (g_fail) { printf("test_spi_master: %d FAIL\n", g_fail); return 1; }
    printf("test_spi_master: all OK\n");
    return 0;
}
