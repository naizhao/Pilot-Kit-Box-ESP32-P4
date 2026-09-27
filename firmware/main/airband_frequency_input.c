#include "airband_frequency_input.h"

#include <stddef.h>
#include <string.h>

bool pk_airband_channel_to_carrier_hz(uint32_t channel_khz,
                                      uint32_t *carrier_hz)
{
    if (carrier_hz == NULL || channel_khz < 118000u ||
        channel_khz > 137000u || channel_khz % 5u != 0u)
        return false;

    const uint32_t remainder = channel_khz % 25u;
    const uint32_t base_hz = (channel_khz - remainder) * 1000u;
    switch (remainder) {
    case 0u:  *carrier_hz = base_hz;          return true;
    case 5u:  *carrier_hz = base_hz;          return true;
    case 10u: *carrier_hz = base_hz + 8330u;  return true;
    case 15u: *carrier_hz = base_hz + 16670u; return true;
    default: return false; /* 020/045/070/095 不是 ICAO 频道标识。 */
    }
}

static bool decode_text(const char *text, char digits[7], size_t *digit_count,
                        bool *has_dot)
{
    if (text == NULL || digits == NULL || digit_count == NULL ||
        has_dot == NULL) return false;
    size_t count = 0;
    bool dot = false;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        const char c = text[i];
        if (c >= '0' && c <= '9') {
            if (count >= 6) return false;
            digits[count++] = c;
        } else if (c == '.' && !dot && count == 3) {
            dot = true;
        } else {
            return false;
        }
    }
    digits[count] = '\0';
    *digit_count = count;
    *has_dot = dot;
    return true;
}

static bool viable_digits(const char digits[7], size_t count)
{
    if (count == 0 || count > 6 || digits[0] != '1') return false;
    if (count == 1) return true;
    if (digits[1] < '1' || digits[1] > '3') return false;
    if (count == 2) return true;

    const unsigned mhz = (unsigned)(digits[0] - '0') * 100u +
                         (unsigned)(digits[1] - '0') * 10u +
                         (unsigned)(digits[2] - '0');
    if (mhz < 118u || mhz > 137u) return false;
    if (mhz == 137u) {
        for (size_t i = 3; i < count; ++i)
            if (digits[i] != '0') return false;
    }
    if (count == 6) {
        const uint32_t khz = mhz * 1000u +
            (uint32_t)(digits[3] - '0') * 100u +
            (uint32_t)(digits[4] - '0') * 10u +
            (uint32_t)(digits[5] - '0');
        uint32_t carrier_hz = 0;
        return pk_airband_channel_to_carrier_hz(khz, &carrier_hz);
    }
    return true;
}

static void format_digits(const char digits[7], size_t count, bool trailing_dot,
                          char out[PK_AIRBAND_FREQ_INPUT_TEXT_MAX + 1])
{
    size_t pos = 0;
    for (size_t i = 0; i < count; ++i) {
        if (i == 3) out[pos++] = '.';
        out[pos++] = digits[i];
    }
    if (trailing_dot && count == 3) out[pos++] = '.';
    out[pos] = '\0';
}

static bool normalize_text(const char *text,
                           char normalized[PK_AIRBAND_FREQ_INPUT_TEXT_MAX + 1],
                           uint32_t *out_hz)
{
    char digits[7];
    size_t count = 0;
    bool has_dot = false;
    if (!decode_text(text, digits, &count, &has_dot) || count < 3 ||
        !viable_digits(digits, count)) return false;

    const uint32_t mhz = (uint32_t)(digits[0] - '0') * 100u +
                         (uint32_t)(digits[1] - '0') * 10u +
                         (uint32_t)(digits[2] - '0');
    uint32_t khz = 0;
    size_t fraction = count - 3;
    for (size_t i = 3; i < count; ++i)
        khz = khz * 10u + (uint32_t)(digits[i] - '0');
    while (fraction++ < 3) khz *= 10u;

    if (normalized != NULL) {
        normalized[0] = digits[0];
        normalized[1] = digits[1];
        normalized[2] = digits[2];
        normalized[3] = '.';
        normalized[4] = count > 3 ? digits[3] : '0';
        normalized[5] = count > 4 ? digits[4] : '0';
        normalized[6] = count > 5 ? digits[5] : '0';
        normalized[7] = '\0';
    }
    uint32_t carrier_hz = 0;
    if (!pk_airband_channel_to_carrier_hz(mhz * 1000u + khz, &carrier_hz))
        return false;
    if (out_hz != NULL) *out_hz = carrier_hz;
    return true;
}

