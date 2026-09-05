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
