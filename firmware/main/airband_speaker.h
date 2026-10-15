#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "airband_playback_policy.h"
#include "airband_speaker_policy.h"

typedef struct {
    bool online;
    bool muted;
    bool tone_active;
    bool pa_enabled;
    uint8_t volume;
    pk_airband_speaker_test_result_t test_result;
    uint32_t test_expected_frames;
    uint32_t test_written_frames;
    uint32_t output_frames;
    uint32_t queue_drops;
    uint32_t write_errors;
    uint32_t codec_restarts;
    uint32_t queued_blocks;
    bool program_playable;
    bool playback_active;
    bool playback_muted;
    uint8_t playback_volume;
    uint32_t playback_queued_blocks;
    pk_airband_speaker_playback_result_t playback_last_result;
} pk_airband_speaker_status_t;

esp_err_t pk_airband_speaker_start(void);

/* 零等待提交；playable=false 会清掉队列，避免静噪关闭后泄漏旧音频。 */
bool pk_airband_speaker_submit(const int16_t *stereo, size_t frames,
                               bool playable);
/* 与 PCM 队列解耦的节目门；false 会立即丢弃陈旧队列并请求 codec mute/PA low。 */
void pk_airband_speaker_set_program_playable(bool playable);

/* Low-priority history source. The call waits for the speaker task to either
 * write this block or reject it; only WRITTEN may advance the file cursor. */
pk_airband_speaker_playback_result_t pk_airband_speaker_playback_submit(
    const int16_t *stereo, size_t frames, uint32_t timeout_ms);
/* Capture a cancellation token before rendering. submit_checked rejects a
 * block when pause/live/tone/stop changed speaker generation in between. */
uint32_t pk_airband_speaker_playback_token(void);
pk_airband_speaker_playback_result_t pk_airband_speaker_playback_submit_checked(
    const int16_t *stereo, size_t frames, uint32_t timeout_ms,
    uint32_t expected_generation);
void pk_airband_speaker_playback_stop(void);
void pk_airband_speaker_playback_set_muted(bool muted);
void pk_airband_speaker_playback_set_volume(uint8_t volume);

bool pk_airband_speaker_get(pk_airband_speaker_status_t *out);
void pk_airband_speaker_set_muted(bool muted);
bool pk_airband_speaker_is_muted(void);
/* BLE route overlay is volatile and never persisted. It combines with the
 * local/NVS mute by OR; disconnect must clear it. */
void pk_airband_speaker_set_remote_muted(bool muted);
void pk_airband_speaker_set_volume(uint8_t volume);
pk_airband_speaker_test_click_t pk_airband_speaker_test_tone_action(void);
bool pk_airband_speaker_test_tone_active(void);
bool pk_airband_speaker_test_tone(void);
