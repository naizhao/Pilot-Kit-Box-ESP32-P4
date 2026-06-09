/*
 * dsp_task.h — public interface for the ADS-B edge decoder task.
 *
 * The dsp_task itself is declared in pilot_kit.h (void dsp_task(void *arg)).
 * This header adds diagnostic read-only accessors for boot-lifetime
 * cumulative counters — consumed by the PFD diagnostics page (pfd_task).
 */
#pragma once

#include <stdint.h>

/*
 * Boot-lifetime cumulative snapshot:
 *   msgs_total    — CRC-ok Mode-S frames received since boot
 *   pos_decoded   — CPR position pairs successfully decoded since boot
 *   icao_unique   — unique ICAO24 addresses seen since boot
 *   iq_drop_total — IQ bytes dropped by the ringbuffer since boot
 *
 * All fields are uint32_t; 32-bit aligned reads are atomic on ESP32-P4
 * (RV32), so no locking is required for single-word loads.
 */
typedef struct {
    uint32_t msgs_total;
    uint32_t pos_decoded;
    uint32_t icao_unique;
    uint32_t iq_drop_total;   /* 累计 IQ 丢字节;uint32 满速丢包约 36min 回绕,正常使用单调 */
} pk_dsp_stats_t;

/* 各字段独立采样,字段间无一致性快照保证(诊断展示足够;勿用于跨字段比率断言)。 */

/*
 * Populate *out with the current boot-lifetime cumulative counters.
 * Safe to call from any task at any time. No side-effects; the counters
 * are never reset by this call.
 */
void pk_dsp_get_stats(pk_dsp_stats_t *out);
