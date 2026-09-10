/*
 * aircraft_state.h — per-aircraft state aggregation across Mode-S frames.
 *
 * The dump1090-derived decoder in mode-s.c hands us one Mode-S message
 * at a time. To produce a GDL90 Traffic Report (msg ID 20) the BLE
 * transport must combine fields that arrive in *different* frames —
 * callsign comes from DF17 metype 1..4, altitude + raw CPR from
 * metype 9..18, velocity + heading + vrate from metype 19. This
 * module owns that fusion layer.
 *
 * Internally it holds a small open-addressing table keyed by 24-bit
 * ICAO address (similar shape to cpr_decode.c's table, kept separate
 * to avoid coupling the BLE transport to CPR math). Each slot tracks
 * the freshest known callsign / altitude / position / velocity plus a
 * monotonic last-seen timestamp; the 1 Hz GDL90 emitter walks the
 * table once per second and notifies BLE subscribers about every
 * aircraft seen in the trailing AIRCRAFT_STALE_AGE_US window.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mode_s_msg;  /* forward decl from mode-s.h */

#define AIRCRAFT_TABLE_CAPACITY   64
#define AIRCRAFT_STALE_AGE_US     (60ULL * 1000000ULL)  /* 60 s */
#define AIRCRAFT_CALLSIGN_LEN     9                     /* 8 chars + NUL */

/* Wake-vortex / aircraft-category enum compatible with ADS-B
 * Category Sets A/B/C (DO-260B section 2.2.3.2.5.2). The mapping
 * from raw (metype, mesub) is performed by aircraft_state_ingest.
 * Single-letter rendering for the list view comes from
 * pk_wake_letter(); full names from pk_wake_name(). */
typedef enum {
    PK_WAKE_NONE = 0,        /* unknown / not yet reported */
    PK_WAKE_LIGHT,           /* A1 */
    PK_WAKE_SMALL,           /* A2 */
    PK_WAKE_LARGE,           /* A3 */
    PK_WAKE_HIGH_VORTEX,     /* A4 — B757 */
    PK_WAKE_HEAVY,           /* A5 — B777/A330/... */
    PK_WAKE_HIGH_PERF,       /* A6 — high-performance / fighter */
    PK_WAKE_ROTOR,           /* A7 — helicopter */
    PK_WAKE_GLIDER,          /* B1 */
    PK_WAKE_LTA,             /* B2 — balloon / blimp */
    PK_WAKE_PARACHUTE,       /* B3 */
    PK_WAKE_ULTRALIGHT,      /* B4 */
    PK_WAKE_UAV,             /* B6 — drone */
    PK_WAKE_SPACE,           /* B7 — spacecraft */
    PK_WAKE_SURFACE_EMERG,   /* C1 — emergency vehicle */
    PK_WAKE_SURFACE_SERVICE, /* C3 — service vehicle */
    PK_WAKE_SURFACE_OBSTACLE,/* C4..C7 */
} pk_wake_t;

/*
 * 字段级新鲜度
 * ------------
 * 「这架飞机还在」与「这个字段还新鲜」是两件事。DF11 全呼叫应答只带地址：
 * 收到它只能证明飞机还在天上，不能证明 60 秒前那一帧的高度/位置/速度仍然
 * 成立。老实现只有一个目标级 last_seen_us，于是一串 DF11 就能给所有字段
 * 无限续命，陈旧值一路流进地图、威胁判定、落盘、本机绑定与 BLE/GDL90。
 *
 * 每个字段因此各带一个 *_us 时间戳，记「该字段最后一次被**携带它的报文**
 * 写入」的时刻。读取边界（aircraft_state_snapshot / _get_own）按
 * AIRCRAFT_STALE_AGE_US 逐字段过期：过期的 have_* 置 false 且值清零，目标
 * 条目本身保留（飞机还在，只是不知道它的高度）。
 *
 * 时间戳本身不随过期清零——它是「上一次有据可依是什么时候」的证据，
 * 也是唯一能把「有据的 on_ground=false」与「过期后归零的 false」区分开的
 * 东西（值都是 false，只有 air_ground_us 不同）。
 *
 * 写入方约定：只有真正携带该字段的报文才允许打戳。明确的 N/A（编码值 0、
 * 状态位为 0）必须**立刻**把 have_* 清掉并把值归零，而不是放着让它按龄
 * 过期——中间那 60 秒里，任何漏检 have_* 的读法都会画出一支笃定的箭头。
 */