bool pk_airband_channel_text_parse(const char *text, uint32_t *channel_khz,
                                   uint32_t *carrier_hz)
{
    if (channel_khz == NULL || carrier_hz == NULL) return false;
    char normalized[PK_AIRBAND_FREQ_INPUT_TEXT_MAX + 1];
    uint32_t hz = 0;
    if (!normalize_text(text, normalized, &hz)) return false;
    *channel_khz = (uint32_t)(normalized[0] - '0') * 100000u +
                   (uint32_t)(normalized[1] - '0') * 10000u +
                   (uint32_t)(normalized[2] - '0') * 1000u +
                   (uint32_t)(normalized[4] - '0') * 100u +
                   (uint32_t)(normalized[5] - '0') * 10u +
                   (uint32_t)(normalized[6] - '0');
    *carrier_hz = hz;
    return true;
}

void pk_airband_frequency_input_init(pk_airband_frequency_input_t *state,
                                     const char *initial)
{
    if (state == NULL) return;
    state->text[0] = '\0';
    state->replace_armed = false;
    char normalized[PK_AIRBAND_FREQ_INPUT_TEXT_MAX + 1];
    if (normalize_text(initial, normalized, NULL)) {
        memcpy(state->text, normalized, sizeof(state->text));
        state->replace_armed = true;
    }
}

bool pk_airband_frequency_input_commit(
    const pk_airband_frequency_input_t *state,
    char normalized[PK_AIRBAND_FREQ_INPUT_TEXT_MAX + 1], uint32_t *out_hz)
{
    if (state == NULL || normalized == NULL || out_hz == NULL) return false;
    return normalize_text(state->text, normalized, out_hz);
}

bool pk_airband_frequency_input_key_enabled(
    const pk_airband_frequency_input_t *state, char key)
{
    if (state == NULL) return false;
    if (key == PK_AIRBAND_FREQ_KEY_OK) {
        char normalized[PK_AIRBAND_FREQ_INPUT_TEXT_MAX + 1];
        uint32_t hz;
        return pk_airband_frequency_input_commit(state, normalized, &hz);
    }
    if (key == PK_AIRBAND_FREQ_KEY_DELETE)
        return !state->replace_armed && state->text[0] != '\0';

    char digits[7];
    size_t count = 0;
    bool has_dot = false;
    if (!decode_text(state->text, digits, &count, &has_dot)) return false;
    if (key == '.')
        return !state->replace_armed && count == 3 && !has_dot;
    if (key < '0' || key > '9') return false;

    if (state->replace_armed) {
        digits[0] = key;
        digits[1] = '\0';
        count = 1;
    } else {
        if (count >= 6) return false;
        digits[count++] = key;
        digits[count] = '\0';
    }
    return viable_digits(digits, count);
}

bool pk_airband_frequency_input_apply(pk_airband_frequency_input_t *state,
                                      char key)
{
    if (!pk_airband_frequency_input_key_enabled(state, key)) return false;
    if (key == PK_AIRBAND_FREQ_KEY_DELETE) {
        char digits[7];
        size_t count = 0;
        bool has_dot = false;
        if (!decode_text(state->text, digits, &count, &has_dot)) return false;
        if (has_dot && count == 3) {
            format_digits(digits, count, false, state->text);
        } else if (count > 0) {
            --count;
            digits[count] = '\0';
            format_digits(digits, count, false, state->text);
        }
        return true;
    }
    if (key == '.') {
        const size_t len = strlen(state->text);
        state->text[len] = '.';
        state->text[len + 1] = '\0';
        return true;
    }

    char digits[7];
    size_t count = 0;
    bool has_dot = false;
    if (state->replace_armed) {
        digits[0] = key;
        digits[1] = '\0';
        count = 1;
        state->replace_armed = false;
    } else {
        if (!decode_text(state->text, digits, &count, &has_dot)) return false;
        digits[count++] = key;
        digits[count] = '\0';
    }
    format_digits(digits, count, false, state->text);
    return true;
}
