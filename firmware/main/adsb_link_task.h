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
