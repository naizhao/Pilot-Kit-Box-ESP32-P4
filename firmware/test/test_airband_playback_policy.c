/*
 * cc -std=c11 -Wall -Wextra -Werror -Ifirmware/main \
 *   -o /tmp/test_airband_playback_policy \
 *   firmware/test/test_airband_playback_policy.c \
 *   firmware/main/airband_playback_policy.c && \
 *   /tmp/test_airband_playback_policy
 */
#include "airband_playback_policy.h"

#include <stdio.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

static void test_recording_pressure_boundary_is_127_128(void)
{
    CHECK(pk_airband_playback_decide(true, false, 127u, false) ==
              PK_AIRBAND_PLAYBACK_DECISION_PLAY,
          "127 queued blocks must still allow playback");
    CHECK(pk_airband_playback_decide(true, false, 128u, false) ==
              PK_AIRBAND_PLAYBACK_DECISION_PAUSE_RECORD_PRESSURE,
          "128 queued blocks must pause playback");
    CHECK(pk_airband_playback_decide(true, false, 511u, false) ==
              PK_AIRBAND_PLAYBACK_DECISION_PAUSE_RECORD_PRESSURE,
          "larger current backlog must remain paused");
}

static void test_live_and_tone_preempt_only_active_user_playback(void)
{
    CHECK(pk_airband_playback_decide(false, true, 500u, true) ==
              PK_AIRBAND_PLAYBACK_DECISION_PAUSE_USER,
          "an already user-paused player keeps the user reason");
    CHECK(pk_airband_playback_decide(true, true, 0u, false) ==
              PK_AIRBAND_PLAYBACK_DECISION_PAUSE_LIVE,
          "live aviation audio must preempt history");
    CHECK(pk_airband_playback_decide(true, true, 500u, true) ==
              PK_AIRBAND_PLAYBACK_DECISION_PAUSE_TEST_TONE,
          "diagnostic tone has highest active-source priority");
}

static void test_only_confirmed_i2s_write_advances_cursor(void)
{
    CHECK(pk_airband_playback_result_commits_cursor(
              PK_AIRBAND_SPEAKER_PLAYBACK_WRITTEN),
          "confirmed write must advance playback cursor");
    CHECK(!pk_airband_playback_result_commits_cursor(
              PK_AIRBAND_SPEAKER_PLAYBACK_LIVE_PREEMPTED),
          "live-preempted block must be replayed later");
    CHECK(!pk_airband_playback_result_commits_cursor(
              PK_AIRBAND_SPEAKER_PLAYBACK_TONE_BUSY),
          "tone-busy block must not advance");
    CHECK(!pk_airband_playback_result_commits_cursor(
              PK_AIRBAND_SPEAKER_PLAYBACK_STOPPED),
          "stopped block must not advance");
    CHECK(!pk_airband_playback_result_commits_cursor(
              PK_AIRBAND_SPEAKER_PLAYBACK_ERROR),
          "failed write must not advance");
}

static void test_speaker_rechecks_priority_immediately_before_write(void)
{
    CHECK(pk_airband_speaker_playback_prewrite(
              true, true, PK_AIRBAND_SPEAKER_PLAYBACK_STOPPED, false, false) ==
              PK_AIRBAND_SPEAKER_PLAYBACK_WRITTEN,
          "current request may proceed when codec is online and no source preempts");
    CHECK(pk_airband_speaker_playback_prewrite(
              true, true, PK_AIRBAND_SPEAKER_PLAYBACK_STOPPED, false, true) ==
              PK_AIRBAND_SPEAKER_PLAYBACK_LIVE_PREEMPTED,
          "live opening after enqueue must reject the queued history block");
    CHECK(pk_airband_speaker_playback_prewrite(
              true, true, PK_AIRBAND_SPEAKER_PLAYBACK_STOPPED, true, false) ==
              PK_AIRBAND_SPEAKER_PLAYBACK_TONE_BUSY,
          "tone becoming active after enqueue must reject history");
    CHECK(pk_airband_speaker_playback_prewrite(
              true, false, PK_AIRBAND_SPEAKER_PLAYBACK_LIVE_PREEMPTED,
              false, true) == PK_AIRBAND_SPEAKER_PLAYBACK_LIVE_PREEMPTED,
          "generation-cancelled request must preserve its live-preempt reason");
    CHECK(pk_airband_speaker_playback_prewrite(
              false, true, PK_AIRBAND_SPEAKER_PLAYBACK_STOPPED, false, false) ==
              PK_AIRBAND_SPEAKER_PLAYBACK_ERROR,
          "offline codec cannot accept history audio");
}

int main(void)
{
    test_recording_pressure_boundary_is_127_128();
    test_live_and_tone_preempt_only_active_user_playback();
    test_only_confirmed_i2s_write_advances_cursor();
    test_speaker_rechecks_priority_immediately_before_write();
    if (failures) return 1;
    puts("airband playback policy tests PASS");
    return 0;
}
