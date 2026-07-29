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

/* GNSS 星座 ID（snr_con[] 取值）。顺序 = 诊断页柱状图分组顺序。
 * 由 NMEA talker 前缀映射：GP→GPS、BD/GB→BDS、GL→GLO、GA→GAL、GQ→QZSS。
 * 当前模块(5N-31)只有 GPS+BDS；其余为换更高型号(如 5N-71)预留，零代码改动即用。 */
typedef enum {
    PK_GNSS_GPS = 0,   /* G */
    PK_GNSS_BDS,       /* B  北斗 */
    PK_GNSS_GLO,       /* R  GLONASS */
    PK_GNSS_GAL,       /* E  Galileo */
    PK_GNSS_QZSS,      /* Q */
    PK_GNSS_OTHER,     /* ? */
    PK_GNSS_COUNT
} pk_gnss_t;

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
    int          sats_in_view;     /* GSV: 可见卫星总数 (GPS+北斗…合计) */
    int          sats_in_view_gps; /* 其中 GPS 可见星数 */
    int          sats_in_view_bds; /* 其中 北斗 可见星数 */
    int          snr_max;          /* 最强卫星 C/N0 dB；无则 0 */
    uint8_t      snr[PK_GPS_SNR_MAX];     /* 各可见星 SNR(dB)，诊断页柱状图用 */
    uint8_t      snr_con[PK_GPS_SNR_MAX]; /* 与 snr[] 平行：pk_gnss_t 星座 ID */
    int          snr_count;        /* snr[]/snr_con[] 有效个数 */
    float        hdop;         /* GGA 水平精度因子；无星时模块报 25.5 */
    pk_gps_ant_t ant_status;   /* 天线自检 */

    /* 最后一次收到**任何** NMEA 行的时间；0 = 开机至今一行都没收到。
     *
     * 这是「模块在不在」的唯一依据，跟「有没有星」是两件事：模块没插时
     * 一行也不会来；插了没天线时 NMEA 照常来（还会带 $GPTXT ANTENNA OPEN），
     * 只是没有星。诊断页把这两种混成一句"检查天线"会把人指向完全错误的
     * 方向——2026-07-29 罩哥没插 GPS 板卡，屏上却显示"no sats - check
     * antenna"，就是这么来的。 */
    int64_t      last_nmea_us;
} pk_gps_state_t;

/* Start UART1 + parser task. Call once at boot, after aircraft_state_init(). */
void pk_gps_start(void);

/* Snapshot current GPS state into *out. Returns out->have_fix. */
bool pk_gps_get(pk_gps_state_t *out);
