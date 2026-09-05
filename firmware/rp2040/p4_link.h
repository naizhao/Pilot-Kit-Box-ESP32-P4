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
void p4_link_send_error(uint8_t code);
void p4_link_poll_rx(void);
void p4_link_tick_health(const uint32_t counters10[10]);
bool p4_link_linked(void);
void p4_link_get_stats(uint32_t *tx, uint32_t *rx, uint32_t *gaps);
