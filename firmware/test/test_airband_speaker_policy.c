/* AIRBAND 喇叭队列/测试音纯策略：
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 *      -o /tmp/test_airband_speaker_policy \
 *      firmware/test/test_airband_speaker_policy.c \
 *      firmware/main/airband_speaker_policy.c -lm && /tmp/test_airband_speaker_policy
 */
#include "airband_speaker_policy.h"

#include <math.h>
#include <stdio.h>

static int fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); fail++; } } while (0)

static void test_queue_policy_never_blocks_capture(void)
{
    CHECK(pk_airband_speaker_queue_action(0, 3, true) ==
              PK_AIRBAND_SPEAKER_QUEUE_ENQUEUE,
          "empty queue should enqueue\n");
    CHECK(pk_airband_speaker_queue_action(2, 3, true) ==
              PK_AIRBAND_SPEAKER_QUEUE_ENQUEUE,
          "part-filled queue should enqueue\n");
    CHECK(pk_airband_speaker_queue_action(3, 3, true) ==
              PK_AIRBAND_SPEAKER_QUEUE_DROP_OLDEST_ENQUEUE,
          "full queue should drop oldest then enqueue\n");
    CHECK(pk_airband_speaker_queue_action(9, 3, true) ==
              PK_AIRBAND_SPEAKER_QUEUE_DROP_OLDEST_ENQUEUE,
          "overfull observation should still recover without blocking\n");
    CHECK(pk_airband_speaker_queue_action(0, 0, true) ==
              PK_AIRBAND_SPEAKER_QUEUE_REJECT,
          "zero-capacity queue accepted audio\n");
}

static void test_closed_gate_flushes_stale_audio(void)
{
    CHECK(pk_airband_speaker_queue_action(0, 3, false) ==
              PK_AIRBAND_SPEAKER_QUEUE_FLUSH,
          "muted empty queue should still establish flush state\n");
    CHECK(pk_airband_speaker_queue_action(3, 3, false) ==
              PK_AIRBAND_SPEAKER_QUEUE_FLUSH,
          "muted full queue would leak stale audio\n");
}

static void test_capture_failure_closes_a_previously_open_program_gate(void)
{
    CHECK(pk_airband_speaker_capture_continuous(true, true, 192),
          "valid rendered capture was classified discontinuous\n");
    CHECK(!pk_airband_speaker_capture_continuous(false, false, 0) &&
          !pk_airband_speaker_capture_continuous(true, false, 0) &&
          !pk_airband_speaker_capture_continuous(true, true, 0),
          "timeout/bad/zero capture was classified continuous\n");
    bool gate = pk_airband_speaker_capture_playable(true, true, 192, true);
    CHECK(gate, "valid playable PCM did not open program gate\n");
    gate = pk_airband_speaker_capture_playable(false, false, 0, gate);
    CHECK(!gate, "I2S timeout retained the previous open program gate\n");
    gate = pk_airband_speaker_capture_playable(true, false, 0, true);
    CHECK(!gate, "malformed PCM retained the previous open program gate\n");
    gate = pk_airband_speaker_capture_playable(true, true, 0, true);
    CHECK(!gate, "zero rendered frames retained the previous open program gate\n");
}

static void test_local_tone_is_bounded_to_one_second(void)
{
    CHECK(pk_airband_speaker_tone_frames(0, 48000) == 0,
          "zero-duration tone produced frames\n");
    CHECK(pk_airband_speaker_tone_frames(250, 48000) == 12000,
          "250 ms tone frame count wrong\n");
    CHECK(pk_airband_speaker_tone_frames(1000, 48000) == 48000,
          "one-second tone frame count wrong\n");
    CHECK(pk_airband_speaker_tone_frames(5000, 48000) == 48000,
          "tone exceeded one-second safety cap\n");
    CHECK(pk_airband_speaker_tone_frames(1000, 0) == 0,
          "zero sample rate accepted\n");
}

