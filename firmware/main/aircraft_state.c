/*
 * aircraft_state.c — per-aircraft state aggregation.
 *
 * Open-addressing table indexed by ICAO % capacity, linear probing for
 * collisions, LRU eviction when the table is full. Mutex-protected so
 * the ADS-B link task (writer via aircraft_state_ingest) and
 * ble_gatt_task (reader via aircraft_state_snapshot) can run on
 * different cores.
 */

#include "aircraft_state.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "config_demo.h"  /* pk_demo_enabled —— 演示模式接管目标表快照 */
#include "pk_callsign.h"  /* pk_callsign_sanitize —— 拒收含保留码位的呼号 */
#include "demo_data.h"
#include "mode_s.h"   /* struct mode_s_msg + MODE_S_UNIT_FEET */
#include "ui_state.h" /* pk_ui_get_own_icao() — pin the bound own-ship
                       * slot against LRU eviction (see lookup_or_claim) */

static const char *TAG = "aircraft";

/* s_table lives in PSRAM (EXT_RAM_BSS). aircraft_t grew enough with the
 * squawk / wake / on_ground additions that keeping all 64 slots in
 * internal DRAM started squeezing ESP-Hosted's boot-time timer-task
 * allocation off the heap. Mutex-guarded reads from PSRAM are cheap
 * enough for the ingest path (~30-50 calls/s). */
static EXT_RAM_BSS_ATTR aircraft_t s_table[AIRCRAFT_TABLE_CAPACITY];
/* 两处长度必须同值：pk_callsign_sanitize 按 PK_CALLSIGN_LEN 写出参，
 * 结果直接 memcpy 进 aircraft_t.callsign[AIRCRAFT_CALLSIGN_LEN]。 */
_Static_assert(PK_CALLSIGN_LEN == AIRCRAFT_CALLSIGN_LEN,
               "callsign buffer length mismatch");

static SemaphoreHandle_t           s_lock;

