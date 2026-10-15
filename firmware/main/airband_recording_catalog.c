#include "airband_recording_catalog.h"

#include <stddef.h>
#include <string.h>

static bool parse_u32_fixed(const char *text, size_t digits, uint32_t *out)
{
    uint64_t value = 0;
    for (size_t i = 0; i < digits; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10u + (uint32_t)(text[i] - '0');
        if (value > UINT32_MAX) return false;
    }
    *out = (uint32_t)value;
    return true;
}

bool pk_airband_recording_name_parse(const char *name,
                                     pk_airband_recording_name_t *out)
{
    if (!name || !out) return false;
    const size_t length = strlen(name);
    const bool normal = length == 45u && strcmp(name + 41, ".wav") == 0;
    const bool interrupted = length == 57u &&
                             strcmp(name + 41, "-interrupted.wav") == 0;
    if (!normal && !interrupted) return false;
    if (name[0] != 's' || name[11] != '-' || name[12] != 'c' ||
        name[19] != '-' || name[20] != 'f' || name[30] != '-') return false;

    pk_airband_recording_name_t parsed = {.interrupted = interrupted};
    if (!parse_u32_fixed(name + 1, 10, &parsed.session) ||
        !parse_u32_fixed(name + 13, 6, &parsed.channel_khz) ||
        !parse_u32_fixed(name + 21, 9, &parsed.carrier_hz) ||
        !parse_u32_fixed(name + 31, 10, &parsed.volume)) return false;
    *out = parsed;
    return true;
}

int pk_airband_recording_newest_first(const void *lhs, const void *rhs)
{
    const pk_airband_recording_name_t *a = lhs;
    const pk_airband_recording_name_t *b = rhs;
    if (a->session != b->session) return a->session < b->session ? 1 : -1;
    if (a->volume != b->volume) return a->volume < b->volume ? 1 : -1;
    return 0;
}
