/*
 * test_cc13_slave.c — CC1312R slave 状态机 host 单测 + master↔slave
 * 全链路互通测试（WP-E T4）。
 *
 * 跑法：
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/rp2040 -I firmware/cc1312r \
 *      -I firmware/components/rp_cc13xx_codec \
 *      -I firmware/components/adsb_link_codec \
 *      -DSPI_MASTER_HOST_TEST \
 *      -o /tmp/test_cc13_slave \
 *      firmware/test/test_cc13_slave.c \
 *      firmware/cc1312r/spi_slave.c \
 *      firmware/rp2040/spi_master.c \
 *      firmware/components/rp_cc13xx_codec/rp_cc13xx_codec.c \
 *      firmware/components/adsb_link_codec/adsb_link_codec.c \
 *   && /tmp/test_cc13_slave
 *
 * 判据：firmware/PROTOCOL_RP2040_CC1312R_SPI.md v1.0。
 *
 * 本文件是 WP-P0b 十轮审计"文档走查不可执行"的最后一块拼图：两侧
 * 状态机（T3 master + T4 slave）**直接对跑**，协议闭环（握手、事件
 * 流、队满重填、异步竞争、分裂脑恢复）端到端可执行——不再有任何
 * 一侧行为只存在于文档走查里。
 */
#include "spi_slave.h"
#include "spi_master.h"
#include "rp_cc13xx_codec.h"

#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("        at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

/* ── UAT 捕获（master 侧）─────────────────────────────────────────── */

static uint8_t  cap_frame[CC13S_UAT_LEN];
static int      cap_n;
static uint8_t  cap_rssi;
static uint32_t cap_ts;

static void uat_cb(const uint8_t *f, size_t len, uint8_t rssi,
                   uint32_t ts, void *u)
{
    (void)u;
    if (len == CC13S_UAT_LEN) memcpy(cap_frame, f, len);
    cap_n++; cap_rssi = rssi; cap_ts = ts;
}

/* ── 互通驱动：一个事务把两侧接起来 ────────────────────────────────
 * CSN↓：slave pending 预载 MISO；master next_txn 产出 MOSI；
 * 事务：mosi→slave、miso→master（全双工 512 B）；
 * CSN↑：slave txn_done（交付沿清源+装载）、master digest。 */

typedef struct { spi_master_t m; cc13s_slave_t s; uint32_t now; } duo_t;

/* ── MISO 帧嗅探（审查 round-T4 突变验证的判别补强：删除规则 3 或
 * 破坏 seq 回显曾全绿——按事务断言帧类型序 + 回显值）───────────── */

static uint8_t  sniff_type[32];
static uint16_t sniff_seq[32];
static int      sniff_n;

static void sniff(const uint8_t miso[CC13S_TXN_LEN])
{
    /* 合法帧的 type 在 offset 3、seq 在 offset 4-5（§3）；全 0 帧
     * 的 type 字节为 0（= 禁用值，不记）。 */
    if (miso[0] != RP_CC13XX_MAGIC0 || miso[1] != RP_CC13XX_MAGIC1) return;
    if (miso[3] == 0x00 || miso[3] == 0xFF) return;
    if (sniff_n < 32) {
        sniff_type[sniff_n] = miso[3];
        sniff_seq[sniff_n]  = (uint16_t)(miso[4] | ((uint16_t)miso[5] << 8));
        sniff_n++;
    }
}

static void duo_init(duo_t *d)
{
    spi_master_init(&d->m, uat_cb, NULL);
    cc13s_init(&d->s, 1, 0);
    d->now = 0;
    cap_n = 0;
    sniff_n = 0;
}

