#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PK_AIRBAND_PLAYBACK_RECORD_PRESSURE_BLOCKS 128u

typedef enum {
    PK_AIRBAND_PLAYBACK_DECISION_PLAY = 0,
    PK_AIRBAND_PLAYBACK_DECISION_PAUSE_USER,
    PK_AIRBAND_PLAYBACK_DECISION_PAUSE_LIVE,
    PK_AIRBAND_PLAYBACK_DECISION_PAUSE_RECORD_PRESSURE,
    PK_AIRBAND_PLAYBACK_DECISION_PAUSE_TEST_TONE,
} pk_airband_playback_decision_t;

typedef enum {
    PK_AIRBAND_SPEAKER_PLAYBACK_WRITTEN = 0,
    PK_AIRBAND_SPEAKER_PLAYBACK_LIVE_PREEMPTED,
    PK_AIRBAND_SPEAKER_PLAYBACK_TONE_BUSY,
    PK_AIRBAND_SPEAKER_PLAYBACK_STOPPED,
    PK_AIRBAND_SPEAKER_PLAYBACK_ERROR,
} pk_airband_speaker_playback_result_t;

pk_airband_playback_decision_t pk_airband_playback_decide(
    bool user_playing, bool live_active, uint32_t record_queued_blocks,
    bool test_tone_active);

bool pk_airband_playback_result_commits_cursor(
    pk_airband_speaker_playback_result_t result);

/* Speaker task calls this after dequeue and immediately before I2S write. */
pk_airband_speaker_playback_result_t pk_airband_speaker_playback_prewrite(
    bool codec_online, bool request_generation_current,
    pk_airband_speaker_playback_result_t cancelled_result,
    bool test_tone_active, bool live_active);
