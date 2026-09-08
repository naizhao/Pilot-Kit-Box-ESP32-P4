/*
 * spi_master.h — RP2040 侧 CC1312R SPI master 事务状态机（WP-E T3）。
 *
 * 规范：firmware/PROTOCOL_RP2040_CC1312R_SPI.md v1.0（十轮审计后冻结）。
 * 本文件只实现**纯逻辑**（状态机 + 调度决策 + 帧消化）——host 可测
 * （SPI_MASTER_HOST_TEST 隔离，模式照 modes_edge / power_service）；SPI1
 * DMA / GPIO 采样 / RESET 脉冲的胶水在 spi_master.c 的目标区。
 *
 * ── 事务驱动模型 ────────────────────────────────────────────────────
 * 调用方（host 测试的 mock slave，或目标端的 DMA 循环）按事务推进：
 *
 *     irq   = sample_irq();
 *     n     = spi_master_next_txn(m, now_us, mosi, irq);   // 决策+编码
 *     miso  = transfer(mosi);                              // 512 B 定长
 *     spi_master_digest(m, now_us + TXN_US, miso);        // 消化+转移
 *
 * 事务时长 TXN_US 由调用方决定（时钟速率未冻结，§1——状态机一切判定
 * 用事务计数或 µs 时基，不假设线速）。
 *
 * ── 状态机（规范 §6）───────────────────────────────────────────────
 *   WAIT_HELLO  每 500 ms 发 HELLO（§6.2）；10 次无应答 → RESET 重来；
 *               收到 ver=1 合法 HELLO → LINKED（seq 基线重置 §6.5、
 *               re-HELLO 清半交付态、RF_CONFIG 待发置位 §6.4）。
 *   LINKED      §6.7 调度合同：RF_CONFIG 待发 > IRQ 高（→drain：
 *               连发 IRQ_ACK 至「无事件且 IRQ 低」§7.2）> 1 Hz PING
 *               （§6.6，不看 IRQ）。RECOVERY 触发：>3 s 无合法帧
 *               （drain 事件流为活性、不计时）或 irq_spurious
 *               （IRQ 高 >1 s 且期间事务均无事件，非 drain）。
 *   RECOVERY    请求 RESET 脉冲（胶水执行 ≥1 ms §1）→ ≥100 ms 启动
 *               等待（§6.1）→ 回 WAIT_HELLO。
 *
 * ── UAT 数据出口 ────────────────────────────────────────────────────
 * 分片重组完成（552 B 交织帧，事实卡 §7）→ uat 回调一次性交付
 * （rssi/ts_us 取自 RX_DESCRIPTOR，§4.3）。目标区胶水把它编码成
 * adsb_link UAT_UPLINK（UART v1.1 §6）送 p4_link；host 测试直接捕获。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rp_cc13xx_codec.h"

/* 规范数值（§ 引用即合同；改动须先改规范） */
#define SPIM_HELLO_RETRY_US   500000u  /* §6.2：WAIT_HELLO 重试周期      */
#define SPIM_HELLO_MAX_TRIES  10       /* §6.2：≈5 s 无应答 → RESET 重来 */
#define SPIM_BOOT_WAIT_US     100000u  /* §6.1：RESET 释放后启动下限      */
#define SPIM_LINKED_PING_US   1000000u /* §6.6：LINKED 1 Hz PING         */
#define SPIM_WATCHDOG_US      3000000u /* §6.6：3 s 无合法帧 → RECOVERY  */
#define SPIM_IRQ_SPURIOUS_US  1000000u /* §6.6：IRQ 假粘判定窗           */
#define SPIM_STALL_MAX        8        /* §7.2：drain 停滞判定           */

typedef enum {
    SPIM_WAIT_HELLO = 0,
    SPIM_LINKED,
    SPIM_RECOVERY,
} spim_state_t;

/* UAT 帧完成回调：552 B 交织帧 + 描述符元数据（§4.3）。 */
typedef void (*spim_uat_fn)(const uint8_t *frame, size_t len,
                            uint8_t rssi, uint32_t ts_us, void *user);

