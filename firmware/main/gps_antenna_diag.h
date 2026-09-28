/* gps_antenna_diag.h — ATGM336H 天线电流状态的纯解释层。
 *
 * 模块的 ANTENNA OK/OPEN/SHORT 是 VCC_RF 负载电流判据，不是定位结果，
 * 也不是“外置天线存在”布尔值。6N 手册 §2.8 明确说明无源天线恒报 OPEN，
 * 所以 UI/日志必须保留 raw，并另给不会误报的 result。
 */
#pragma once

#include <stdbool.h>

#include "gps.h"

typedef enum {
    PK_GPS_ANT_RESULT_UNKNOWN = 0,
    PK_GPS_ANT_RESULT_ACTIVE_LOAD_OK,
    PK_GPS_ANT_RESULT_OPEN_OR_PASSIVE,
    PK_GPS_ANT_RESULT_RF_RECEIVING,
    PK_GPS_ANT_RESULT_SHORT,
} pk_gps_ant_result_t;

typedef enum {
    PK_GPS_HEADER_SATS,
    PK_GPS_HEADER_SHORT,
    PK_GPS_HEADER_ANT_UNKNOWN,
    PK_GPS_HEADER_NO_FIX,
} pk_gps_header_state_t;

static inline pk_gps_header_state_t pk_gps_header_state(bool fix,
                                                       pk_gps_ant_result_t result)
{
    if (result == PK_GPS_ANT_RESULT_SHORT) return PK_GPS_HEADER_SHORT;
    if (fix || result == PK_GPS_ANT_RESULT_RF_RECEIVING ||
        result == PK_GPS_ANT_RESULT_ACTIVE_LOAD_OK) return PK_GPS_HEADER_SATS;
    if (result == PK_GPS_ANT_RESULT_OPEN_OR_PASSIVE) return PK_GPS_HEADER_ANT_UNKNOWN;
    return PK_GPS_HEADER_NO_FIX;
}

static inline pk_gps_ant_result_t
pk_gps_ant_assess(pk_gps_ant_t raw, bool have_rf_evidence)
{
    /* SHORT 是模块已进入限流保护的原始状态，不能被旧的/偶发接收证据盖掉。 */
    if (raw == PK_GPS_ANT_SHORT) return PK_GPS_ANT_RESULT_SHORT;
    if (have_rf_evidence) return PK_GPS_ANT_RESULT_RF_RECEIVING;
    if (raw == PK_GPS_ANT_OK) return PK_GPS_ANT_RESULT_ACTIVE_LOAD_OK;
    if (raw == PK_GPS_ANT_OPEN) return PK_GPS_ANT_RESULT_OPEN_OR_PASSIVE;
    return PK_GPS_ANT_RESULT_UNKNOWN;
}

static inline const char *pk_gps_ant_raw_name(pk_gps_ant_t raw)
{
    switch (raw) {
    case PK_GPS_ANT_OK:    return "OK";
    case PK_GPS_ANT_OPEN:  return "OPEN";
    case PK_GPS_ANT_SHORT: return "SHORT";
    default:               return "UNKNOWN";
    }
}

static inline const char *pk_gps_ant_result_name(pk_gps_ant_result_t result)
{
    switch (result) {
    case PK_GPS_ANT_RESULT_ACTIVE_LOAD_OK: return "ACTIVE-LOAD-OK";
    case PK_GPS_ANT_RESULT_OPEN_OR_PASSIVE:
        return "INDETERMINATE-OPEN-OR-PASSIVE";
    case PK_GPS_ANT_RESULT_RF_RECEIVING: return "RF-RECEIVING";
    case PK_GPS_ANT_RESULT_SHORT:        return "SHORT";
    default:                             return "UNKNOWN";
    }
}
