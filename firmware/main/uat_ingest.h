/*
 * uat_ingest.h — UAT 上行帧的统一解码前门（WP-E Task 2）。
 *
 * 镜像 modes_ingest.h 的前门模式：RP2040 UART 链路（协议 v1.1 §6
 * UAT_UPLINK）送来"已成形"的 552 B 交织帧（CC1312R 解调产物，事实卡 §7
 * 边界裁决），解交织 + RS(92,72)×6 纠错 + 消息层解码归本单元内部的
 * uat_decode（Task 1 纯单元），结果交注册的 sink。UAT payload 无 CRC
 * ——完整性完全由 RS 承担（事实卡 §6 裁决），"坏帧"即"RS 不可纠"。
 *
 * 本文件保持纯 C（无 ESP-IDF include），host 单测直接编它。
 *
 * 融合口径（Task 2 裁决）：UAT 上行消息层**没有**目标身份字段——
 * lat/lon 是地面站站点坐标、信息帧载荷语义属后续任务（事实卡 §4/§9），
 * 所以本前门的 sink 目前不喂 aircraft_state（1090 路径的融合库，
 * modes_ingest → adsb_link_task → aircraft_state_ingest）。待 T5 解出
 * TIS-B/ADS-R 目标报告后，目标必须经**同一** aircraft_state 接口入库
 * （icao24 键、同一张表），不得另起并行状态库。
 */
#pragma once

#include <stdint.h>
#include "uat_decode.h"

typedef struct {
    uint8_t  rssi;      /* 0.5 dB 单位；0xFF = 未提供（协议 §6.1，与 SPI
                          * RX_DESCRIPTOR §4.3 / MODES_RAW 同族单位）*/
    uint32_t rp_ts_us;  /* **CC1312R 描述符时钟**（SPI §4.3 ts_us，经
                          * RP2040 原样转发——UART v1.1 §6.1；字名沿用
                          * rp_ts_us 是 v1.1 载荷族惯例，时钟域不是
                          * RP2040 的 preamble 沿，与 MODES_RAW 的
                          * rp_ts_us **不同源**）。模 2^32 单调、差值按
                          * (u32)(now − prev) 无符号解释，0 是合法回绕值
                          * （无哨兵）。跨链（978 vs 1090）时效比较不得
                          * 混用两域。*/
} uat_ingest_meta_t;

/* rs_corrected：本帧 RS 纠正的字节总数（≤60，事实卡 §2/§7——跨链路保留
 * 的链路质量观测量）。up 内的 info[].data/fisb.payload 指向本模块内部
 * 静态缓冲，**下一次 feed 前有效**（契约同 uat_decode 的 data_out）。 */
typedef void (*uat_ingest_sink_fn)(const uat_uplink_t *up,
                                   const uat_ingest_meta_t *meta,
                                   uint8_t rs_corrected,
                                   void *user);

void uat_ingest_init(uat_ingest_sink_fn sink, void *user);

/* frame：552 B 交织帧原样（协议 §6.1 payload 偏移 5 起的字节）。
 * meta：可为 NULL（sink 收到清零 meta，modes_ingest 同款）。 */
void uat_ingest_feed(const uint8_t frame[UAT_UPLINK_FRAME_BYTES],
                     const uat_ingest_meta_t *meta);

/* 计数口径（镜像 modes_ingest：成功/失败分桶、relaxed 原子、单调累计）：
 *   frames_total — RS 成功解码的帧（sink 为 NULL 时照常计数，与 modes
 *                  msgs_total 同语义；≈ 递交 sink 的帧数）；
 *   bad_rs      — RS 不可纠拒绝（UAT 无 CRC，这是唯一的"坏帧"形态）；
 *   no_sync     — 全零"无帧"哨兵拒绝：SPI §2.3 规则 5 的合法空事务经
 *                 UART 透传后的形态（同步字不上 UART——CC1312R 已在 RF
 *                 层完成同步，事实卡 §7；P4 侧唯一的"非帧"输入就是全零）。
 */
void uat_ingest_get_stats(uint32_t *frames_total, uint32_t *bad_rs,
                          uint32_t *no_sync);
