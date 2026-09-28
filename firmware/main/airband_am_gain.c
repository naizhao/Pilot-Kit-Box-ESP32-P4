#include "airband_am_gain.h"
#include <stddef.h>

/* Register encodings/dB estimates: OneOfEleven's Apache-2.0 am_fix.c in
 * egzumer/uv-k5-firmware-custom. These are reference estimates, not a measured
 * calibration of this board. Ordered minimum -> original -7dB maximum;
 * no gain above the existing 0x03BE is permitted. */
static const struct { uint16_t reg; int8_t db; } gains[] = {
    {0x0000,-93}, {0x0008,-91}, {0x0010,-88}, {0x0001,-87},
    {0x0009,-85}, {0x0011,-82}, {0x0002,-81}, {0x000A,-79},
    {0x0012,-76}, {0x0003,-75}, {0x000B,-73}, {0x0013,-70},
    {0x0004,-69}, {0x000C,-67}, {0x000D,-64}, {0x001C,-61},
    {0x001D,-58}, {0x001E,-55}, {0x001F,-52}, {0x003E,-50},
    {0x003F,-47}, {0x005E,-45}, {0x005F,-42}, {0x007E,-40},
    {0x007F,-37}, {0x009F,-34}, {0x00BF,-32}, {0x00DF,-30},
    {0x00FF,-28}, {0x01DF,-26}, {0x01FF,-24}, {0x02BF,-23},
    {0x02DF,-21}, {0x02FF,-19}, {0x035E,-17}, {0x035F,-14},
    {0x037E,-12}, {0x037F,-9}, {0x03BE,-7},
};
_Static_assert(sizeof(gains) / sizeof(gains[0]) == PK_AIRBAND_AM_GAIN_MAX_INDEX + 1,
               "gain index bounds");

void pk_airband_am_gain_reset(pk_airband_am_gain_t *s, unsigned applied, uint64_t now)
{
    if (s == NULL) return;
    if (applied > PK_AIRBAND_AM_GAIN_MAX_INDEX) applied = PK_AIRBAND_AM_GAIN_MAX_INDEX;
    *s = (pk_airband_am_gain_t){.applied_index = applied,
        .requested_index = applied, .last_ms = now, .reason = PK_AIRBAND_AM_GAIN_RESET};
}

bool pk_airband_am_gain_update(pk_airband_am_gain_t *s, bool valid, uint16_t raw, uint64_t now)
{
    if (s == NULL) return false;
    s->requested_index = s->applied_index;
    if (!valid || now < s->last_ms) {
        s->low_tracking = false;
        s->hold_until_ms = now + 300;
        s->reason = PK_AIRBAND_AM_GAIN_INVALID;
        s->last_ms = now;
        return false;
    }
    /* Missing observations do not prove that a signal stayed weak. */
    if (now - s->last_ms > 100) s->low_tracking = false;
    s->last_ms = now;
    s->reason = PK_AIRBAND_AM_GAIN_HOLD;
    if (raw > PK_AIRBAND_AM_TARGET_RSSI) {
        s->low_tracking = false;
        s->hold_until_ms = now + 300;
        unsigned next = s->applied_index;
        if (next > 0) {
            next--;
            if (raw >= PK_AIRBAND_AM_TARGET_RSSI + 20) {
                const int desired_db = pk_airband_am_gain_db(s->applied_index) -
                    ((int)raw - PK_AIRBAND_AM_TARGET_RSSI + 1) / 2;
                while (next > 0 && pk_airband_am_gain_db(next) > desired_db) next--;
                s->reason = PK_AIRBAND_AM_GAIN_FAST_ATTACK;
            } else {
                s->reason = PK_AIRBAND_AM_GAIN_ATTACK;
            }
        }
        s->requested_index = next;
    } else if (raw < PK_AIRBAND_AM_TARGET_RSSI - 12) {
        if (!s->low_tracking) {
            s->low_tracking = true;
            s->low_since_ms = now;
        }
        if (now - s->low_since_ms >= 300 && now >= s->hold_until_ms &&
            now >= s->next_release_ms && s->applied_index < PK_AIRBAND_AM_GAIN_MAX_INDEX) {
            s->requested_index++;
            s->reason = PK_AIRBAND_AM_GAIN_RELEASE;
        }
    } else {
        s->low_tracking = false;
    }
    return s->requested_index != s->applied_index;
}

void pk_airband_am_gain_commit(pk_airband_am_gain_t *s, unsigned index, uint64_t now)
{
    if (s == NULL || index > PK_AIRBAND_AM_GAIN_MAX_INDEX) return;
    if (index < s->applied_index) s->hold_until_ms = now + 300;
    if (index > s->applied_index) s->next_release_ms = now + 100;
    s->applied_index = index;
    s->requested_index = index;
}

int pk_airband_am_gain_db(unsigned index)
{ return gains[index <= PK_AIRBAND_AM_GAIN_MAX_INDEX ? index : PK_AIRBAND_AM_GAIN_MAX_INDEX].db; }
uint16_t pk_airband_am_gain_reg13(unsigned index)
{ return gains[index <= PK_AIRBAND_AM_GAIN_MAX_INDEX ? index : PK_AIRBAND_AM_GAIN_MAX_INDEX].reg; }
int16_t pk_airband_am_reference_rssi_x2(uint16_t raw, unsigned index)
{ return (int16_t)(raw & 0x1FFu) - 320 + 2 * (-7 - pk_airband_am_gain_db(index)); }
const char *pk_airband_am_gain_reason_name(pk_airband_am_gain_reason_t reason)
{
    static const char *const names[] = {"reset", "hold", "attack", "fast", "release", "invalid"};
    return (unsigned)reason < sizeof(names) / sizeof(names[0]) ? names[reason] : "invalid";
}
