#include "airband_speaker_policy.h"

#include <math.h>

pk_airband_speaker_test_click_t pk_airband_speaker_test_click(
    bool codec_online, bool tone_active)
{
    if (!codec_online) return PK_AIRBAND_SPEAKER_TEST_CLICK_REJECT;
    return tone_active ? PK_AIRBAND_SPEAKER_TEST_CLICK_STOP
                       : PK_AIRBAND_SPEAKER_TEST_CLICK_START;
}

uint32_t pk_airband_speaker_cancel_written_frames(bool start_pending,
                                                   uint32_t active_written)
{
    return start_pending ? 0u : active_written;
}

int16_t pk_airband_speaker_tone_sample(uint32_t phase)
{
    static const int16_t tone[48] = {
             0,   3024,   5997,   8867,  11585,  14105,  16384,  18382,
         20066,  21406,  22381,  22972,  23170,  22972,  22381,  21406,
         20066,  18382,  16384,  14105,  11585,   8867,   5997,   3024,
             0,  -3024,  -5997,  -8867, -11585, -14105, -16384, -18382,
        -20066, -21406, -22381, -22972, -23170, -22972, -22381, -21406,
        -20066, -18382, -16384, -14105, -11585,  -8867,  -5997,  -3024,
    };
    return tone[phase % 48u];
}

pk_airband_speaker_queue_action_t pk_airband_speaker_queue_action(
    size_t queued, size_t capacity, bool playable)
{
    if (capacity == 0) return PK_AIRBAND_SPEAKER_QUEUE_REJECT;
    if (!playable) return PK_AIRBAND_SPEAKER_QUEUE_FLUSH;
    if (queued >= capacity)
        return PK_AIRBAND_SPEAKER_QUEUE_DROP_OLDEST_ENQUEUE;
    return PK_AIRBAND_SPEAKER_QUEUE_ENQUEUE;
}

bool pk_airband_speaker_capture_continuous(bool read_ok, bool pcm_valid,
                                           size_t rendered_frames)
{
    return read_ok && pcm_valid && rendered_frames != 0;
}

bool pk_airband_speaker_capture_playable(bool read_ok, bool pcm_valid,
                                         size_t rendered_frames,
                                         bool requested_playable)
{
    return pk_airband_speaker_capture_continuous(read_ok, pcm_valid,
                                                  rendered_frames) &&
           requested_playable;
}

uint32_t pk_airband_speaker_tone_frames(uint32_t duration_ms,
                                        uint32_t sample_rate_hz)
{
    if (duration_ms == 0 || sample_rate_hz == 0) return 0;
    if (duration_ms >= 1000) return sample_rate_hz;
    return (uint32_t)(((uint64_t)duration_ms * sample_rate_hz) / 1000u);
}

uint32_t pk_airband_speaker_tone_begin(uint32_t remaining_frames,
                                       uint32_t sample_rate_hz)
{
    if (remaining_frames != 0) return remaining_frames;
    return pk_airband_speaker_tone_frames(1000, sample_rate_hz);
}

pk_airband_speaker_test_result_t pk_airband_speaker_tone_result(
    bool setup_ok, uint32_t expected_frames, uint32_t written_frames,
    uint32_t write_errors)
{
    return setup_ok && expected_frames != 0 &&
                   written_frames == expected_frames && write_errors == 0
               ? PK_AIRBAND_SPEAKER_TEST_PASS
               : PK_AIRBAND_SPEAKER_TEST_FAIL;
}

bool pk_airband_speaker_pa_enabled(bool codec_online, bool muted,
                                   bool program_playable, bool tone_active)
{
    return codec_online && (tone_active || (!muted && program_playable));
}

bool pk_airband_speaker_source_audible(
    bool codec_online, bool live_muted, bool program_playable,
    bool playback_active, bool playback_muted, bool tone_active)
{
    return codec_online &&
           (tone_active || (playback_active ? !playback_muted
                                            : (!live_muted && program_playable)));
}

uint8_t pk_airband_speaker_source_volume(uint8_t live_volume,
                                         uint8_t playback_volume,
                                         bool playback_active)
{
    return playback_active ? playback_volume : live_volume;
}

float pk_airband_speaker_volume_db(uint8_t volume_percent)
{
    if (volume_percent > 100u) volume_percent = 100u;
    if (volume_percent == 0u) return -49.5f;
    /* esp_codec_dev 会再按 5.0 V/3.3 V 的硬件增益模型补约 +3.61 dB。
     * 请求 -1 dB 时 ES8311 实际数字增益约 +2.61 dB；配合 -3 dBFS 测试音
     * 仍留约 0.39 dB 余量。直接请求 +3 dB 会被补成约 +6.61 dB。 */
    return 20.0f * log10f((float)volume_percent / 100.0f) - 1.0f;
}

float pk_airband_speaker_test_volume_db(float user_volume_db)
{
    (void)user_volume_db;
    return -1.0f;
}
