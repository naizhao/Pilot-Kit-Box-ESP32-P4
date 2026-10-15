/*
 * cc -std=c11 -Wall -Wextra -Werror -Ifirmware/main \
 *   -o /tmp/test_airband_recording_catalog \
 *   firmware/test/test_airband_recording_catalog.c \
 *   firmware/main/airband_recording_catalog.c && \
 *   /tmp/test_airband_recording_catalog
 */
#include "airband_recording_catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

static void test_complete_and_interrupted_names(void)
{
    pk_airband_recording_name_t got = {0};
    CHECK(pk_airband_recording_name_parse(
              "s0000000042-c121175-f121183333-0000000003.wav", &got),
          "complete project filename must parse");
    CHECK(got.session == 42u, "session must come from the ten digit field");
    CHECK(got.channel_khz == 121175u, "channel must preserve cockpit display value");
    CHECK(got.carrier_hz == 121183333u, "carrier must preserve tuned frequency");
    CHECK(got.volume == 3u, "volume must parse");
    CHECK(!got.interrupted, "normal final file must not be interrupted");

    memset(&got, 0, sizeof(got));
    CHECK(pk_airband_recording_name_parse(
              "s4294967295-c118000-f118000000-4294967295-interrupted.wav", &got),
          "maximum uint32 fields and interrupted suffix must parse");
    CHECK(got.session == UINT32_MAX && got.volume == UINT32_MAX,
          "uint32 boundary must not wrap");
    CHECK(got.interrupted, "interrupted suffix must be observable");
}

static void test_non_final_or_malformed_names_are_rejected(void)
{
    static const char *const bad[] = {
        "s0000000042-c121175-f121183333-0000000003.part.wav",
        "s0000000042-c121175-f121183333-0000000003-interrupted.part.wav",
        "s000000042-c121175-f121183333-0000000003.wav",
        "s0000000042-c12117-f121183333-0000000003.wav",
        "s0000000042-c121175-f12118333-0000000003.wav",
        "s0000000042-c121175-f121183333-000000003.wav",
        "s4294967296-c121175-f121183333-0000000003.wav",
        "s0000000042-c121175-f121183333-0000000003.wav.bak",
        "../s0000000042-c121175-f121183333-0000000003.wav",
        "not-a-recording.wav",
    };
    pk_airband_recording_name_t got;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        memset(&got, 0xA5, sizeof(got));
        CHECK(!pk_airband_recording_name_parse(bad[i], &got), bad[i]);
    }
}

static void test_all_53_entries_survive_newest_first_sort(void)
{
    pk_airband_recording_name_t rows[53];
    char name[96];
    for (unsigned i = 0; i < 53; ++i) {
        unsigned session = (i * 17u) % 53u + 1u;
        snprintf(name, sizeof(name),
                 "s%010u-c121500-f121500000-%010u.wav", session, i + 1u);
        CHECK(pk_airband_recording_name_parse(name, &rows[i]),
              "every generated fixture must parse");
    }
    qsort(rows, 53, sizeof(rows[0]), pk_airband_recording_newest_first);
    CHECK(rows[0].session == 53u, "newest entry must be first");
    CHECK(rows[52].session == 1u, "oldest entry must remain reachable");
    for (size_t i = 1; i < 53; ++i)
        CHECK(rows[i - 1].session > rows[i].session,
              "all entries must be strictly newest first without truncation");

    pk_airband_recording_name_t same[3] = {
        {.session = 7, .volume = 1},
        {.session = 7, .volume = 9},
        {.session = 7, .volume = 2},
    };
    qsort(same, 3, sizeof(same[0]), pk_airband_recording_newest_first);
    CHECK(same[0].volume == 9 && same[1].volume == 2 && same[2].volume == 1,
          "volumes in one session must be newest first");
}

int main(void)
{
    test_complete_and_interrupted_names();
    test_non_final_or_malformed_names_are_rejected();
    test_all_53_entries_survive_newest_first_sort();
    if (failures) return 1;
    puts("airband recording catalog tests PASS");
    return 0;
}