typedef struct {
    uint32_t icao24;          /* 0 → empty slot */
    int64_t  last_seen_us;    /* 目标级：任何一帧（含 DF11）都刷新 */

    bool     have_callsign;
    char     callsign[AIRCRAFT_CALLSIGN_LEN];
    int64_t  callsign_us;

    /* PK_WAKE_NONE until DF17/18 metype 1-4 seen. 与呼号同源但不同帧
     * 序（呼号可能校验失败而尾流等级仍然有效），各自打戳。 */
    pk_wake_t wake;
    int64_t  wake_us;

    bool     have_altitude;   /* 气压高度：GDL90 / 相对高度 / 相位判定用 */
    int      altitude_ft;     /* converted from meters if needed */
    int64_t  altitude_us;

    /* GNSS 椭球高（DF17 TC20-22 的 HAE）。与气压高度是两个基准，同一地点
     * 可能差上百英尺，**不得互相顶替**——线上协议与交通相对高度要的都是
     * 气压高度。 */
    bool     have_gnss_altitude;
    int      gnss_altitude_ft;
    int64_t  gnss_altitude_us;

    bool     have_position;
    double   lat;
    double   lon;
    int64_t  position_us;     /* timestamp of the last decoded fix */

    /* 地速与地面航迹各自独立有效：地面报文（metype 5-8）会只给出其中
     * 一个（MOV=0 或 S=0），空中速度帧的两个分量也会各自 N/A。 */
    bool     have_ground_speed;
    int      ground_speed_kt;
    int64_t  ground_speed_us;

    bool     have_heading;    /* 地面航迹，不是空中航向 */
    int      heading_deg;     /* 0..359 */
    int64_t  heading_us;

    /* 复合位：地速与航迹同时有效才算有一支可用的速度矢量。GDL90 的
     * Traffic Report 与地图上的箭头要的是这一位。 */
    bool     have_velocity;

    bool     have_vertical_rate;
    int      vert_rate_fpm;   /* signed; positive = climb */
    int64_t  vert_rate_us;

    /* 空速 + 空中航向（DF17 metype 19 subtype 3/4）。与地速矢量是两条
     * 独立链路：有风时航向与航迹能差十几度，绝不能互相顶替。 */
    bool     have_airspeed;
    int      airspeed_kt;     /* IAS 或 TAS，取决于发射方 */
    int64_t  airspeed_us;

    bool     have_airspeed_heading;
    int      airspeed_heading_deg;  /* 0..359 */
    int64_t  airspeed_heading_us;

    bool     have_squawk;     /* set when a DF5/DF21 identity reply was
                                 decoded for this aircraft */
    int      squawk;          /* 4-digit octal Mode-A code 0000..7777 */
    int64_t  squawk_us;

    /* 空地状态是**三态**：have_air_ground=false 表示"不知道"。身份帧、
     * DF11、DF20/21 都不携带空地信息，把"不知道"当成"已知在空中"会让
     * GDL90 的 airborne 位和 pk_flight_phase 的 UC6 矛盾检测拿到一个
     * 凭空的正向断言。on_ground 只在 have_air_ground 为真时可读。 */
    bool     have_air_ground;
    bool     on_ground;       /* true if the last air/ground-bearing frame
                                 was DF17 metype 5-8 (surface position) */
    int64_t  air_ground_us;

    /* Turn-rate tracking, used to DERIVE bank angle from coordinated-
     * turn geometry (bank = atan(V × ω / g)). ADS-B doesn't broadcast
     * bank or pitch, so the only way to surface the aircraft's actual
     * attitude over Mode-S is to estimate it from the velocity vector's
     * evolution between successive DF17 metype 19 frames.
     *
     *   - prev_heading_deg / prev_velocity_us hold the previous sample;
     *   - turn_rate_dps is an exponentially-smoothed
     *     delta-heading / delta-time in degrees per second
     *     (signed; + = right turn).
     * have_turn_rate flips on once we've consumed at least two velocity
     * samples within a sane temporal window (≈100 ms to 5 s apart).
     *
     * turn_rate_us 记的是 EMA **最后一次真的被更新**的时刻，不是最后一次
     * 收到速度帧的时刻：中断 61 s 后再来一帧速度，dt 超窗、EMA 不更新，
     * 那个估计值依然是一分钟前的，不该跟着新帧复活。 */
    bool     have_turn_rate;
    float    turn_rate_dps;
    int      prev_heading_deg;
    int64_t  prev_velocity_us;
    int64_t  turn_rate_us;
} aircraft_t;

