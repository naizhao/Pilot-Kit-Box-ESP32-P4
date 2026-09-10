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

/* 位置 fix 与 GGA 高度的新鲜度窗口。
 *
 * 「上一次收到过有效数据」不等于「现在有数据」：天线被遮、模块掉线、UART
 * 断了，这些情况下**一行 NMEA 都不会来**，于是任何只在收到新数据时才写的
 * have_* 都会永远停在 true，坐标冻在最后一次定位上。屏上/手机上看到的是一个
 * 静止不动但完全笃定的本机位置——那比显示"无 GPS"危险得多，后者会让人去看
 * 别的信息源，前者不会。
 *
 * 因此每个字段各带一个 *_us 时间戳，只在**携带它的那句报文**真的解析成功时
 * 才刷新；pk_gps_get() 在交出快照前按这个窗口逐字段过期，过期的 have_* 置
 * false 并把值清零（留着旧经纬度等于把冻结坐标继续递给消费者）。
 *
 * 5 s：模块 1 Hz 出 RMC/GGA，允许连丢 4 句。与 time_locked 的 NMEA 新鲜度项
 * 同一口径（见下方 time_locked 注释）。 */
#define PK_GPS_FIX_MAX_AGE_US   5000000LL

typedef struct {
    bool    have_fix;          /* RMC status == 'A' **且**经纬度解析合法 */
    double  lat, lon;          /* decimal degrees, +N/+E */
    bool    have_altitude;     /* GGA 正高(MSL)有效——与 fix 各自独立过期 */
    int     altitude_ft;       /* MSL, from GGA —— **不是**气压高度，见下 */

    /* 地速与航迹各自独立有效：RMC 的 speed/track 字段可以单独为空（模块半死
     * 时真实存在），也可以单独是垃圾。atof 把空字段和 "12.3abc" 都变成一个
     * 看起来合法的 0，而 0 kt / 航迹正北是两个**合法读数**——读的人分不出
     * "静止朝北"和"没有数据"。 */
    bool    have_ground_speed;
    int     ground_speed_kt;
    bool    have_track;
    int     track_deg;         /* 0..359 true */

    int     sats;              /* GGA: 参与定位解算的卫星数 (in use) */
    int64_t updated_us;        /* esp_timer_get_time() of last valid fix */
    int64_t altitude_us;       /* 最近一次**有效 GGA 高度**的时刻；0 = 从未 */

    /* --- PPS / 时间锁定（GPIO50 上升沿；ISR 只计数+打戳，1 Hz 快照提交） ---
     * 两条状态语义（刻意分开）：
     *   位置有效 = have_fix —— RMC status 'A'，纯 UART 数据面。
     *   时间锁定 = fix 有效 + **有效 RMC <5 s** + PPS <2 s —— 除了数据有效，
     *              还要求秒脉冲真的在跳、且最近一条**有效** RMC 也不算旧
     *              （updated_us；last_nmea_us 连坏 checksum 行都算「在讲话」，
     *              不作数）。由 gps_task 的 1 Hz 快照判定并写入 time_locked
     *              （非实时；ISR 内不做任何判定）。
     * last_pps_us == 0 表示开机至今没见过 PPS 沿（没接线/模块不出 PPS）。
     * (pps_count, last_pps_us) 由 ISR 在 spinlock 临界区内成对写、1 Hz 快照
     * 成对读：对 ISR 原子，不会出现半新半旧的组合。 */
    uint32_t pps_count;        /* PPS 上升沿累计计数 */
    int64_t  last_pps_us;      /* 最近一次 PPS 上升沿的 esp_timer 时间戳 */
    bool     time_locked;      /* 上述「时间锁定」判定结果，1 Hz 刷新 */

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
     * 方向——2026-07-29 实测没插 GPS 板卡，屏上却显示"no sats - check
     * antenna"，就是这么来的。 */
    int64_t      last_nmea_us;
} pk_gps_state_t;

/* Start UART1 + parser task. Call once at boot, after aircraft_state_init(). */
void pk_gps_start(void);

/* 复位解析状态（互斥量、快照、GSV 累积器、PPS 计数）。pk_gps_start() 内部
 * 先调它；host 测试直接调它来重置世界，不必去碰 UART。 */
void pk_gps_state_init(void);

/*
 * 喂一整行已经组装好的 NMEA 句子（不含 \r\n）。UART 任务拼完一行之后调用的
 * 就是这个函数——测试因此跑的是**生产解析链路本身**，不是它的复制品。
 *
 * checksum 不过、非 '$' 开头、超长的行整句丢弃（gps_nmea_feed_line 的闸），
 * 但仍然刷新 last_nmea_us：那一位记的是"模块在不在讲话"，坏行同样算讲话。
 * 非重入（与 gps_nmea 同一约定）：固件侧只有 gps 任务这一个消费者。
 */
void pk_gps_feed_line(const char *line);

/*
 * Snapshot current GPS state into *out. Returns out->have_fix.
 *
 * 交出去之前按 PK_GPS_FIX_MAX_AGE_US 做逐字段过期（见该宏注释）：调用方拿到
 * 的 have_fix / lat / lon / 地速 / 航迹 / 高度要么是新鲜的，要么已经被清成
 * "没有"。消费方不需要（也不应该）各自再去比 updated_us。
 *
 * altitude_ft 是 GGA 的 **GNSS 正高 (MSL)**，不是气压高度：它与 1013.25 基准
 * 的气压高度差着当地气压偏差，非标准日下地面就能差上千英尺，绝不能拿去和
 * ADS-B 目标的 Mode-C/DF17 高度相减，也不能编进 GDL90 的高度字段。
 * 本机三种高度的分离见 own_ship.h 的 pk_own_alt_t。
 */
bool pk_gps_get(pk_gps_state_t *out);
