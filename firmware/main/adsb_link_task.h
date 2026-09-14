/*
 * adsb_link_task.h — P4 侧 RP2040 ADS-B UART 链路任务。
 *
 * 硬件：UART2，RX=GPIO46（RP2040 TXD，J3-35）、TX=GPIO32（RP2040 RXD，
 * J3-31），921600 8N1（台架候选值，PLAN.md §5.1）。协议 v1 见
 * firmware/PROTOCOL_P4_RP2040_UART.md；编解码用共享组件 adsb_link_codec。
 * 本任务同时承载原 dsp_task 的报文后业务链（CPR/航迹/记录/看板）。
 */
#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "mode_s.h"
#include "modes_ingest.h"

typedef enum {
    PK_ADSB_LINK_NO_LINK = 0,    /* 上电后未收到任何合法帧 */
    PK_ADSB_LINK_PROTO_MISMATCH, /* 收到过 major≠1 的帧：拒绝工作态（规范§3.2） */
    PK_ADSB_LINK_STALLED,        /* 曾 LINKED，>5 s 无合法帧 */
    PK_ADSB_LINK_LINKED,         /* 收到合法帧且 5 s 内有活性 */
} pk_adsb_link_state_t;

typedef struct {
    uint32_t rx_frames;      /* codec 成功递交帧数（含未知类型） */
    uint32_t rx_crc_errors;
    uint32_t rx_seq_gaps;
    uint32_t rx_resyncs;
    uint32_t modes_fed;      /* 实际送入 modes_ingest 的 MODES_RAW 帧数 */
} pk_adsb_link_stats_t;

/* 诊断页只读快照；stats 可传 NULL。 */
pk_adsb_link_state_t pk_adsb_link_state_get(pk_adsb_link_stats_t *stats);

/* RP2040 最近一次 HEALTH_STATS 的原始字段（协议 §4；字段数与下标见
 * adsb_link.h 的 ADSB_LINK_HEALTH_FIELDS，out 必须能放下这么多个 u32）：
 * 0=pre 1=f56 2=f112 3=degraded 4=noise 5=ovr 6=tx 7=txdrop 8=rx 9=gap
 * 10=vbus_mid_mV 11=**实际生效**门限(permille) 12=**实际生效** 1090 天线
 * (0=板载 IFA/1=外接 J6)。返回**有效字段数**，0 = 还没收到过任何一帧。
 *
 * 下标 11/12 是扫描/CDC 切换时唯一能反映板上真实配置的来源：P4 本地存的是
 * 用户设定值，扫描期间 RP 的门限由扫描状态机逐点改，两者会不同。对面是
 * v1.2 及更早的 RP2040 时只发 11 个字段，返回 11、下标 11/12 填 0，调用方
 * 据此回退到 P4 本地值（见 rf1090_log.c）。
 *
 * 为什么要暴露：这些数产生在 RP2040 上，而 P4 是唯一能把它们落到 SD 的地方。
 * 不落盘的话，它们的存活就取决于当时有没有插着 USB 线——而射频调试全程在
 * 拔线焊板，这个前提根本不成立（见 rf1090_log.h）。 */
int pk_adsb_rp_health(uint32_t *out);

/* ── 门限扫描结果（RP2040 经 SWEEP_ROW 逐点上报）─────────────────────
 * UI 边收边画，不等整轮结束——一轮 90s。 */
#define PK_SWEEP_MAX_ROWS 12

typedef struct {
    uint16_t permille;   /* 已解码成 permille，UI 不再碰协议标度 */
    uint16_t level_mv;
    uint32_t frames, pre, noise, ovr;
} pk_sweep_row_t;

/* 拷出已收到的行；返回行数。total 出参是本轮总点数（0 = 还没收到任何行）。
 * 拷贝而不是回指内部数组：这张表由链路任务写、UI 任务读，回指等于把撕裂
 * 风险交给调用方。 */
int  pk_sweep_snapshot(pk_sweep_row_t *out, int cap, int *total);

/* 本轮是否还在进行（收到的行数 < total）。UI 用它决定显示"扫描中"还是
 * 结果，也用它在扫描期间禁掉天线切换——切了整张表就跨了两种条件。 */
bool pk_sweep_running(void);

/* 启动/中止扫描（经 CONFIG_REQ 的 SWEEP key 下发）。启动会清空上一轮结果。 */
void pk_sweep_start(void);
void pk_sweep_stop(void);

/* 存一行结果。参数是**已解码**的值——生产路径由 SWEEP_ROW 的处理分支解出
 * 后调用。拆成这个形状是为了让表本身的行为（下标推进、越界拒收、清空语义）
 * 能被 host 直接驱动；payload 的解码另由 codec 单测覆盖，两边不重复。 */
void pk_sweep_ingest_row(uint8_t idx, uint8_t total, uint16_t permille,
                         uint16_t level_mv, uint32_t frames, uint32_t pre,
                         uint32_t noise, uint32_t ovr);

/* 创建链路任务。前置：record sinks 与 pk_rec_ingest_init() 已就绪
 * （调用点保持在 app_main 末尾，即旧 sdr/dsp 的位置）。 */
void pk_adsb_link_start(void);

/*
 * 把当前天线配置下发给 RP2040（CONFIG_REQ，协议 v1.2 §7）。
 *
 * 两个调用时机：用户在设置页改了值；以及**每次收到 RP2040 的 HELLO**——
 * RP2040 不持久化配置，上电只回到 rf_safety 的安全默认，所以它每重启一次
 * 都要重新推一次。挂在 HELLO 上而不是"只在 linked 边沿推一次"，是因为
 * RP 侧未 linked 时就是 1 Hz 发 HELLO，边沿判定漏一次就要等到下次开机。
 *
 * 线程安全：内部自己加锁，可从 UI 任务直接调。
 */
void pk_adsb_link_push_config(void);

#ifdef PK_HOST_TEST
/* Host dispatch proof: the task loop cannot run under FreeRTOS stubs, but the
 * production modes_ingest sink must still be exercised without copying it. */
void on_ingest_msg(const struct mode_s_msg *mm,
                   const modes_ingest_meta_t *meta,
                   void *user);
#endif
