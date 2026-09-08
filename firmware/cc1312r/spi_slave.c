/*
 * spi_slave.c — CC1312R 侧 SPI slave 状态机（纯逻辑）。
 *
 * 由 WP-E T3 的 mock slave（test_spi_master.c 内嵌参考实现）提炼为
 * 正式实现——两者结构同源，本文件是权威；mock 保留在测试里作为
 * master 侧测试的独立陪练（同源风险以 master↔slave 全链互通测试
 * + golden vectors 双向钉死来对冲）。
 *
 * 规范条款逐条对照见 spi_slave.h 头注与各函数内注释。
 */
#include "spi_slave.h"

#include <string.h>

static void load_frame(cc13s_slave_t *s, const uint8_t *frame, size_t n,
                       int kind)
{
    memset(s->pend, 0, sizeof(s->pend));
    if (frame && n) memcpy(s->pend, frame, n < CC13S_TXN_LEN ? n : CC13S_TXN_LEN);
    s->pend_kind  = kind;
    s->pend_dlen  = 0;
    s->pend_final = false;
    s->pend_async = false;
}

static void load_all_zero(cc13s_slave_t *s)
{
    load_frame(s, NULL, 0, 0);
}

static void pop_queue(cc13s_slave_t *s)
{
    if (s->qn <= 0) { s->cur = -1; return; }
    memmove(s->q, s->q + 1, sizeof(s->q[0]) * (size_t)(s->qn - 1));
    memmove(s->q_rssi, s->q_rssi + 1, sizeof(s->q_rssi[0]) * (size_t)(s->qn - 1));
    memmove(s->q_ts, s->q_ts + 1, sizeof(s->q_ts[0]) * (size_t)(s->qn - 1));
    s->qn--;
    s->cur = -1;
}

void cc13s_init(cc13s_slave_t *s, uint16_t fw_major, uint16_t fw_minor)
{
    memset(s, 0, sizeof(*s));
    s->cur        = -1;
    s->state      = CC13S_WAIT_HELLO;
    s->fw_major   = fw_major;
    s->fw_minor   = fw_minor;
    s->reset_reason = RP_CC13XX_RESET_POR;
}

void cc13s_pending(const cc13s_slave_t *s, uint8_t miso[CC13S_TXN_LEN])
{
    memcpy(miso, s->pend, CC13S_TXN_LEN);
}

bool cc13s_irq(const cc13s_slave_t *s)
{
    /* §1 不变式：IRQ = 四源之或，严格 iff（审计 round-WP-E-1 P2-1：
     * 不按状态短路——生产路径上 WAIT_HELLO 态源恒空【复位清一切】，
     * iff 自然成立；若测试注入造成"未握手却有源"，iff 语义也保持
     * 诚实：源在即报。WAIT_HELLO 的 master 行为不受影响——master
     * 在该态只发 HELLO，不看 IRQ【§6.2】）。 */
    if (s->qfull || s->aerr) return true;
    if (s->cur >= 0 && s->cur_sent < s->cur_total) return true;
    return s->qn > 0;
}

void cc13s_push_uat(cc13s_slave_t *s, const uint8_t frame[CC13S_UAT_LEN],
                    uint8_t rssi, uint32_t ts_us)
{
    if (s->qn >= CC13S_MAXQ) {
        /* §4.5：队满丢弃新帧（或按实现丢弃队头——本实现弃新，与
         * events_dropped 口径一致）、置 qfull 通知 master。 */
        s->dropped++;
        s->qfull = true;
        return;
    }
    memcpy(s->q[s->qn], frame, CC13S_UAT_LEN);
    s->q_rssi[s->qn] = rssi;
    s->q_ts[s->qn]   = ts_us;
    s->qn++;
}

void cc13s_raise_async(cc13s_slave_t *s, uint8_t code)
{
    (void)code;
    s->aerr = true;                /* code 存入装载时的 ERROR 帧（0x04/0x05）*/
    s->async_code = code;
}