/* Single-letter abbreviation for the list view (one column). Returns
 * ' ' (space) for PK_WAKE_NONE so the column renders blank rather than
 * showing a misleading code. */
char pk_wake_letter(pk_wake_t w);

/* Human-readable name for the detail pane. Returns "" for PK_WAKE_NONE. */
const char *pk_wake_name(pk_wake_t w);

/*
 * Derive an estimated bank angle (degrees, signed: + = right bank)
 * from the bound aircraft's smoothed turn rate and ground speed using
 * the coordinated-turn formula  bank = atan(V × ω / g)  where V is
 * approximated by GS in m/s and ω is the yaw rate in rad/s.
 *
 * Returns true and fills *out_bank_deg only when:
 *   - the aircraft has at least 2 fresh velocity samples
 *     (have_turn_rate is set);
 *   - the latest velocity report is within max_age_us of now_us;
 *   - ground speed is high enough (≥ 60 kt) for the coordinated-turn
 *     assumption to be sensible — at lower speeds the heading is
 *     dominated by skidding / wind and the derivation devolves to
 *     noise.
 *
 * Returns false (and leaves *out_bank_deg untouched) otherwise. Caller
 * is expected to fall back to IMU roll or render the attitude
 * indicator un-banked in that case.
 *
 * Note this is the AIRCRAFT'S bank under the coordinated-turn
 * assumption — useful when own-ship is bound to a transponder; the
 * kit's own IMU stays authoritative when there's no ADS-B reference.
 */
bool pk_aircraft_derive_bank(uint32_t icao24, int64_t now_us,
                             int64_t max_age_us, float *out_bank_deg);

/* Reset table. Call once on boot. */
void aircraft_state_init(void);

/*
 * Ingest one CRC-valid Mode-S frame. Caller passes the parsed
 * `mode_s_msg` (from mode_s.c) plus a monotonic timestamp in
 * microseconds (esp_timer_get_time()). The function updates the
 * relevant fields of the aircraft's slot, allocating a new slot or
 * evicting the LRU one on collision.
 *
 * Position decoding (CPR) is *not* duplicated here — that lives in
 * cpr_decode.c. The position field is populated by passing the freshly
 * decoded cpr_position_t through aircraft_state_update_position().
 */
void aircraft_state_ingest(const struct mode_s_msg *mm, int64_t now_us);

/*
 * Override an aircraft's position. Used by adsb_link_task.c after running
 * cpr_decode_position() so the BLE traffic report carries lat/lon.
 */
void aircraft_state_update_position(uint32_t icao24,
                                    double lat, double lon,
                                    int64_t now_us);

/*
 * Copy a snapshot of recently-seen aircraft into *out (capacity cap).
 * Returns the number of entries written. Aircraft whose last_seen_us
 * is older than max_age_us are skipped. Pass AIRCRAFT_STALE_AGE_US
 * to get the "fresh contacts" window that the BLE/GDL90 emitter uses;
 * pass a larger value (e.g. 30 minutes) for diagnostic history dumps.
 * Snapshot is taken under an internal mutex; safe to call from any task.
 *
 * max_age_us 只筛**目标**。写出的每一条都另外过一道固定
 * AIRCRAFT_STALE_AGE_US 的字段级过期（见 aircraft_t 头注）：诊断用的
 * 30 分钟窗口能看到目标条目，但看不到它半小时前的高度。
 */
size_t aircraft_state_snapshot(aircraft_t *out, size_t cap, int64_t now_us,
                               int64_t max_age_us);

/*
 * Copy the slot for `icao24` into *out, but only if its last_seen_us
 * is within max_age_us of now_us. Returns true on fresh hit, false if
 * not present or stale. Takes the same internal mutex as
 * aircraft_state_snapshot/_ingest; safe to call from any task. Used
 * by the PFD to source own-ship altitude / VS / GS from the live ADS-B
 * receive pipeline.
 *
 * 与 aircraft_state_snapshot() **同一套字段级判据**：调用方传多大的
 * max_age_us（adsb_link_task 的查表窗口是 24 小时）都不会让字段跟着
 * 活那么久。PFD / 本机绑定 / 落盘都从这里取数，判据分家就等于同一架
 * 飞机在两个页面上一个显示高度一个不显示。
 */
bool aircraft_state_get_own(uint32_t icao24, int64_t now_us,
                            int64_t max_age_us, aircraft_t *out);
