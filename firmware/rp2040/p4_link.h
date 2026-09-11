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
void p4_link_tick_health(const uint32_t counters10[10]);
bool p4_link_linked(void);
void p4_link_get_stats(uint32_t *tx, uint32_t *rx, uint32_t *gaps);

/* P4 经 CONFIG_REQ 下发的当前天线选择（0 = 上电默认，见 rf_safety.h）。
 * 只用于诊断打印，真值表与实际电平归 rf_safety.c。 */
uint8_t p4_link_ant_1090(void);
uint8_t p4_link_ant_gnss(void);