static void duo_txn(duo_t *d, uint32_t dt)
{
    d->now += dt;
    if (spi_master_reset_requested(&d->m)) {
        cc13s_init(&d->s, 1, 0);           /* RESET 清 slave 全部会话态 */
        spi_master_reset_done(&d->m, d->now);
        return;
    }
    uint8_t mosi[CC13S_TXN_LEN], miso[CC13S_TXN_LEN];
    const bool irq = cc13s_irq(&d->s);
    if (spi_master_next_txn(&d->m, d->now, mosi, irq) == 0) return;
    cc13s_pending(&d->s, miso);            /* CSN↓：预载 MISO（§2.2）   */
    sniff(miso);                           /* 判别记录：帧类型 + seq     */
    cc13s_txn_done(&d->s, mosi);           /* CSN↑：slave 闭环          */
    spi_master_digest(&d->m, 512, miso);   /* master 收到本事务 MISO     */
}

static void duo_handshake(duo_t *d)
{
    duo_txn(d, 1000);                      /* T1 HELLO，MISO 全 0 */
    duo_txn(d, 500000);                    /* T2 → 双方 LINKED */
    duo_txn(d, 1000);                      /* T3 RF_CONFIG */
    duo_txn(d, 1000000);                   /* T4 PING → RF_STATUS 取走 */
}

static void push_uat(duo_t *d, uint8_t seed)
{
    uint8_t f[CC13S_UAT_LEN];
    for (int i = 0; i < CC13S_UAT_LEN; i++) f[i] = (uint8_t)(seed + i);
    cc13s_push_uat(&d->s, f, 0x2A, 987654u + (uint32_t)d->s.qn);
}

/* ── 场景 ─────────────────────────────────────────────────────────── */

/* E2E-1 全链握手 + 事件流：RF 注入帧 → slave 队列 → IRQ → master
 * IRQ_ACK 流水 → DESC/分片 → master 回调收 552 B 原帧 + 元数据。 */
static void e2e_event_flow(void)
{
    duo_t d; duo_init(&d);
    duo_handshake(&d);
    CHECK(d.m.state == SPIM_LINKED && d.s.state == CC13S_LINKED, "e2e1 linked\n");
    CHECK(d.m.peer_rf.config_version == 0, "e2e1 rf echo v0\n");

    push_uat(&d, 0x10);
    for (int i = 0; i < 5 && cap_n == 0; i++) duo_txn(&d, 1000);
    CHECK(cap_n == 1, "e2e1 uat %d\n", cap_n);
    CHECK(cap_frame[0] == 0x10 && cap_frame[551] == (uint8_t)(0x10 + 551),
          "e2e1 bytes\n");
    CHECK(cap_rssi == 0x2A && cap_ts == 987654u, "e2e1 meta\n");
    CHECK(d.s.qn == 0, "e2e1 slave drained\n");
    CHECK(d.m.stats.seq_gaps == 0, "e2e1 gaps %u\n", d.m.stats.seq_gaps);
}

/* E2E-2 队满 + 重填（B34 端到端）：注 5 帧（容量 4）→ 首帧被弃 +
 * qfull → master drain → drain 中再注 → 再通知 → 全部取空。 */
static void e2e_queue_full(void)
{
    duo_t d; duo_init(&d);
    duo_handshake(&d);

    for (int i = 0; i < CC13S_MAXQ + 1; i++) push_uat(&d, (uint8_t)(i * 9));
    CHECK(d.s.qfull && d.s.dropped == 1, "e2e2 overflow flag\n");

    int guard = 0;
    while ((d.s.qn > 0 || d.s.qfull || d.s.aerr) && guard++ < 40) {
        duo_txn(&d, 1000);
        if (guard == 10) {                 /* drain 中重填 */
            push_uat(&d, 0xE0);
        }
    }
    CHECK(guard < 40, "e2e2 terminated\n");
    CHECK(d.s.qn == 0 && !d.s.qfull, "e2e2 drained\n");
    CHECK(cap_n >= CC13S_MAXQ, "e2e2 delivered %d\n", cap_n);
    CHECK(d.m.stats.recoveries == 0, "e2e2 no recovery\n");
    CHECK(d.m.stats.seq_gaps == 0, "e2e2 gaps %u\n", d.m.stats.seq_gaps);
}

