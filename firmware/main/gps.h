#pragma once
#include <stdbool.h>
#include <stdint.h>

/* 天线自检状态，来自模块的 $GPTXT,...,ANTENNA OK/OPEN/SHORT。 */
typedef enum {
    PK_GPS_ANT_UNKNOWN = 0,
    PK_GPS_ANT_OK,
    PK_GPS_ANT_OPEN,           /* 天线开路：没接 / 馈电断 / 有源天线没供上电 */
    PK_GPS_ANT_SHORT,          /* 天线短路：保护启动 */
} pk_gps_ant_t;

#define PK_GPS_SNR_MAX  32     /* AT6558 32 通道，SNR 列表上限 */

typedef struct {
    bool    have_fix;          /* RMC status == 'A' */
    double  lat, lon;          /* decimal degrees, +N/+E */
    bool    have_altitude;
    int     altitude_ft;       /* MSL, from GGA */
    int     ground_speed_kt;
    int     track_deg;         /* 0..359 true */
    int     sats;              /* GGA: 参与定位解算的卫星数 (in use) */
    int64_t updated_us;        /* esp_timer_get_time() of last valid fix */

    /* --- 诊断字段（1 Hz 快照，来自 GSV/GGA/TXT） --- */
    int          sats_in_view; /* GSV: 可见卫星总数 (GPS+BDS…合计) */
    int          snr_max;      /* 最强卫星 C/N0 dB；无则 0 */
    uint8_t      snr[PK_GPS_SNR_MAX]; /* 各可见星 SNR(dB)，诊断页柱状图用 */
    int          snr_count;    /* snr[] 有效个数 */
    float        hdop;         /* GGA 水平精度因子；无星时模块报 25.5 */
    pk_gps_ant_t ant_status;   /* 天线自检 */
} pk_gps_state_t;

/* Start UART1 + parser task. Call once at boot, after aircraft_state_init(). */
void pk_gps_start(void);

/* Snapshot current GPS state into *out. Returns out->have_fix. */
bool pk_gps_get(pk_gps_state_t *out);
