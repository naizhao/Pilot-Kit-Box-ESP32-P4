/*
 * spi_slave.h — CC1312R 侧 SPI slave 事务状态机（纯逻辑，host 可测）。
 *
 * 规范：firmware/PROTOCOL_RP2040_CC1312R_SPI.md v1.0（十轮审计冻结）。
 * 本模块是 slave 侧的协议实现：§2.3 五步装载链、§1 挂起源生命周期
 * （交付沿清除）、§6.2 WAIT_HELLO 应答策略、§4 消息族、RF 配置存储
 * （§4.6）。SSR/DMA/中断胶水在 main.c（目标端）；host 测试与 RP2040
 * 侧 spi_master 直接对跑（master↔slave 全链互通 = 协议闭环的可执行
 * 证明——T4 的核心交付）。
 *
 * ── 事务驱动模型（与 master 对称）──────────────────────────────────
 * CC13 侧每事务：
 *
 *     [CSN↓ 前]  cc13_slave_pending(s, miso);   // 预载 MISO（§2.2）
 *     [事务中]    SSI DMA 全双工 512 B；
 *     [CSN↑ 后]  cc13_slave_txn_done(s, mosi);  // 交付沿清源 + 处理 + 装载
 *
 * IRQ 电平：cc13_slave_irq()（§1 四源之或；装载不清除，交付完成才落低）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rp_cc13xx_codec.h"

#define CC13S_MAXQ        4      /* 事件队列容量（UAT 报文数）          */
#define CC13S_UAT_LEN     552    /* 事实卡 §7：交织帧定长               */
#define CC13S_TXN_LEN     512    /* §2.1 定长事务                       */

typedef enum {
    CC13S_WAIT_HELLO = 0,
    CC13S_LINKED,
} cc13s_state_t;

typedef struct {
    cc13s_state_t state;

    /* 事件队列（IRQ 背书源 1）*/
    uint8_t  q[CC13S_MAXQ][CC13S_UAT_LEN];
    uint8_t  q_rssi[CC13S_MAXQ];
    uint32_t q_ts[CC13S_MAXQ];
    int      qn;
    uint32_t dropped;             /* 队满丢弃累计（§4.5 events_dropped） */

    /* 挂起标志（IRQ 背书源 3/4）*/
    bool     qfull;
    bool     aerr;                /* 异步 ERROR 待交付（§2.3 规则 4c）   */
    uint8_t  async_code;          /* 异步错误码（0x04/0x05，§4.9）       */

    /* 交付中的报文（IRQ 背书源 2：分片未取完）*/
    int      cur;                 /* 队列下标；-1 = 无                   */
    uint16_t cur_desc_id, cur_sent, cur_total;
    uint16_t last_desc_id;

    /* slave 事件计数器（§3.5：事件类帧唯一 seq 域）*/
    uint16_t ev_seq;

    /* pending 槽（事务 N 装载、事务 N+1 交付，§2.2；装载沿只装不清）*/
    uint8_t  pend[CC13S_TXN_LEN];
    int      pend_kind;           /* RP_CC13XX_MSG_*；0 = 全 0 无帧      */
    uint16_t pend_dlen;           /* pend 为 CHUNK 时的 data_len         */
    bool     pend_final;          /* pend 为末片                         */
    bool     pend_async;          /* pend 为异步 ERROR（交付沿才清 aerr）*/

    /* 顺延的命令性 ERROR（§2.3 规则 2：无后备队列，优先于直接应答
     * 之外的一切装载）。来源：ver≠1 拒收 ERROR{0x01}（§6.2）、
     * config_version 非单调写拒绝 ERROR{0x03}（§4.6）。 */
    bool     cmd_err_pending;
    uint8_t  cmd_err_code;
    uint16_t cmd_err_seq;         /* 回显被拒命令的 seq（§3.5 应答类）  */

    /* RF 配置存储（§4.6：config_version + digest 对账槽位）*/
    rp_cc13xx_rf_config_t rf;

    /* 会话统计（诊断；master 侧可查）*/
    uint32_t txns;
    uint32_t legal_frames;
    uint32_t no_frame_txns;
    uint32_t resyncs, crc_errors, len_errors, version_mismatch;
    uint32_t prelink_reject;

    /* 本端版本（HELLO §4.1 携带）*/
    uint16_t fw_major, fw_minor;
    uint32_t reset_reason;
} cc13s_slave_t;

void cc13s_init(cc13s_slave_t *s, uint16_t fw_major, uint16_t fw_minor);

/* CSN↓ 前：把 pending 槽内容拷入 miso（无帧 = 全 0，§2.3 规则 5）。 */
void cc13s_pending(const cc13s_slave_t *s, uint8_t miso[CC13S_TXN_LEN]);

/* CSN↑ 后：交付沿清源（§1）→ 处理 MOSI 命令 → 按 §2.3 五步链装载
 * 下一 pending。一个调用完成一个事务的 slave 侧闭环。 */
void cc13s_txn_done(cc13s_slave_t *s, const uint8_t mosi[CC13S_TXN_LEN]);

/* §1：IRQ = 事件队列非空 ∨ 分片未取完 ∨ qfull ∨ aerr（WAIT_HELLO
 * 不驱动事件流——§6.2）。 */
bool cc13s_irq(const cc13s_slave_t *s);

/* RF 收包注入（事件入队；队满置 qfull 并计 dropped，§4.5）。 */
void cc13s_push_uat(cc13s_slave_t *s, const uint8_t frame[CC13S_UAT_LEN],
                    uint8_t rssi, uint32_t ts_us);

/* 异步错误注入（固件内部错误 / RX 溢出 → ERROR 0x04/0x05，§4.9）。 */
void cc13s_raise_async(cc13s_slave_t *s, uint8_t code);