static void take_lock(void)    { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void release_lock(void) { xSemaphoreGive(s_lock); }

void aircraft_state_init(void)
{
    memset(s_table, 0, sizeof(s_table));
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

/* Locate an existing slot or claim a free one. LRU eviction on full table.
 * Must be called under s_lock.
 *
 * Own-ship pinning: the slot whose icao24 matches pk_ui_get_own_icao()
 * is excluded from LRU candidacy. After a packet-loss gap the PFD
 * relies on the slot's preserved altitude / velocity / position to
 * keep ALT/VS/GS visible the instant the bound aircraft's first
 * post-gap message arrives — without pinning, that slot can get
 * evicted under a busy sky (≥ 64 unique ICAOs) and reappear empty,
 * which makes the PFD look like it "lost" the binding even though
 * pk_ui_get_own_icao() still points at the correct ICAO. */
static aircraft_t *lookup_or_claim(uint32_t icao24, int64_t now_us)
{
    const uint32_t base     = icao24 % AIRCRAFT_TABLE_CAPACITY;
    const uint32_t own_icao = pk_ui_get_own_icao();
    aircraft_t    *empty    = NULL;
    aircraft_t    *lru      = NULL;

    for (uint32_t step = 0; step < AIRCRAFT_TABLE_CAPACITY; ++step) {
        aircraft_t *s = &s_table[(base + step) % AIRCRAFT_TABLE_CAPACITY];
        if (s->icao24 == icao24) return s;
        if (!empty && s->icao24 == 0) empty = s;
        /* Skip the bound own-ship slot when picking an eviction victim. */
        if (own_icao != 0 && s->icao24 == own_icao) continue;
        if (lru == NULL || s->last_seen_us < lru->last_seen_us) lru = s;
    }

    aircraft_t *chosen = empty ? empty : lru;
    if (chosen == NULL) {
        /* Pathological: every slot is pinned. Can only happen if the
         * own_icao matches the entire table, which lookup_or_claim's
         * "at most one slot per icao24" invariant prevents. Defensive
         * fallback: just reuse the home slot so we don't crash. */
        ESP_LOGW(TAG, "lookup_or_claim: no eviction candidate for %06lX "
                      "(own_icao=%06lX, table full of pinned?) — using "
                      "home slot",
                 (unsigned long)icao24, (unsigned long)own_icao);
        chosen = &s_table[base];
    }
    memset(chosen, 0, sizeof(*chosen));
    chosen->icao24       = icao24;
    chosen->last_seen_us = now_us;
    return chosen;
}

/* Decode the ADS-B aircraft category (DF17 metype 1-4) into the
 * compact pk_wake_t enum. Caller passes the message's metype (1..4 →
 * Category Sets D/C/B/A — note the ordering is reversed) and mesub
 * (0..7). Returns PK_WAKE_NONE for combinations we don't categorise. */
static pk_wake_t decode_wake_category(int metype, int mesub)
{
    /* DO-260B Table 2-67: metype 4 = Set A (most common — airborne
     * powered), metype 3 = Set B (gliders, LTAs, UAV, ...), metype 2
     * = Set C (surface vehicles), metype 1 = Set D (reserved). */
    if (metype == 4) {
        switch (mesub) {
        case 1: return PK_WAKE_LIGHT;
        case 2: return PK_WAKE_SMALL;
        case 3: return PK_WAKE_LARGE;
        case 4: return PK_WAKE_HIGH_VORTEX;
        case 5: return PK_WAKE_HEAVY;
        case 6: return PK_WAKE_HIGH_PERF;
        case 7: return PK_WAKE_ROTOR;
        default: return PK_WAKE_NONE;
        }
    }
    if (metype == 3) {
        switch (mesub) {
        case 1: return PK_WAKE_GLIDER;
        case 2: return PK_WAKE_LTA;
        case 3: return PK_WAKE_PARACHUTE;
        case 4: return PK_WAKE_ULTRALIGHT;
        case 6: return PK_WAKE_UAV;
        case 7: return PK_WAKE_SPACE;
        default: return PK_WAKE_NONE;
        }
    }
    if (metype == 2) {
        switch (mesub) {
        case 1: return PK_WAKE_SURFACE_EMERG;
        case 3: return PK_WAKE_SURFACE_SERVICE;
        case 4: case 5: case 6: case 7:
            return PK_WAKE_SURFACE_OBSTACLE;
        default: return PK_WAKE_NONE;
        }
    }
    return PK_WAKE_NONE;
}

char pk_wake_letter(pk_wake_t w)
{
    switch (w) {
    case PK_WAKE_LIGHT:           return 'L';
    case PK_WAKE_SMALL:           return 'S';
    case PK_WAKE_LARGE:           return 'M';   /* M = Medium (FAA convention) */
    case PK_WAKE_HIGH_VORTEX:     return 'V';
    case PK_WAKE_HEAVY:           return 'H';
    case PK_WAKE_HIGH_PERF:       return 'F';   /* F = Fast / Fighter */
    case PK_WAKE_ROTOR:           return 'R';
    case PK_WAKE_GLIDER:          return 'G';
    case PK_WAKE_LTA:             return 'B';   /* B = Balloon */
    case PK_WAKE_PARACHUTE:       return 'P';
    case PK_WAKE_ULTRALIGHT:      return 'U';
    case PK_WAKE_UAV:             return 'D';   /* D = Drone */
    case PK_WAKE_SPACE:           return 'X';
    case PK_WAKE_SURFACE_EMERG:   return 'E';
    case PK_WAKE_SURFACE_SERVICE: return 'T';   /* T = Tug / service */
    case PK_WAKE_SURFACE_OBSTACLE:return 'O';
    case PK_WAKE_NONE:
    default:                      return ' ';
    }
}

const char *pk_wake_name(pk_wake_t w)
{
    switch (w) {
    case PK_WAKE_LIGHT:            return "Light";
    case PK_WAKE_SMALL:            return "Small";
    case PK_WAKE_LARGE:            return "Medium";
    case PK_WAKE_HIGH_VORTEX:      return "B757-class";
    case PK_WAKE_HEAVY:            return "Heavy";
    case PK_WAKE_HIGH_PERF:        return "High-perf";
    case PK_WAKE_ROTOR:            return "Rotorcraft";
    case PK_WAKE_GLIDER:           return "Glider";
    case PK_WAKE_LTA:              return "Balloon/LTA";
    case PK_WAKE_PARACHUTE:        return "Parachute";
    case PK_WAKE_ULTRALIGHT:       return "Ultralight";
    case PK_WAKE_UAV:              return "Drone/UAV";
    case PK_WAKE_SPACE:            return "Spacecraft";
    case PK_WAKE_SURFACE_EMERG:    return "Emergency vehicle";
    case PK_WAKE_SURFACE_SERVICE:  return "Service vehicle";
    case PK_WAKE_SURFACE_OBSTACLE: return "Surface object";
    case PK_WAKE_NONE:
    default:                       return "";
    }
}

/* 高度单位换算。米→英尺一律**截断**，与 mode_s 的 GNSS 判据同口径
 * （2570 m = 8431 ft，不是 8432）：这是个测量量不是计数，多出来的那
 * 一英尺没有任何来源，只会让两处算出的数对不上。 */
static int altitude_to_feet(const struct mode_s_msg *mm)
{
    return (mm->unit == MODE_S_UNIT_METERS)
               ? (int)((double)mm->altitude * 3.28084)
               : mm->altitude;
}

/* 空地状态：只有真的携带空地信息的报文才允许调用（地面位置帧、空中位置
 * 帧、空中速度帧）。身份帧 / DF11 / DF20 / DF21 一律不碰，让它保持
 * "不知道"。 */
static void set_air_ground(aircraft_t *a, bool on_ground, int64_t now_us)
{
    a->on_ground      = on_ground;
    a->have_air_ground = true;
    a->air_ground_us  = now_us;
}

void aircraft_state_ingest(const struct mode_s_msg *mm, int64_t now_us)
{
    if (mm == NULL || !mm->crcok || mm->errorbit >= 0) return;
    /* Only DF5 / DF11 / DF17 / DF18 / DF20 / DF21 carry useful per-aircraft
     * data. DF5 (surveillance identity reply) carries Squawk; DF20
     * (Comm-B altitude) carries altitude; DF21 (Comm-B identity) carries
     * Squawk. Identity-bearing DFs need a separate ingest hook below. */
    const int df = mm->msgtype;
    if (df != 5 && df != 11 && df != 17 && df != 18 && df != 20 && df != 21) return;

    /* DF18 的 CF/IMF 可能说"这个地址不是 ICAO 地址"（TIS-B 的 Mode-A 码 +
     * 航迹文件号、匿名 ADS-B、TIS-B 管理报文）。用它开槽会凭空造出一架
     * 飞机，而且这个"地址"下一秒就可能被另一个航迹文件复用——同一个槽位
     * 会在两架真实飞机之间来回跳。 */
    if (!mm->aa_is_icao) return;

    const uint32_t icao24 = ((uint32_t)mm->aa1 << 16)
                          | ((uint32_t)mm->aa2 << 8)
                          | (uint32_t)mm->aa3;
    if (icao24 == 0) return;

    take_lock();
    aircraft_t *a = lookup_or_claim(icao24, now_us);
    /* 目标级时间戳：任何一帧都刷新，包括只带地址的 DF11。字段级时间戳
     * 只由携带该字段的分支各自打——这两者混为一谈就是整个问题的根源。 */
    a->last_seen_us = now_us;

    /* DF17 / DF18 carry the Extended Squitter sub-types. */
    if (df == 17 || df == 18) {
        if (mm->metype >= 1 && mm->metype <= 4) {
            /* Aircraft identification — callsign + wake category.
             * metype 1..4 maps to Category Sets D/C/B/A; mesub gives
             * the in-set position (Light/Small/Large/Heavy/etc.). */
            char cs[PK_CALLSIGN_LEN];
            if (pk_callsign_sanitize(mm->flight, cs)) {
                memcpy(a->callsign, cs, sizeof(cs));
                a->have_callsign = true;
                a->callsign_us   = now_us;
            }
            /* 校验没过就什么都不动：保留上一次收到的好呼号，have_callsign
             * 与它的时间戳也维持原样（那份呼号自己会按龄过期）。为什么不
             * "剔掉非法字符再显示"见 pk_callsign.h 头注。 */
            pk_wake_t w = decode_wake_category(mm->metype, mm->mesub);
            if (w != PK_WAKE_NONE) {
                a->wake    = w;
                a->wake_us = now_us;
            }
            /* 身份帧不携带空地信息——不碰 have_air_ground。 */
        } else if (mm->metype >= 5 && mm->metype <= 8) {
            /* Surface position — aircraft is on the ground (taxi /
             * runway / apron). The CPR itself is decoded separately in
             * adsb_link_task.c via cpr_decode_surface_local() (local/
             * single-frame decode — ground targets squitter too
             * infrequently once stationary for the global odd+even
             * pairing window in cpr_decode.c to stay useful). Here we
             * just flag ground state and ingest the ground speed/track
             * this message type itself carries: a surface target NEVER
             * sends metype 19 (airborne-velocity-only), so this is the
             * ONLY source of speed/heading while taxiing — don't wait
             * for a metype 19 that will never arrive. */
            set_air_ground(a, /*on_ground=*/true, now_us);
            /* MOV=0 与 S=0 都是**明确的 N/A**，不是"这一帧没提"：它们必须
             * 立刻推翻上一帧的地速/航迹。目标级 last_seen 会照常被这一帧
             * 刷新，靠按龄过期是清不掉它们的。 */
            if (mm->surface_ground_speed >= 0.0) {
                a->ground_speed_kt   = (int)lround(mm->surface_ground_speed);
                a->have_ground_speed = true;
                a->ground_speed_us   = now_us;
            } else {
                a->ground_speed_kt   = 0;
                a->have_ground_speed = false;
            }
            if (mm->surface_track_valid) {
                a->heading_deg  = ((int)lround(mm->surface_track)) % 360;
                a->have_heading = true;
                a->heading_us   = now_us;
            } else {
                a->heading_deg  = 0;
                a->have_heading = false;
            }
            a->have_velocity = a->have_ground_speed && a->have_heading;
            /* Surface frames carry no vertical rate field (those bits
             * are reused for MOV/TRK) and a grounded target isn't
             * climbing/descending — 明确置为"不可用"而不是 0 fpm，
             * 也不让起飞前最后一次空中垂速赖着不走。 */
            a->vert_rate_fpm      = 0;
            a->have_vertical_rate = false;
        } else if (mm->metype >= 9 && mm->metype <= 18) {
            /* Airborne position — altitude only (position arrives via CPR
             * path, see aircraft_state_update_position).
             *
             * 判据是解码器给的 altitude_valid，**不是** altitude != 0：
             * Gillham 000000011010 是一个合法的 0 英尺（刚接地/低平飞的
             * 老应答机），拿 0 当哨兵会把它当解码失败丢掉。解不出来时保留
             * 上一次的好值，让它按龄自己过期。 */
            if (mm->altitude_valid &&
                mm->altitude_source == MODE_S_ALTITUDE_BARO) {
                a->altitude_ft   = altitude_to_feet(mm);
                a->have_altitude = true;
                a->altitude_us   = now_us;
            }
            /* Airborne position implies the aircraft is no longer on
             * the ground — 这是一次**有据的**否定，要连时间戳一起打，
             * 否则它与"过期后归零的 false"在读取端无法区分。 */
            set_air_ground(a, /*on_ground=*/false, now_us);
        } else if (mm->metype >= 20 && mm->metype <= 22) {
            /* GNSS 椭球高（HAE）。单独存：它不是气压高度，GDL90 线上、
             * 交通相对高度、相位判定要的都是气压高度，顶替过去等于把一个
             * 差上百英尺的量当成同一个量用。 */
            if (mm->altitude_valid &&
                mm->altitude_source == MODE_S_ALTITUDE_GNSS) {
                a->gnss_altitude_ft   = altitude_to_feet(mm);
                a->have_gnss_altitude = true;
                a->gnss_altitude_us   = now_us;
            }
            set_air_ground(a, /*on_ground=*/false, now_us);
        } else if (mm->metype == 19 && mm->mesub >= 1 && mm->mesub <= 4) {
            /* Airborne velocity (sub-types 1-4).
             *
             * DO-260B 里 metype 19 只在**空中**发送——这是一次真实的空中
             * 证据，与空中位置帧同等。 */
            set_air_ground(a, /*on_ground=*/false, now_us);

            /* mm->vert_rate is the 9-bit encoded value per RTCA DO-260B
             * Table 2-69: real fpm = (encoded - 1) * 64; encoded == 0
             * means "vertical rate information not available" —— 解码器
             * 把这一位单独给了 vert_rate_valid，"不可用"与"0 fpm"是两件
             * 事。原实现把两者都写成 0 fpm 并保持有效，于是一架不报垂速
             * 的飞机在 PFD 上是"VS 0"。 */
            if (mm->vert_rate_valid) {
                int v_fpm = (mm->vert_rate - 1) * 64;
                a->vert_rate_fpm      = (mm->vert_rate_sign == 0) ? v_fpm : -v_fpm;
                a->have_vertical_rate = true;
                a->vert_rate_us       = now_us;
            } else {
                a->vert_rate_fpm      = 0;
                a->have_vertical_rate = false;
            }

            if (mm->mesub == 1 || mm->mesub == 2) {
                /* 地速矢量。 */
                if (mm->heading_is_valid) {
                    const int hdg = ((int)lround(mm->heading)) % 360;
                    /* Update turn-rate estimate BEFORE overwriting
                     * heading_deg — we need the previous and current
                     * values to compute the delta. Sample-to-sample delta
                     * is noisy (heading_deg is integer-valued, dt jitters
                     * around 1 s), so smooth with an EMA. Skip the update
                     * if dt is unrealistic (< 100 ms means frame
                     * duplication; > 5 s means stale gap, restart fresh).
                     *
                     * 只有 subtype 1/2 才进这里：subtype 3/4 给的是空中
                     * 航向，拿它去和地面航迹做差会算出一个纯属虚构的
                     * 转弯率，再经协调转弯公式变成一个假坡度。 */
                    if (a->prev_velocity_us != 0) {
                        int64_t dt_us = now_us - a->prev_velocity_us;
                        if (dt_us > 100000 && dt_us < 5000000) {
                            int delta = hdg - a->prev_heading_deg;
                            while (delta >  180) delta -= 360;
                            while (delta < -180) delta += 360;
                            float new_rate_dps =
                                (float)delta * 1000000.0f / (float)dt_us;
                            if (a->have_turn_rate) {
                                a->turn_rate_dps =
                                    0.5f * a->turn_rate_dps + 0.5f * new_rate_dps;
                            } else {
                                a->turn_rate_dps  = new_rate_dps;
                                a->have_turn_rate = true;
                            }
                            a->turn_rate_us = now_us;
                        }
                    }
                    a->prev_heading_deg = hdg;
                    a->prev_velocity_us = now_us;

                    a->heading_deg  = hdg;
                    a->have_heading = true;
                    a->heading_us   = now_us;
                } else {
                    /* 分量 N/A → 航迹明确不可用，连值一起清。 */
                    a->heading_deg  = 0;
                    a->have_heading = false;
                }
                if (mm->velocity_valid) {
                    a->ground_speed_kt   = mm->velocity;
                    a->have_ground_speed = true;
                    a->ground_speed_us   = now_us;
                } else {
                    a->ground_speed_kt   = 0;
                    a->have_ground_speed = false;
                }
                a->have_velocity = a->have_ground_speed && a->have_heading;
            } else {
                /* subtype 3/4：空速 + 空中航向。**不碰**地速/地面航迹——
                 * 它们是两条独立链路，互相顶替会让地图上的箭头指向一个
                 * 有风时差十几度的方向。 */
                if (mm->velocity_valid) {
                    a->airspeed_kt   = mm->velocity;
                    a->have_airspeed = true;
                    a->airspeed_us   = now_us;
                } else {
                    a->airspeed_kt   = 0;
                    a->have_airspeed = false;
                }
                if (mm->heading_is_valid) {
                    a->airspeed_heading_deg  = ((int)lround(mm->heading)) % 360;
                    a->have_airspeed_heading = true;
                    a->airspeed_heading_us   = now_us;
                } else {
                    a->airspeed_heading_deg  = 0;
                    a->have_airspeed_heading = false;
                }
            }
        }
    }

    /* DF5 (Surveillance Identity Reply) and DF21 (Comm-B Identity
     * Reply) both decode the Squawk (4-octal Mode-A code) into
     * mm->identity. Other DFs leave it as stack residue, so only
     * ingest from these two. The decoder always populates the
     * identity field unconditionally (mode-s.c:412-428 is outside
     * any if-block), but for DF other than 5/21 the bit positions
     * map to altitude / other things — interpreting those as a
     * Squawk would be garbage. */
    if (df == 5 || df == 21) {
        a->squawk      = mm->identity;
        a->have_squawk = true;
        a->squawk_us   = now_us;
        /* 身份应答不携带空地信息（fs 里的 airborne/on-the-ground 位在真实
         * 空域里被大量应答机填成常数，不足以当断言）——不碰它。 */
    }

    /* DF20 (Comm-B altitude reply) carries an AC13 altitude field.
     * DF21 (Comm-B identity reply) occupies the same bit positions
     * with the Squawk identity code instead — there is NO altitude in
     * a DF21 frame, and the vendored mode_s_decode() reflects that by
     * skipping decode_ac13_field() for msgtype 21 (mode-s.c:458-463
     * only handles DF0/4/16/20). Crucially `struct mode_s_msg mm;` in
     * mode_s_detect (mode-s.c:804) is uninitialised, so for a DF21
     * frame `mm->altitude` is whatever the previous decoder call left
     * on the stack — almost always a neighbour aircraft's recently-
     * decoded altitude. Ingesting that scribbles random "altitudes"
     * (often 5000 / 16700 / 33000 ft, depending on what's overhead)
     * over the real aircraft's known altitude, which is exactly the
     * "altitude jumps between 5000, 19900, 33000 for CSZ993X" symptom.
     *
     * 判据同样是解码器的 altitude_valid，不是 altitude != 0：DF20 是
     * TC9-18 之外的第二条气压高度入口，只发 DF20 的目标全靠这一处打戳，
     * 漏掉的话它的高度会在 60 s 后凭空过期。 */
    if (df == 20 && mm->altitude_valid &&
        mm->altitude_source == MODE_S_ALTITUDE_BARO) {
        a->altitude_ft   = altitude_to_feet(mm);
        a->have_altitude = true;
        a->altitude_us   = now_us;
    }
    release_lock();
}

/*
 * 字段级过期——读取边界唯一的一处实现。
 *
 * 作用在**拷贝出去的那一份**上，不动 s_table：过期是 now_us 的纯函数，
 * 每次读各算各的，表里留着原值和原时间戳，下一帧同类报文照常续上。
 *
 * 窗口固定 AIRCRAFT_STALE_AGE_US，与调用方传的目标级 max_age_us 无关。
 * 诊断页要看 30 分钟内出现过哪些目标，但没人想看到它半小时前的高度被
 * 当成现在的高度。
 */
#define FIELD_STALE(now_us, ts) \
    ((int64_t)((now_us) - (ts)) > (int64_t)AIRCRAFT_STALE_AGE_US)

static void expire_stale_fields(aircraft_t *a, int64_t now_us)
{
    if (a->have_callsign && FIELD_STALE(now_us, a->callsign_us)) {
        a->have_callsign = false;
        a->callsign[0]   = '\0';
    }
    if (a->wake != PK_WAKE_NONE && FIELD_STALE(now_us, a->wake_us)) {
        a->wake = PK_WAKE_NONE;
    }
    if (a->have_altitude && FIELD_STALE(now_us, a->altitude_us)) {
        a->have_altitude = false;
        a->altitude_ft   = 0;
    }
    if (a->have_gnss_altitude && FIELD_STALE(now_us, a->gnss_altitude_us)) {
        a->have_gnss_altitude = false;
        a->gnss_altitude_ft   = 0;
    }
    if (a->have_position && FIELD_STALE(now_us, a->position_us)) {
        a->have_position = false;
        a->lat = 0.0;
        a->lon = 0.0;
    }
    if (a->have_ground_speed && FIELD_STALE(now_us, a->ground_speed_us)) {
        a->have_ground_speed = false;
        a->ground_speed_kt   = 0;
    }
    if (a->have_heading && FIELD_STALE(now_us, a->heading_us)) {
        a->have_heading = false;
        a->heading_deg  = 0;
    }
    /* 复合位永远由分量重算，不单独存新鲜度：两个分量的过期时刻不同，
     * 各自过期后 have_velocity 必须跟着塌下来。 */
    a->have_velocity = a->have_ground_speed && a->have_heading;

    if (a->have_vertical_rate && FIELD_STALE(now_us, a->vert_rate_us)) {
        a->have_vertical_rate = false;
        a->vert_rate_fpm      = 0;
    }
    if (a->have_airspeed && FIELD_STALE(now_us, a->airspeed_us)) {
        a->have_airspeed = false;
        a->airspeed_kt   = 0;
    }
    if (a->have_airspeed_heading &&
        FIELD_STALE(now_us, a->airspeed_heading_us)) {
        a->have_airspeed_heading = false;
        a->airspeed_heading_deg  = 0;
    }
    if (a->have_squawk && FIELD_STALE(now_us, a->squawk_us)) {
        a->have_squawk = false;
        a->squawk      = 0;
    }
    /* 空地状态两位一起回到"不知道"。只清 on_ground 会把它变成"已知在
     * 空中"——一架早已落地的飞机会被 GDL90 断言成空中交通。 */
    if (a->have_air_ground && FIELD_STALE(now_us, a->air_ground_us)) {
        a->have_air_ground = false;
        a->on_ground       = false;
    }
    if (a->have_turn_rate && FIELD_STALE(now_us, a->turn_rate_us)) {
        a->have_turn_rate = false;
        a->turn_rate_dps  = 0.0f;
    }
}

void aircraft_state_update_position(uint32_t icao24,
                                    double lat, double lon,
                                    int64_t now_us)
{
    if (icao24 == 0) return;
    take_lock();
    aircraft_t *a   = lookup_or_claim(icao24, now_us);
    a->lat          = lat;
    a->lon          = lon;
    a->position_us  = now_us;
    a->last_seen_us = now_us;
    a->have_position = true;
    release_lock();
}

/* 合成目标的字段补戳，见 aircraft_state_snapshot 的演示分支。
 * 无条件写：have_* 为 false 的字段有没有时间戳都不影响判定。 */
static void demo_stamp_fields(aircraft_t *a)
{
    const int64_t t = a->last_seen_us;
    a->callsign_us          = t;
    a->wake_us              = t;
    a->altitude_us          = t;
    a->gnss_altitude_us     = t;
    a->position_us          = t;
    a->ground_speed_us      = t;
    a->heading_us           = t;
    a->vert_rate_us         = t;
    a->airspeed_us          = t;
    a->airspeed_heading_us  = t;
    a->squawk_us            = t;
    a->air_ground_us        = t;
    a->turn_rate_us         = t;
}

/* qsort comparator: ascending by ICAO24. Stable row order is what the
 * list view + index-based cursor rely on; without it the hash-table
 * scan order shuffles whenever an aircraft enters or leaves the table
 * and the selection cursor lands on a different aircraft. */
static int cmp_aircraft_by_icao(const void *a, const void *b)
{
    uint32_t la = ((const aircraft_t *)a)->icao24;
    uint32_t lb = ((const aircraft_t *)b)->icao24;
    if (la < lb) return -1;
    if (la > lb) return  1;
    return 0;
}

size_t aircraft_state_snapshot(aircraft_t *out, size_t cap, int64_t now_us,
                               int64_t max_age_us)
{
    if (out == NULL || cap == 0) return 0;

    /*
     * 演示模式接管点。
     *
     * 选在这里而不是"往 s_table 里灌假飞机"，是一条安全边界：合成目标**永远
     * 不进真实融合表**，因此不会流进 record_sink（落盘的 ts 日志仍然只有真实
     * 报文），也不会污染 CPR 解码与 own-ship 绑定。屏幕上看到的假目标与硬盘里
     * 记下的真数据是两条独立的路径，事后回放不会把演示当成一次真实飞行。
     *
     * GDL90 那一路虽然也调本函数，但在 ble_gatt.c 的发射任务里被**显式**掐掉，
     * 见那里的注释——手机 App 分不出真假，只能不发。
     */
    if (pk_demo_enabled()) {
        size_t n = pk_demo_traffic(out, cap, now_us, /*anim_us=*/now_us,
                                   pk_demo_yaw_deg(now_us),
                                   pk_demo_own_alt_ft(now_us),
                                   0.0f, false);
        /* 新鲜度窗口照样生效：调用方传 60 s 与传 30 min 拿到的必须是不同的集合，
         * 否则看板的 SEEN 列与"过期消失"这条行为在演示模式下就试不出来。 */
        size_t k = 0;
        for (size_t i = 0; i < n; ++i) {
            if ((int64_t)(now_us - out[i].last_seen_us) > max_age_us) continue;
            if (k != i) out[k] = out[i];
            /* 合成目标是 pk_demo_traffic 按 now_us **现算**的，每个字段都
             * 与它的 last_seen_us 同刻——数据源不该为了融合表的内部记账
             * 去逐字段打戳，所以在这里统一补上，再走与真实目标同一道字段级
             * 过期。漏补的症状是屏上十几架飞机的高度/速度/呼号整片变成
             * ---，而目标本身还在。 */
            demo_stamp_fields(&out[k]);
            expire_stale_fields(&out[k], now_us);
            ++k;
        }
        return k;
    }

    size_t n = 0;
    take_lock();
    for (size_t i = 0; i < AIRCRAFT_TABLE_CAPACITY && n < cap; ++i) {
        const aircraft_t *s = &s_table[i];
        if (s->icao24 == 0) continue;
        if ((int64_t)(now_us - s->last_seen_us) > max_age_us) continue;
        out[n++] = *s;
    }
    release_lock();
    /* 字段级过期在锁外做：拿到的已经是各自独立的拷贝，不需要占着表锁。 */
    for (size_t i = 0; i < n; ++i) expire_stale_fields(&out[i], now_us);
    if (n > 1) qsort(out, n, sizeof(*out), cmp_aircraft_by_icao);
    return n;
}

bool aircraft_state_get_own(uint32_t icao24, int64_t now_us,
                            int64_t max_age_us, aircraft_t *out)
{
    if (icao24 == 0 || out == NULL) return false;
    /* 演示模式下一律"没有绑定的本机"，于是 pk_own_ship_resolve() 退到 GPS，
     * 而 GPS 那侧已经是合成数据。
     *
     * 不这么做的话会出现最糟的一种混合：真的 SDR 收到了用户此前绑定的那架
     * 飞机，本机位置/高度是**真的**，而周围的目标全是假的——屏上没有任何东西
     * 能提示这一半真一半假。要么全真，要么全假。 */
    if (pk_demo_enabled()) return false;
    bool fresh = false;
    take_lock();
    for (size_t i = 0; i < AIRCRAFT_TABLE_CAPACITY; ++i) {
        const aircraft_t *s = &s_table[i];
        if (s->icao24 != icao24) continue;
        if ((int64_t)(now_us - s->last_seen_us) <= max_age_us) {
            *out  = *s;
            fresh = true;
        }
        break;
    }
    release_lock();
    /* 与 aircraft_state_snapshot() 同一道字段级过期。调用方传 24 小时的
     * 查表窗口是为了"这架飞机曾经出现过吗"，不是为了让它 24 小时前的高度
     * 继续当真。 */
    if (fresh) expire_stale_fields(out, now_us);
    return fresh;
}

bool pk_aircraft_derive_bank(uint32_t icao24, int64_t now_us,
                             int64_t max_age_us, float *out_bank_deg)
{
    if (icao24 == 0 || out_bank_deg == NULL) return false;

    aircraft_t a;
    if (!aircraft_state_get_own(icao24, now_us, max_age_us, &a)) return false;
    if (!a.have_turn_rate || !a.have_velocity) return false;
    /* Coordinated-turn assumption breaks down at low GS — heading
     * changes are dominated by wind / yaw / skidding rather than a
     * banked turn. 60 kt is a typical airliner taxi / final-approach
     * floor; below it the derivation isn't meaningful. */
    if (a.ground_speed_kt < 60) return false;
    /* Also gate on freshness of the velocity sample: if the latest
     * DF17 metype 19 is more than ~10 s old, the turn-rate EMA is
     * stale (the aircraft may have already rolled out of the turn). */
    if ((int64_t)(now_us - a.prev_velocity_us) > 10LL * 1000000LL) {
        return false;
    }

    /* bank = atan(V × ω / g)   — coordinated-turn formula
     *   V in m/s              = kt × 0.514444
     *   ω in rad/s            = deg/s × π/180
     *   g = 9.81 m/s²
     * Sign of ω carries through to the bank sign (right turn → + ω →
     * + bank), matching the PFD attitude-indicator convention.
     *
     * Bank rate-of-change isn't smoothed beyond the EMA on turn-rate
     * itself — the PFD draws once every ~33 ms so any high-freq
     * residue averages out visually. Clamp to ±60° to keep wild noise
     * (e.g. heading wrap glitches we missed) from rotating the
     * horizon line all the way around. */
    const float KT_TO_MPS = 0.514444f;
    const float DEG_TO_RAD = 3.14159265358979323846f / 180.0f;
    const float G_MPSS = 9.81f;
    float v_mps     = (float)a.ground_speed_kt * KT_TO_MPS;
    float omega_rps = a.turn_rate_dps * DEG_TO_RAD;
    float bank_rad  = atanf(v_mps * omega_rps / G_MPSS);
    float bank_deg  = bank_rad * (180.0f / 3.14159265358979323846f);
    if (bank_deg >  60.0f) bank_deg =  60.0f;
    if (bank_deg < -60.0f) bank_deg = -60.0f;
    *out_bank_deg = bank_deg;
    return true;
}