/* 统计（诊断口径，§5.3 计数由 codec 返回值累加；此处 master 会话级） */
typedef struct {
    uint32_t txns;              /* 发起的事务总数                        */
    uint32_t legal_frames;      /* 收到合法 MISO 帧（OK/UNKNOWN）        */
    uint32_t event_txns;        /* 事件事务（drain 活性证明/stall 清零）  */
    uint32_t no_frame_txns;     /* 全 0 无帧事务                          */
    uint32_t resyncs;           /* codec ERR_MAGIC 累计（§5.2）           */
    uint32_t crc_errors;        /* codec ERR_CRC 累计                     */
    uint32_t len_errors;        /* codec ERR_LEN 累计                     */
    uint32_t version_mismatch;  /* codec ERR_VERSION 累计                 */
    uint32_t seq_gaps;          /* 事件类 seq_gap 判定累计（§5.5）        */
    uint32_t recoveries;        /* 进入 RECOVERY 次数                     */
    uint32_t uat_complete;      /* 重组完成的 UAT 帧                      */
} spim_stats_t;

typedef struct {
    spim_state_t   state;

    /* 时基（µs，由 next_txn/digest 的 now_us 喂入；模 2^32 差值判定） */
    uint32_t       t_now;          /* 最近一次 next_txn 时刻               */
    uint32_t       t_last_hello;   /* 最近一次 HELLO 发出                  */
    uint32_t       t_last_ping;    /* 最近一次 PING 发出                   */
    uint32_t       t_last_legal;   /* 最近一次合法 MISO 帧（3 s 看护）     */
    uint32_t       t_last_event;   /* 最近一次事件事务（irq_spurious 判定）*/
    uint32_t       t_irq_high;     /* IRQ 高电平起始（0=当前不高）         */
    uint32_t       t_reset_req;    /* RESET 脉冲请求时刻（0=无请求）       */

    uint8_t        hello_tries;    /* WAIT_HELLO 连续无应答计数            */
    uint8_t        stall_cnt;      /* drain 连续无事件事务（§7.2）         */
    uint16_t       cmd_seq;        /* master 命令 seq（§3.4 每帧 +1）      */
    uint16_t       ev_seq;         /* 事件类 seq 基线（§6.5 HELLO 重置）   */
    bool           ev_seq_valid;

    bool           rf_pending;     /* LINKED 后 RF_CONFIG 查询待发（§6.4） */
    rp_cc13xx_rf_config_t peer_rf; /* 对端 RF 配置（RF_CONFIG_STATUS 解得） */

    /* UAT 通路 */
    rp_cc13xx_reasm_t reasm;
    rp_cc13xx_rx_desc_t desc;      /* 当前重组报文的元数据（rssi/ts_us）  */
    bool           uat_done;       /* 本报文完成回调已触发（start 时清）   */

    /* 对端信息（诊断） */
    rp_cc13xx_hello_t peer_hello;

    /* RESET 脉冲请求（胶水层消费；digest 置位，spi_master_reset_done 清除） */
    bool           reset_req;

    spim_stats_t   stats;

    spim_uat_fn    uat_cb;
    void          *uat_user;
} spi_master_t;

/* 初始化。rf 参数仅取指针拷贝占位（查询用版本号 0 的 RF_CONFIG，§4.6）；
 * uat 回调可为 NULL（测试可不挂）。 */
void spi_master_init(spi_master_t *m, spim_uat_fn uat_cb, void *user);

/* 决策 + 编码下一事务的 MOSI 帧（写入 moso[512]，帧尾填 0）。
 * 返回帧本体长度（>0 = 已发起事务，缓冲补 0 至定长 512，§2.1；
 * 0 = 本拍不发——WAIT_HELLO 重试窗内或 RECOVERY 等待 RESET 期间）。
 * irq_high = 本事务发起前采样的 SUBG_IRQ 电平（§6.7 在 CSN↑ 后采样，
 * 调用方在上一事务结束时采好传入即可）。无事务可发时（理论上仅
 * WAIT_HELLO 等待重试窗内）返回 0 且不写 mosi。 */
size_t spi_master_next_txn(spi_master_t *m, uint32_t now_us,
                           uint8_t mosi[512], bool irq_high);

/* 消化本事务的 MISO（512 B 定长缓冲）。内部完成：帧解码（codec）、
 * 事件类处理（descriptor/chunk/QUEUE_FULL/ERROR）、seq gap 判定、
 * 状态转移（含 RECOVERY 触发）、UAT 回调。txn_us = 本事务时长
 * （t_now 推进量，看护计时用）。 */
void spi_master_digest(spi_master_t *m, uint32_t txn_us,
                       const uint8_t miso[512]);

/* RESET 脉冲请求查询（胶水层：执行 ≥1 ms 低脉冲后调用下面这个清除） */
static inline bool spi_master_reset_requested(const spi_master_t *m)
{
    return m->reset_req;
}
void spi_master_reset_done(spi_master_t *m, uint32_t now_us);