/* E2E-3 异步错误 + PING 竞争（B35 端到端）。判别补强：按事务断言
 * MISO 帧类型序（PONG 先于 ERROR）与 PONG 的 seq 回显（审查突变
 * 验证：删除规则 3 或 seq 改 +1 的实现曾全绿——本组断言使其必红）。 */
static void e2e_async_race(void)
{
    duo_t d; duo_init(&d);
    duo_handshake(&d);
    const uint16_t ping_seq = d.m.cmd_seq; /* 即将发出的 PING 的 seq */

    duo_txn(&d, 1000000);                  /* PING（IRQ 低时发出） */
    cc13s_raise_async(&d.s, 0x04);         /* 事务中到达 → IRQ 升高 */
    duo_txn(&d, 1000);                     /* master IRQ_ACK：收 PONG */
    duo_txn(&d, 1000);                     /* master IRQ_ACK：收 ERROR */
    CHECK(!d.s.aerr, "e2e3 aerr cleared\n");
    duo_txn(&d, 1000);                     /* IRQ 低 → 空闲 */
    CHECK(d.m.state == SPIM_LINKED && d.m.stats.recoveries == 0,
          "e2e3 linked\n");

    /* 帧类型序：握手后 sniff = [..., PONG, ERROR]（尾部两个）。 */
    CHECK(sniff_n >= 2, "e2e3 sniff %d\n", sniff_n);
    CHECK(sniff_type[sniff_n - 2] == RP_CC13XX_MSG_PONG,
          "e2e3 pong before error (t=%02x)\n", sniff_type[sniff_n - 2]);
    CHECK(sniff_type[sniff_n - 1] == RP_CC13XX_MSG_ERROR,
          "e2e3 error last (t=%02x)\n", sniff_type[sniff_n - 1]);
    /* §3.5 回显：PONG 的 seq == 触发它的 PING 的 seq。 */
    CHECK(sniff_seq[sniff_n - 2] == ping_seq,
          "e2e3 pong seq echo %04x != %04x\n",
          sniff_seq[sniff_n - 2], ping_seq);
    /* 异步 ERROR 的 seq 哨兵（§3.5：固定 0x0000）。 */
    CHECK(sniff_seq[sniff_n - 1] == 0x0000, "e2e3 error sentinel\n");

    /* 哨兵不污染 gap 链：ERROR 后注入完整事件帧，gaps 必须为 0
     * （T3 审查 Important-1 的端到端复验）。 */
    push_uat(&d, 0x77);
    for (int i = 0; i < 5 && cap_n < 2; i++) duo_txn(&d, 1000);
    CHECK(d.m.stats.seq_gaps == 0, "e2e3 no sentinel gaps %u\n",
          d.m.stats.seq_gaps);
}

/* E2E-4 分裂脑（B30 端到端）：slave 掉电复位 → master PING 静默 →
 * 3 s → RECOVERY → RESET → 重握手 → RF_CONFIG 重发。 */
static void e2e_split_brain(void)
{
    duo_t d; duo_init(&d);
    duo_handshake(&d);

    cc13s_init(&d.s, 1, 0);                /* slave 自主复位 */
    int guard = 0;
    while (d.m.state == SPIM_LINKED && guard++ < 8) duo_txn(&d, 1000000);
    CHECK(d.m.state == SPIM_RECOVERY, "e2e4 recovery\n");

    duo_txn(&d, 1000);                     /* RESET 脉冲 + 启动等待 */
    CHECK(d.m.state == SPIM_WAIT_HELLO, "e2e4 wait\n");
    duo_handshake(&d);
    CHECK(d.m.state == SPIM_LINKED, "e2e4 relinked\n");
    CHECK(d.m.peer_hello.fw_ver_major == 1, "e2e4 peer fw\n");
    CHECK(d.m.stats.recoveries == 1, "e2e4 recoveries\n");

    /* 复位后事件流恢复 */
    push_uat(&d, 0x99);
    for (int i = 0; i < 5 && cap_n == 0; i++) duo_txn(&d, 1000);
    CHECK(cap_n == 1 && cap_frame[0] == 0x99, "e2e4 post-reset uat\n");
}