/* §2.3 五步装载链。 */
static void load_next(cc13s_slave_t *s, const rp_cc13xx_msg_t *in, bool cmd_ok)
{
    /* WAIT_HELLO 应答策略（§6.2 集中裁决）：HELLO 应答 / PING 静默 /
     * ERROR 计数不装载 / 其余 prelink_reject；ver≠1 拒收的
     * ERROR{0x01}（规则 2 槽位，txn_done 帧级设置）优先装载。 */
    if (s->state == CC13S_WAIT_HELLO) {
        if (s->cmd_err_pending) {
            rp_cc13xx_error_t e = { .code = s->cmd_err_code, .msg_len = 3,
                                    .msg = { 'D','E','N' } };
            uint8_t f[CC13S_TXN_LEN];
            size_t n = rp_cc13xx_encode_error(f, sizeof(f),
                                              s->cmd_err_seq, &e);
            load_frame(s, f, n, RP_CC13XX_MSG_ERROR);
            s->cmd_err_pending = false;
            return;
        }
        if (cmd_ok && in->type == RP_CC13XX_MSG_HELLO &&
            in->payload_len == RP_CC13XX_HELLO_LEN) {
            /* ver 检查在帧级完成（codec ERR_VERSION）；对端 ver=1 的
             * HELLO → 应答 + 双方 LINKED。 */
            rp_cc13xx_hello_t h = { .fw_ver_major = s->fw_major,
                                    .fw_ver_minor = s->fw_minor,
                                    .reset_reason = s->reset_reason };
            uint8_t f[CC13S_TXN_LEN];
            size_t n = rp_cc13xx_encode_hello(f, sizeof(f), in->seq, &h);
            load_frame(s, f, n, RP_CC13XX_MSG_HELLO);
            s->state = CC13S_LINKED;    /* 握手段不删（§6.2）*/
        } else {
            if (cmd_ok && in->type == RP_CC13XX_MSG_ERROR) {
                /* ERROR → 仅计数、不装载应答（§6.2）*/
            } else if (cmd_ok && in->type != RP_CC13XX_MSG_PING) {
                s->prelink_reject++;     /* 业务/查询/写 → 整事务作废；
                                          * PING 属静默，不在此列（§6.2）*/
            }
            load_all_zero(s);
        }
        return;
    }

    /* 规则 1：取片延续（命令为 IRQ_ACK 且报文有剩余）——优先于一切。 */
    if (cmd_ok && in->type == RP_CC13XX_MSG_IRQ_ACK &&
        s->cur >= 0 && s->cur_sent < s->cur_total) {
        rp_cc13xx_chunk_t c = {
            .desc_id   = s->cur_desc_id,
            .offset    = s->cur_sent,
            .total_len = s->cur_total,
        };
        const uint16_t left = (uint16_t)(s->cur_total - s->cur_sent);
        c.data_len = left > RP_CC13XX_CHUNK_MAX_DATA
                   ? RP_CC13XX_CHUNK_MAX_DATA : left;
        memcpy(c.data, s->q[s->cur] + s->cur_sent, c.data_len);
        uint8_t f[CC13S_TXN_LEN];
        size_t n = rp_cc13xx_encode_rx_chunk(f, sizeof(f), s->ev_seq++, &c);
        load_frame(s, f, n, RP_CC13XX_MSG_RX_PAYLOAD_CHUNK);
        s->pend_dlen  = c.data_len;
        s->pend_final = (uint16_t)(s->cur_sent + c.data_len) >= s->cur_total;
        return;
    }
    /* 规则 2：顺延的命令性 ERROR——无后备队列，被挤掉即永久丢失
     *（§2.3：与取片延续同优先级段，直接应答之前）。 */
    if (s->cmd_err_pending) {
        rp_cc13xx_error_t e = { .code = s->cmd_err_code, .msg_len = 3,
                                .msg = { 'D','E','N' } };
        uint8_t f[CC13S_TXN_LEN];
        size_t n = rp_cc13xx_encode_error(f, sizeof(f), s->cmd_err_seq, &e);
        load_frame(s, f, n, RP_CC13XX_MSG_ERROR);   /* 命令性：非 async */
        s->cmd_err_pending = false;
        return;
    }
    if (cmd_ok && in->type == RP_CC13XX_MSG_HELLO) {
        /* LINKED 中收到合法 HELLO = 会话重建信号（§6.5）：清半交付态
         * ——在途报文作废、不重新入队（master 重启即放弃该报文），
         * 事件队列与 RF 配置保留。 */
        rp_cc13xx_hello_t h = { .fw_ver_major = s->fw_major,
                                .fw_ver_minor = s->fw_minor,
                                .reset_reason = s->reset_reason };
        uint8_t f[CC13S_TXN_LEN];
        size_t n = rp_cc13xx_encode_hello(f, sizeof(f), in->seq, &h);
        load_frame(s, f, n, RP_CC13XX_MSG_HELLO);
        if (s->cur >= 0) pop_queue(s);   /* §6.5：半交付报文作废出队 */
        return;
    }
    if (cmd_ok && in->type == RP_CC13XX_MSG_PING) {
        uint8_t f[CC13S_TXN_LEN];
        size_t n = rp_cc13xx_encode_empty(f, sizeof(f),
                                          RP_CC13XX_MSG_PONG, in->seq);
        load_frame(s, f, n, RP_CC13XX_MSG_PONG);        /* 规则 3 */
        return;
    }
    if (cmd_ok && in->type == RP_CC13XX_MSG_RF_CONFIG) {
        rp_cc13xx_rf_config_t in_cfg;
        bool deny = false;
        if (rp_cc13xx_decode_rf_config(in, &in_cfg) == RP_CC13XX_OK) {
            if (in_cfg.config_version == 0) {
                /* §4.6：0 = 只读查询 → 回当前配置。 */
            } else if (in_cfg.config_version > s->rf.config_version ||
                       s->rf.config_version == 0) {
                s->rf = in_cfg;          /* 单调递增写：接受并存储（§4.6）*/
            } else {
                deny = true;             /* §4.6：非单调写 = 拒绝——直接
                                          * 应答 ERROR{0x03}（seq 回显），
                                          * 保留现行配置，不装 STATUS。 */
            }
        }
        uint8_t f[CC13S_TXN_LEN];
        size_t n;
        int    kind;
        if (deny) {
            rp_cc13xx_error_t e = { .code = 0x03, .msg_len = 3,
                                    .msg = { 'D','E','N' } };
            n = rp_cc13xx_encode_error(f, sizeof(f), in->seq, &e);
            kind = RP_CC13XX_MSG_ERROR;
        } else {
            n = rp_cc13xx_encode_rf_config(f, sizeof(f),
                                           RP_CC13XX_MSG_RF_CONFIG_STATUS,
                                           in->seq, &s->rf);
            kind = RP_CC13XX_MSG_RF_CONFIG_STATUS;
        }
        load_frame(s, f, n, kind);
        return;
    }
    if (cmd_ok && in->type == RP_CC13XX_MSG_RESET_STATUS_REQ) {
        rp_cc13xx_reset_status_t st = { .reset_reason = s->reset_reason,
                                        .uptime_ms = 0 };
        uint8_t f[CC13S_TXN_LEN];
        size_t n = rp_cc13xx_encode_reset_status(f, sizeof(f), in->seq, &st);
        load_frame(s, f, n, RP_CC13XX_MSG_RESET_STATUS);
        return;
    }
    if (cmd_ok && in->type == RP_CC13XX_MSG_UPGRADE_STATUS_REQ) {
        rp_cc13xx_upgrade_status_t u = { .state = RP_CC13XX_UPG_NORMAL,
                                         .running_image_version = 0 };
        uint8_t f[CC13S_TXN_LEN];
        size_t n = rp_cc13xx_encode_upgrade_status(f, sizeof(f), in->seq, &u);
        load_frame(s, f, n, RP_CC13XX_MSG_UPGRADE_STATUS);
        return;
    }
    if (s->qfull) {                       /* 规则 4a：通知优先（§4.5） */
        rp_cc13xx_queue_full_t q = { .events_dropped = (uint16_t)s->dropped,
                                     .queue_depth = (uint8_t)s->qn };
        uint8_t f[CC13S_TXN_LEN];
        size_t n = rp_cc13xx_encode_queue_full(f, sizeof(f),
                                               s->ev_seq++, &q);
        load_frame(s, f, n, RP_CC13XX_MSG_QUEUE_FULL);
        return;
    }
    if (s->qn > 0 && s->cur < 0) {        /* 规则 4b：队头事件 → DESC */
        rp_cc13xx_rx_desc_t d = {
            .desc_id   = (uint16_t)(s->last_desc_id + 1),
            .freq_hz   = 978000000u,
            .ts_us     = s->q_ts[0],
            .total_len = CC13S_UAT_LEN,
            .rssi      = s->q_rssi[0],
        };
        uint8_t f[CC13S_TXN_LEN];
        size_t n = rp_cc13xx_encode_rx_descriptor(f, sizeof(f),
                                                  s->ev_seq++, &d);
        load_frame(s, f, n, RP_CC13XX_MSG_RX_DESCRIPTOR);
        s->last_desc_id = d.desc_id;
        s->cur          = 0;
        s->cur_desc_id  = d.desc_id;
        s->cur_sent     = 0;
        s->cur_total    = CC13S_UAT_LEN;
        return;
    }
    if (s->aerr) {                        /* 规则 4c：异步 ERROR（seq 哨兵） */
        rp_cc13xx_error_t e = { .code = s->async_code ? s->async_code : 0x04,
                                .msg_len = 4, .msg = { 'F','W','E','R' } };
        uint8_t f[CC13S_TXN_LEN];
        size_t n = rp_cc13xx_encode_error(f, sizeof(f), 0x0000, &e);
        load_frame(s, f, n, RP_CC13XX_MSG_ERROR);
        s->pend_async = true;             /* 交付沿清 aerr（§1）          */
        return;
    }
    load_all_zero(s);                     /* 规则 5 */
}

