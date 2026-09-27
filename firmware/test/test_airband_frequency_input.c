/* 航空频率计算器式输入的纯状态模型：
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 *      -o /tmp/test_airband_frequency_input \
 *      firmware/test/test_airband_frequency_input.c \
 *      firmware/main/airband_frequency_input.c \
 *      && /tmp/test_airband_frequency_input
 */
#include "airband_frequency_input.h"

#include <stdio.h>
#include <string.h>

static int fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); fail++; } } while (0)

static void press(pk_airband_frequency_input_t *s, char key,
                  const char *expected)
{
    CHECK(pk_airband_frequency_input_key_enabled(s, key),
          "key '%c' unexpectedly disabled for '%s'\n", key, s->text);
    CHECK(pk_airband_frequency_input_apply(s, key),
          "key '%c' was not applied to '%s'\n", key, s->text);
    CHECK(strcmp(s->text, expected) == 0,
          "key '%c' produced '%s', expected '%s'\n", key, s->text, expected);
}

static void test_first_valid_digit_replaces_prefill_and_formats_live(void)
{
    pk_airband_frequency_input_t s;
    pk_airband_frequency_input_init(&s, "121.500");
    CHECK(strcmp(s.text, "121.500") == 0 && s.replace_armed,
          "current frequency was not preserved as replaceable prefill\n");
    press(&s, '1', "1");
    press(&s, '2', "12");
    press(&s, '0', "120");
    press(&s, '4', "120.4");
}

static void test_six_digits_display_and_commit_as_three_decimals(void)
{
    pk_airband_frequency_input_t s;
    char normalized[8] = {0};
    uint32_t hz = 0;
    pk_airband_frequency_input_init(&s, "123.450");
    const char sequence[] = "121175";
    const char *expected[] = {"1", "12", "121", "121.1", "121.17", "121.175"};
    for (size_t i = 0; i < sizeof(sequence) - 1; ++i)
        press(&s, sequence[i], expected[i]);
    CHECK(pk_airband_frequency_input_commit(&s, normalized, &hz),
          "121.175 was not committable\n");
    CHECK(strcmp(normalized, "121.175") == 0 && hz == 121175000u,
          "121.175 committed as '%s'/%lu\n", normalized, (unsigned long)hz);
}

static void test_integer_commit_pads_fraction_and_prefill_can_be_confirmed(void)
{
    pk_airband_frequency_input_t s;
    char normalized[8] = {0};
    uint32_t hz = 0;
    pk_airband_frequency_input_init(&s, "121.500");
    CHECK(pk_airband_frequency_input_key_enabled(&s, PK_AIRBAND_FREQ_KEY_OK),
          "valid prefill cannot be confirmed\n");
    CHECK(pk_airband_frequency_input_commit(&s, normalized, &hz) &&
          strcmp(normalized, "121.500") == 0 && hz == 121500000u,
          "prefill commit changed current frequency\n");

    press(&s, '1', "1");
    press(&s, '2', "12");
    press(&s, '8', "128");
    CHECK(pk_airband_frequency_input_key_enabled(&s, PK_AIRBAND_FREQ_KEY_OK),
          "complete integer frequency left OK disabled\n");
    CHECK(pk_airband_frequency_input_commit(&s, normalized, &hz) &&
          strcmp(normalized, "128.000") == 0 && hz == 128000000u,
          "128 committed as '%s'/%lu\n", normalized, (unsigned long)hz);
}

static void test_impossible_prefix_keys_are_disabled_and_cannot_mutate(void)
{
    pk_airband_frequency_input_t s;
    pk_airband_frequency_input_init(&s, "121.500");
    CHECK(!pk_airband_frequency_input_key_enabled(&s, '5'),
          "invalid first digit 5 was enabled\n");
    CHECK(!pk_airband_frequency_input_apply(&s, '5') &&
          strcmp(s.text, "121.500") == 0 && s.replace_armed,
          "invalid first digit destroyed the prefill\n");

    press(&s, '1', "1");
    CHECK(!pk_airband_frequency_input_key_enabled(&s, '5'),
          "15x prefix remained possible\n");
    CHECK(!pk_airband_frequency_input_apply(&s, '5') && strcmp(s.text, "1") == 0,
          "disabled 5 mutated prefix 1\n");
    press(&s, '1', "11");
    CHECK(!pk_airband_frequency_input_key_enabled(&s, '7') &&
          pk_airband_frequency_input_key_enabled(&s, '8') &&
          pk_airband_frequency_input_key_enabled(&s, '9'),
          "11x did not constrain the third digit to 8/9\n");

    pk_airband_frequency_input_init(&s, "121.500");
    press(&s, '1', "1"); press(&s, '3', "13");
    CHECK(!pk_airband_frequency_input_key_enabled(&s, '8') &&
          pk_airband_frequency_input_key_enabled(&s, '7'),
          "13x did not reject values above 137\n");
}