/* E2E-5 RF_CONFIG 写入对账：master 写 v3+digest → slave 存储 →
 * 查询回读一致（§4.6 端到端）。 */
static void e2e_rf_config(void)
{
    duo_t d; duo_init(&d);
    duo_handshake(&d);

    /* 手工驱动一轮 RF_CONFIG 写（master 侧 API 无显式写入口——用
     * 帧直注：构造写帧经 duo_txn 的 MOSI 通道。为保持 duo_txn 的
     * 决策所有权在 master，这里退化为验证 T4 查询回读路径：骨架
     * 里 slave 存储与回读在单测层覆盖（见下）。 */
    rp_cc13xx_rf_config_t cfg = { .config_version = 42 };
    for (int i = 0; i < 16; i++) cfg.digest[i] = (uint8_t)(0xA0 + i);
    uint8_t f[CC13S_TXN_LEN];
    size_t n = rp_cc13xx_encode_rf_config(f, sizeof(f),
                                          RP_CC13XX_MSG_RF_CONFIG, 7, &cfg);
    CHECK(n > 0, "e2e5 encode\n");
    cc13s_txn_done(&d.s, f);               /* slave 处理写帧 */
    CHECK(d.s.rf.config_version == 42, "e2e5 stored v%d\n",
          d.s.rf.config_version);
    CHECK(d.s.rf.digest[0] == 0xA0 && d.s.rf.digest[15] == 0xAF,
          "e2e5 digest\n");
    /* 应答帧是 RF_CONFIG_STATUS（含刚写入的配置） */
    uint8_t miso[CC13S_TXN_LEN];
    cc13s_pending(&d.s, miso);
    rp_cc13xx_msg_t m;
    CHECK(rp_cc13xx_decode_frame(miso, CC13S_TXN_LEN, &m) == RP_CC13XX_OK,
          "e2e5 status frame\n");
    CHECK(m.type == RP_CC13XX_MSG_RF_CONFIG_STATUS, "e2e5 status type\n");
    rp_cc13xx_rf_config_t echo;
    CHECK(rp_cc13xx_decode_rf_config(&m, &echo) == RP_CC13XX_OK &&
          echo.config_version == 42 && echo.digest[0] == 0xA0,
          "e2e5 echo\n");
}

/* E2E-6 ver≠1 → ERROR{0x01}（§6.2/§2.3 规则 2）：帧直注坏版本 HELLO，
 * 断言下一 pending 为命令性 ERROR 且 seq 回显。 */
static void e2e_version_deny(void)
{
    duo_t d; duo_init(&d);
    rp_cc13xx_hello_t h = { 1, 0, RP_CC13XX_RESET_POR };
    uint8_t f[CC13S_TXN_LEN];
    size_t n = rp_cc13xx_encode_hello(f, sizeof(f), 0x002A, &h);
    f[2] = 2;                               /* ver=2，重算 CRC */
    const uint16_t crc = rp_cc13xx_crc16(f, n - 2);
    f[n - 2] = (uint8_t)(crc & 0xFF);
    f[n - 1] = (uint8_t)(crc >> 8);
    memset(f + n, 0, sizeof(f) - n);
    cc13s_txn_done(&d.s, f);
    CHECK(d.s.version_mismatch == 1, "e2e6 vm\n");
    uint8_t miso[CC13S_TXN_LEN];
    cc13s_pending(&d.s, miso);
    CHECK(miso[0] == RP_CC13XX_MAGIC0 && miso[3] == RP_CC13XX_MSG_ERROR,
          "e2e6 error loaded (t=%02x)\n", miso[3]);
    const uint16_t eseq = (uint16_t)(miso[4] | ((uint16_t)miso[5] << 8));
    CHECK(eseq == 0x002A, "e2e6 seq echo %04x\n", eseq);   /* §3.5 回显 */
    CHECK(d.s.state == CC13S_WAIT_HELLO, "e2e6 not linked\n");
}