void cc13s_txn_done(cc13s_slave_t *s, const uint8_t mosi[CC13S_TXN_LEN])
{
    s->txns++;

    /* 1) 交付沿清源（§1：各源于携带其帧的事务 CSN 上升沿清除——
     * 装载不清除，已装载未交付的帧仍算 IRQ 置位）。pend_async 区分
     * 异步 ERROR（清 aerr）与命令性 ERROR（规则 2 槽位，无源可清）。 */
    const int      kind = s->pend_kind;
    const uint16_t dd   = s->pend_dlen;
    const bool     fin  = s->pend_final;
    const bool     asyn = s->pend_async;
    if (kind == RP_CC13XX_MSG_QUEUE_FULL) s->qfull = false;
    if (kind == RP_CC13XX_MSG_ERROR && asyn) s->aerr = false;
    if (kind == RP_CC13XX_MSG_RX_PAYLOAD_CHUNK) {
        s->cur_sent = (uint16_t)(s->cur_sent + dd);
        if (fin) pop_queue(s);
    }

    /* 2) 处理 MOSI（帧级校验：magic→len→CRC→ver，§5.1）。 */
    rp_cc13xx_msg_t in;
    const rp_cc13xx_status_t st =
        rp_cc13xx_decode_frame(mosi, CC13S_TXN_LEN, &in);
    bool cmd_ok = false;
    switch (st) {
    case RP_CC13XX_OK:
        s->legal_frames++;
        cmd_ok = true;
        break;
    case RP_CC13XX_NO_FRAME:   s->no_frame_txns++;   break;
    case RP_CC13XX_ERR_MAGIC:  s->resyncs++;         break;
    case RP_CC13XX_ERR_CRC:    s->crc_errors++;      break;
    case RP_CC13XX_ERR_VERSION:
        s->version_mismatch++;
        /* §6.2：任一态 ver≠1 → 拒收、装 ERROR{0x01} 于下一事务
         * （规则 2 槽位；LINKED 中收到坏 ver 帧同样拒——审计
         * round-WP-E-1 P2-2：对端不会因已 LINKED 而豁免版本合同）。
         * seq 从帧头 offset 4 提取（decode 失败路径 codec 不保证
         * 填充 out）。 */
        if (!s->cmd_err_pending) {
            s->cmd_err_pending = true;
            s->cmd_err_code     = 0x01;
            s->cmd_err_seq      = (uint16_t)(mosi[4] | ((uint16_t)mosi[5] << 8));
        }
        break;
    case RP_CC13XX_ERR_LEN:    s->len_errors++;      break;
    case RP_CC13XX_UNKNOWN_TYPE:
        /* CRC 合法但类型未知：容忍忽略（§5.4）；合法帧——但 slave 对
         * 未知类型命令不装载应答（prelink_reject 同款语义：整事务
         * 作废，回全 0），仅计数（§5.3，审计 round-WP-E-1 P2-3）。 */
        s->unknown_types++;
        break;
    default: break;
    }

    /* 3) 装载下一 pending（§2.3）。 */
    load_next(s, &in, cmd_ok);
}