static void test_repeated_request_does_not_extend_active_tone(void)
{
    CHECK(pk_airband_speaker_tone_begin(0, 48000) == 48000,
          "first request did not start one second\n");
    CHECK(pk_airband_speaker_tone_begin(24000, 48000) == 24000,
          "second request extended an active tone\n");
}

static void test_second_click_stops_instead_of_rejecting_active_test(void)
{
    CHECK(pk_airband_speaker_test_click(false, false) ==
              PK_AIRBAND_SPEAKER_TEST_CLICK_REJECT,
          "offline click was accepted\n");
    CHECK(pk_airband_speaker_test_click(true, false) ==
              PK_AIRBAND_SPEAKER_TEST_CLICK_START,
          "idle online click did not start\n");
    CHECK(pk_airband_speaker_test_click(true, true) ==
              PK_AIRBAND_SPEAKER_TEST_CLICK_STOP,
          "active test click did not request stop\n");
}

static void test_cancel_before_start_consumption_reports_zero_frames(void)
{
    CHECK(pk_airband_speaker_cancel_written_frames(true, 48000) == 0,
          "pending start cancellation reused the previous test frame count\n");
    CHECK(pk_airband_speaker_cancel_written_frames(false, 12345) == 12345,
          "active cancellation lost the frames already written\n");
}

static void test_tone_result_requires_every_frame_and_no_error(void)
{
    CHECK(pk_airband_speaker_tone_result(true, 48000, 48000, 0) ==
              PK_AIRBAND_SPEAKER_TEST_PASS,
          "complete successful tone was not PASS\n");
    CHECK(pk_airband_speaker_tone_result(false, 48000, 0, 0) ==
              PK_AIRBAND_SPEAKER_TEST_FAIL,
          "codec setup failure was reported successful\n");
    CHECK(pk_airband_speaker_tone_result(true, 48000, 47808, 0) ==
              PK_AIRBAND_SPEAKER_TEST_FAIL,
          "short I2S write was reported successful\n");
    CHECK(pk_airband_speaker_tone_result(true, 48000, 48000, 1) ==
              PK_AIRBAND_SPEAKER_TEST_FAIL,
          "write error was reported successful\n");
}

static void test_pa_is_only_enabled_for_audible_online_output(void)
{
    CHECK(!pk_airband_speaker_pa_enabled(false, false, true, false),
          "offline codec enabled PA\n");
    CHECK(!pk_airband_speaker_pa_enabled(true, true, true, false),
          "muted receiver enabled PA\n");
    CHECK(!pk_airband_speaker_pa_enabled(true, false, false, false),
          "closed squelch/program gate left PA enabled\n");
    CHECK(pk_airband_speaker_pa_enabled(true, false, true, false),
          "playable online receiver left PA disabled\n");
    CHECK(pk_airband_speaker_pa_enabled(true, true, false, true),
          "local test tone did not override mute/gate temporarily\n");
}

static void test_history_playback_has_independent_mute_and_volume(void)
{
    /* 实时 AIRBAND 静音不能泄漏到历史回放；两个来源也不能偷用对方音量。 */
    CHECK(pk_airband_speaker_source_audible(
              true, true, false, true, false, false),
          "live mute incorrectly silenced history playback\n");
    CHECK(!pk_airband_speaker_source_audible(
              true, false, false, true, true, false),
          "history playback mute did not silence history playback\n");
    CHECK(pk_airband_speaker_source_audible(
              true, true, true, false, true, true),
          "diagnostic tone did not remain an explicit override\n");
    CHECK(pk_airband_speaker_source_volume(20, 65, false) == 20,
          "live output did not retain live volume\n");
    CHECK(pk_airband_speaker_source_volume(20, 65, true) == 65,
          "history playback did not use its own volume\n");
}

