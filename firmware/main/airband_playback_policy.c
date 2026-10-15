#include "airband_playback_policy.h"

pk_airband_playback_decision_t pk_airband_playback_decide(
    bool user_playing, bool live_active, uint32_t record_queued_blocks,
    bool test_tone_active)
{
    if (!user_playing) return PK_AIRBAND_PLAYBACK_DECISION_PAUSE_USER;
    if (test_tone_active)
        return PK_AIRBAND_PLAYBACK_DECISION_PAUSE_TEST_TONE;
    if (live_active) return PK_AIRBAND_PLAYBACK_DECISION_PAUSE_LIVE;
    if (record_queued_blocks >= PK_AIRBAND_PLAYBACK_RECORD_PRESSURE_BLOCKS)
        return PK_AIRBAND_PLAYBACK_DECISION_PAUSE_RECORD_PRESSURE;
    return PK_AIRBAND_PLAYBACK_DECISION_PLAY;
}

bool pk_airband_playback_result_commits_cursor(
    pk_airband_speaker_playback_result_t result)
{
    return result == PK_AIRBAND_SPEAKER_PLAYBACK_WRITTEN;
}

pk_airband_speaker_playback_result_t pk_airband_speaker_playback_prewrite(
    bool codec_online, bool request_generation_current,
    pk_airband_speaker_playback_result_t cancelled_result,
    bool test_tone_active, bool live_active)
{
    if (!request_generation_current) return cancelled_result;
    if (!codec_online) return PK_AIRBAND_SPEAKER_PLAYBACK_ERROR;
    if (test_tone_active) return PK_AIRBAND_SPEAKER_PLAYBACK_TONE_BUSY;
    if (live_active) return PK_AIRBAND_SPEAKER_PLAYBACK_LIVE_PREEMPTED;
    return PK_AIRBAND_SPEAKER_PLAYBACK_WRITTEN;
}