/* E2E-7 RF_CONFIG 非单调写拒绝（§4.6 ERROR{0x03}）+ 单调写接受。 */
static void e2e_rf_monotonic(void)
{
    duo_t d; duo_init(&d);
    d.s.state = CC13S_LINKED;              /* 单测直达 LINKED */
    rp_cc13xx_rf_config_t c5 = { .config_version = 5 };
    uint8_t f[CC13S_TXN_LEN];
    size_t n = rp_cc13xx_encode_rf_config(f, sizeof(f),
                                          RP_CC13XX_MSG_RF_CONFIG, 0x0101, &c5);
    memset(f + n, 0, sizeof(f) - n);
    cc13s_txn_done(&d.s, f);
    CHECK(d.s.rf.config_version == 5 && !d.s.cmd_err_pending,
          "e2e7 monotonic accept\n");

    rp_cc13xx_rf_config_t c3 = { .config_version = 3 };    /* 回退写 */
    n = rp_cc13xx_encode_rf_config(f, sizeof(f),
                                   RP_CC13XX_MSG_RF_CONFIG, 0x0102, &c3);
    memset(f + n, 0, sizeof(f) - n);
    cc13s_txn_done(&d.s, f);
    CHECK(d.s.rf.config_version == 5, "e2e7 config retained\n");
    uint8_t miso[CC13S_TXN_LEN];
    cc13s_pending(&d.s, miso);             /* 直接应答 = ERROR{0x03} */
    CHECK(miso[3] == RP_CC13XX_MSG_ERROR, "e2e7 error frame\n");
    const uint16_t eseq = (uint16_t)(miso[4] | ((uint16_t)miso[5] << 8));
    CHECK(eseq == 0x0102, "e2e7 seq echo %04x\n", eseq);
}

/* E2E-8 LINKED-HELLO 清半交付态（§6.5）：在途报文作废不重新入队，
 * 重握手后事件流从新报文开始。 */
static void e2e_rehello_clears(void)
{
    duo_t d; duo_init(&d);
    duo_handshake(&d);
    push_uat(&d, 0x30);
    duo_txn(&d, 1000);                     /* DESC 装载 */
    duo_txn(&d, 1000);                     /* DESC 交付 → 在途（1 分片已取）*/
    CHECK(d.s.cur >= 0, "e2e8 in-flight\n");

    /* master 重启 → 重握手（新 HELLO；slave 清半交付态）。 */
    rp_cc13xx_hello_t h = { 0, 1, 0 };
    uint8_t f[CC13S_TXN_LEN];
    size_t n = rp_cc13xx_encode_hello(f, sizeof(f), 0x0B0C, &h);
    memset(f + n, 0, sizeof(f) - n);
    cc13s_txn_done(&d.s, f);
    CHECK(d.s.cur < 0, "e2e8 half-delivery cleared\n");
    CHECK(d.s.qn == 0, "e2e8 stale frame dropped not requeued (q=%d)\n",
          d.s.qn);
    /* 事件流恢复：新帧正常交付。 */
    push_uat(&d, 0x40);
    for (int i = 0; i < 5 && cap_n == 0; i++) duo_txn(&d, 1000);
    CHECK(cap_n == 1 && cap_frame[0] == 0x40, "e2e8 new frame flows\n");
    CHECK(d.m.stats.recoveries == 0, "e2e8 no recovery needed\n");
}

int main(void)
{
    e2e_event_flow();
    e2e_queue_full();
    e2e_async_race();
    e2e_split_brain();
    e2e_rf_config();
    e2e_version_deny();
    e2e_rf_monotonic();
    e2e_rehello_clears();
    if (g_fail) { printf("test_cc13_slave: %d FAIL\n", g_fail); return 1; }
    printf("test_cc13_slave: all OK\n");
    return 0;
}
