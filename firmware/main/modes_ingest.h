/*
 * modes_ingest.h — raw Mode-S 帧的统一解码前门。
 *
 * 1090 数据入口从 RTL-SDR IQ 换成 RP2040 UART（PLAN.md §6.6）之后，
 * P4 收到的是"已成形"的 56/112-bit 帧；CRC 策略仍归 P4（RP 不裁决），
 * 所以这里继续用 mode_s_decode + check_crc 做校验与字段展开，再把
 * 结果交给注册的 sink（业务层 = 原 dsp_task 的 on_mode_s_msg 链）。
 * 本文件保持纯 C（无 ESP-IDF include），host 单测直接编它。
 */
#pragma once

#include <stdint.h>
#include "mode_s.h"

typedef struct {
    uint8_t  rssi;      /* 0.5 dB 单位；0xFF = 未提供 */
    uint32_t rp_ts_us;  /* RP2040 单调 µs（preamble 首沿）；模 2^32 单调、
                         * 约 71.6 min 回绕（PROTOCOL §2 勘误）：时间差必须
                         * 用无符号差值 (u32)(now − prev) 解释；0 = 未提供
                         * （仅首沿前） */
} modes_ingest_meta_t;

typedef void (*modes_ingest_sink_fn)(const struct mode_s_msg *mm,
                                     const modes_ingest_meta_t *meta,
                                     void *user);

void modes_ingest_init(modes_ingest_sink_fn sink, void *user);

/* frame：MSB-first 的 7 或 14 字节；msgbits：56 或 112（其余值被忽略）。 */
void modes_ingest_feed(const uint8_t *frame, int msgbits,
                       const modes_ingest_meta_t *meta);

void modes_ingest_get_stats(uint32_t *msgs_total, uint32_t *frames_bad_crc);

/* --- 诊断快照（沿用 RTL-SDR 时代的 dsp_task.h 接口名，字段见下）------ */
typedef struct {
    uint32_t msgs_total;       /* CRC-ok 帧累计（与 ingest 统计同源） */
    uint32_t pos_decoded;      /* CPR 成功解码累计（业务层维护） */
    uint32_t icao_unique;      /* 开机以来唯一 ICAO 数（业务层维护） */
    uint32_t frames_bad_crc;   /* 旧名 iq_drop_total；语义改为坏 CRC 帧数 */
} pk_dsp_stats_t;

void pk_dsp_get_stats(pk_dsp_stats_t *out);   /* 实现在 adsb_link_task.c */