static void test_volume_percent_is_amplitude_not_linear_db(void)
{
    CHECK(fabsf(pk_airband_speaker_volume_db(0) - (-49.5f)) < 0.01f,
          "zero volume must use codec floor\n");
    CHECK(fabsf(pk_airband_speaker_volume_db(1) - (-41.0f)) < 0.05f,
          "1%% did not include the codec compensation-safe ceiling\n");
    CHECK(fabsf(pk_airband_speaker_volume_db(10) - (-21.0f)) < 0.05f,
          "10%% did not include the codec compensation-safe ceiling\n");
    CHECK(fabsf(pk_airband_speaker_volume_db(35) - (-10.12f)) < 0.05f,
          "35%% was mapped too quietly\n");
    CHECK(fabsf(pk_airband_speaker_volume_db(50) - (-7.02f)) < 0.05f,
          "50%% amplitude conversion wrong\n");
    CHECK(fabsf(pk_airband_speaker_volume_db(100) - (-1.0f)) < 0.01f,
          "100%% should leave headroom for codec hardware-gain compensation\n");
    CHECK(fabsf(pk_airband_speaker_volume_db(255) - (-1.0f)) < 0.01f,
          "out-of-range volume was not clamped\n");
    CHECK(fabsf(pk_airband_speaker_test_volume_db(-49.5f) - (-1.0f)) < 0.01f,
          "diagnostic tone remained inaudible at low user volume\n");
    CHECK(fabsf(pk_airband_speaker_test_volume_db(-9.0f) - (-1.0f)) < 0.01f,
          "diagnostic tone did not apply its minimum audible gain\n");
    CHECK(fabsf(pk_airband_speaker_test_volume_db(-3.0f) - (-1.0f)) < 0.01f,
          "diagnostic tone is not using the compensation-safe codec request\n");
    CHECK(PK_AIRBAND_SPEAKER_TEST_PEAK == 23170,
          "diagnostic PCM peak is not the reviewed -3 dBFS level\n");
}


static void test_entire_tone_table_is_a_minus_3_dbfs_sine(void)
{
    int64_t sum_sq = 0;
    int peak = 0;
    for (uint32_t i = 0; i < 48; ++i) {
        const int sample = pk_airband_speaker_tone_sample(i);
        const int opposite = pk_airband_speaker_tone_sample(i + 24u);
        CHECK(opposite == -sample,
              "tone lost half-wave symmetry at phase %u\n", (unsigned)i);
        const int magnitude = sample < 0 ? -sample : sample;
        if (magnitude > peak) peak = magnitude;
        sum_sq += (int64_t)sample * sample;
    }
    CHECK(pk_airband_speaker_tone_sample(6) >= 16380 &&
          pk_airband_speaker_tone_sample(6) <= 16388,
          "45-degree sample was not scaled with the new peak\n");
    CHECK(peak == PK_AIRBAND_SPEAKER_TEST_PEAK,
          "tone table peak=%d, expected %d\n", peak,
          PK_AIRBAND_SPEAKER_TEST_PEAK);
    const float rms = sqrtf((float)sum_sq / 48.0f);
    CHECK(rms >= 16370.0f && rms <= 16400.0f,
          "tone RMS %.1f is not a -3 dBFS peak sine\n", rms);
}

int main(void)
{
    test_queue_policy_never_blocks_capture();
    test_closed_gate_flushes_stale_audio();
    test_capture_failure_closes_a_previously_open_program_gate();
    test_local_tone_is_bounded_to_one_second();
    test_repeated_request_does_not_extend_active_tone();
    test_second_click_stops_instead_of_rejecting_active_test();
    test_cancel_before_start_consumption_reports_zero_frames();
    test_tone_result_requires_every_frame_and_no_error();
    test_pa_is_only_enabled_for_audible_online_output();
    test_history_playback_has_independent_mute_and_volume();
    test_volume_percent_is_amplitude_not_linear_db();
    test_entire_tone_table_is_a_minus_3_dbfs_sine();
    puts(fail ? "FAILED" : "OK");
    return fail ? 1 : 0;
}
