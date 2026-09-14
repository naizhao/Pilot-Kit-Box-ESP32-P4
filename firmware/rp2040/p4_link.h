/*
 * p4_link.h — RP2040 侧 P4 链路（UART0 @921600，协议 v1）。
 *
 * 协议：firmware/PROTOCOL_P4_RP2040_UART.md；编解码复用共享 codec
 * （components/adsb_link_codec，同一份 .c 编进本工程）。
 * 时序契约：p4_link_send_modes 仅允许 core0 sender 循环调用（内部阻塞写）；
 * 解码核绝不直发，帧环背压丢帧由上层（Task 14）计数。
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "modes_edge.h"

void p4_link_init(void);
bool p4_link_send_modes(const modes_edge_frame_t *f, uint8_t rssi);
/* UAT_UPLINK 转发（协议 v1.1 §6，type 0x11）：552 B 交织帧原样 +
 * rssi（0.5 dB/LSB，0xFF=无值）+ ts_us（CC1312R 描述符时钟，单位
 * 不改写）。帧内容合同见 uat_decode.h / UAT 事实卡 §7。 */
bool p4_link_send_uat(const uint8_t frame552[552], uint8_t rssi,
                      uint32_t ts_us);
void p4_link_send_error(uint8_t code);
void p4_link_poll_rx(void);
/* HEALTH_STATS：ADSB_LINK_HEALTH_FIELDS 个 u32（协议 v1.3；v1.0 是 10 个，
 * 第 11 个是 VBUS 分压中点 mV，第 12/13 个是**实际生效**的门限/天线）。
 * 接收方只看前缀也仍然正确——这是 minor 前向兼容的用法。 */
void p4_link_tick_health(const uint32_t counters[ADSB_LINK_HEALTH_FIELDS]);
bool p4_link_linked(void);
void p4_link_get_stats(uint32_t *tx, uint32_t *rx, uint32_t *gaps);

/* 门限扫描逐点结果上报（SWEEP_ROW）。逐点发而不是整轮攒完再发：一轮
 * 90s，P4 得能边收边画进度，也得有机会中途叫停。 */
void p4_link_send_sweep_row(uint8_t idx, uint8_t total, uint8_t permille_val,
                            uint16_t level_mv, uint32_t frames, uint32_t pre,
                            uint32_t noise, uint32_t ovr);

/* ── 由 adsb1090.c 实现的扫描钩子 ──────────────────────────────────
 * CONFIG_REQ 的 SWEEP key 最终落到 adsb1090.c 那个状态机上。在这里只声明、
 * 不让 p4_link 自己持有状态机，是因为扫描要读 health 统计、要驱动
 * threshold_ctl，那些都长在 adsb1090.c 上；反过来把 CONFIG_REQ 解析搬过去
 * 又会让协议逻辑散成两处。CDC 的 'W' 和这里走的是同一个状态机，不各抄
 * 一份真值。 */
void p4_link_sweep_start_hook(void);
void p4_link_sweep_stop_hook(void);

/* P4 经 CONFIG_REQ 下发的当前天线选择（0 = 上电默认，见 rf_safety.h）。
 * 只用于诊断打印，真值表与实际电平归 rf_safety.c。 */
uint8_t p4_link_ant_1090(void);
uint8_t p4_link_ant_gnss(void);