static void test_upper_bound_only_allows_zero_fraction_and_ok_needs_three_digits(void)
{
    pk_airband_frequency_input_t s;
    pk_airband_frequency_input_init(&s, "121.500");
    press(&s, '1', "1");
    CHECK(!pk_airband_frequency_input_key_enabled(&s, PK_AIRBAND_FREQ_KEY_OK),
          "one-digit prefix enabled OK\n");
    press(&s, '3', "13");
    CHECK(!pk_airband_frequency_input_key_enabled(&s, PK_AIRBAND_FREQ_KEY_OK),
          "two-digit prefix enabled OK\n");
    press(&s, '7', "137");
    CHECK(pk_airband_frequency_input_key_enabled(&s, '0') &&
          !pk_airband_frequency_input_key_enabled(&s, '1'),
          "137 allowed a non-zero fractional digit\n");
    press(&s, '0', "137.0");
    CHECK(!pk_airband_frequency_input_key_enabled(&s, '9'),
          "137.0 allowed a non-zero continuation\n");
}

static void test_dot_and_delete_follow_calculator_state(void)
{
    pk_airband_frequency_input_t s;
    pk_airband_frequency_input_init(&s, "121.500");
    CHECK(!pk_airband_frequency_input_key_enabled(&s, '.'),
          "dot enabled before replacement input\n");
    press(&s, '1', "1"); press(&s, '2', "12"); press(&s, '0', "120");
    CHECK(pk_airband_frequency_input_key_enabled(&s, '.'),
          "dot disabled after three integer digits\n");
    press(&s, '.', "120.");
    press(&s, '4', "120.4");
    press(&s, PK_AIRBAND_FREQ_KEY_DELETE, "120");
}

static void test_non_channel_suffix_is_disabled_before_it_can_be_entered(void)
{
    pk_airband_frequency_input_t s;
    pk_airband_frequency_input_init(&s, "121.500");
    const char sequence[] = "11889";
    const char *expected[] = {"1", "11", "118", "118.8", "118.89"};
    for (size_t i = 0; i < sizeof(sequence) - 1; ++i)
        press(&s, sequence[i], expected[i]);

    CHECK(!pk_airband_frequency_input_key_enabled(&s, '6'),
          "118.896 final digit was not disabled\n");
    CHECK(!pk_airband_frequency_input_apply(&s, '6') &&
          strcmp(s.text, "118.89") == 0,
          "disabled 118.896 final digit mutated the input\n");
    CHECK(pk_airband_frequency_input_key_enabled(&s, '0'),
          "valid ICAO channel 118.890 was disabled\n");
}

static void test_icao_channel_designator_maps_to_actual_carrier(void)
{
    static const struct {
        const char *text;
        uint32_t carrier_hz;
    } cases[] = {
        {"118.000", 118000000u},
        {"118.005", 118000000u},
        {"118.010", 118008330u},
        {"118.015", 118016670u},
        {"118.025", 118025000u},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        pk_airband_frequency_input_t s;
        char normalized[8] = {0};
        uint32_t hz = 0;
        pk_airband_frequency_input_init(&s, cases[i].text);
        CHECK(s.replace_armed,
              "valid channel %s was rejected during init\n", cases[i].text);
        CHECK(pk_airband_frequency_input_commit(&s, normalized, &hz) &&
              strcmp(normalized, cases[i].text) == 0 &&
              hz == cases[i].carrier_hz,
              "channel %s committed as %s/%lu, expected carrier %lu\n",
              cases[i].text, normalized, (unsigned long)hz,
              (unsigned long)cases[i].carrier_hz);
    }
}

static void test_channel_text_parser_preserves_designator_and_carrier(void)
{
    uint32_t channel_khz = 0;
    uint32_t carrier_hz = 0;
    CHECK(pk_airband_channel_text_parse("118.010", &channel_khz, &carrier_hz) &&
          channel_khz == 118010u && carrier_hz == 118008330u,
          "channel text parser lost designator/carrier identity\n");
    CHECK(!pk_airband_channel_text_parse("118.896", &channel_khz, &carrier_hz),
          "channel text parser accepted 118.896\n");
}

static void test_omitted_icao_designators_are_rejected(void)
{
    static const char *invalid[] = {"118.020", "118.045", "118.070",
                                    "118.095", "118.896"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        pk_airband_frequency_input_t s;
        char normalized[8] = {0};
        uint32_t hz = 0;
        pk_airband_frequency_input_init(&s, invalid[i]);
        CHECK(!s.replace_armed && s.text[0] == '\0',
              "invalid channel %s was accepted during init\n", invalid[i]);
        CHECK(!pk_airband_frequency_input_commit(&s, normalized, &hz),
              "invalid channel %s was committable\n", invalid[i]);
    }
}

int main(void)
{
    test_first_valid_digit_replaces_prefill_and_formats_live();
    test_six_digits_display_and_commit_as_three_decimals();
    test_integer_commit_pads_fraction_and_prefill_can_be_confirmed();
    test_impossible_prefix_keys_are_disabled_and_cannot_mutate();
    test_upper_bound_only_allows_zero_fraction_and_ok_needs_three_digits();
    test_dot_and_delete_follow_calculator_state();
    test_non_channel_suffix_is_disabled_before_it_can_be_entered();
    test_icao_channel_designator_maps_to_actual_carrier();
    test_channel_text_parser_preserves_designator_and_carrier();
    test_omitted_icao_designators_are_rejected();
    puts(fail ? "FAILED" : "OK");
    return fail ? 1 : 0;
}
