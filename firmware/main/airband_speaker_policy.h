#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 16-bit full scale 的 70.7%（约 -3 dBFS）；codec 请求 -1 dB 后，驱动按
 * 硬件增益模型补约 +3.61 dB，实际约 +2.61 dB，仍留约 0.39 dB 余量。 */
#define PK_AIRBAND_SPEAKER_TEST_PEAK 23170

typedef enum {
    PK_AIRBAND_SPEAKER_QUEUE_REJECT = 0,
    PK_AIRBAND_SPEAKER_QUEUE_ENQUEUE,
    PK_AIRBAND_SPEAKER_QUEUE_DROP_OLDEST_ENQUEUE,
    PK_AIRBAND_SPEAKER_QUEUE_FLUSH,
} pk_airband_speaker_queue_action_t;

typedef enum {
    PK_AIRBAND_SPEAKER_TEST_IDLE = 0,
    PK_AIRBAND_SPEAKER_TEST_RUNNING,
    PK_AIRBAND_SPEAKER_TEST_PASS,
    PK_AIRBAND_SPEAKER_TEST_FAIL,
    PK_AIRBAND_SPEAKER_TEST_STOPPED,
} pk_airband_speaker_test_result_t;

typedef enum {
    PK_AIRBAND_SPEAKER_TEST_CLICK_REJECT = 0,
    PK_AIRBAND_SPEAKER_TEST_CLICK_START,
    PK_AIRBAND_SPEAKER_TEST_CLICK_STOP,
} pk_airband_speaker_test_click_t;

pk_airband_speaker_test_click_t pk_airband_speaker_test_click(
    bool codec_online, bool tone_active);

/* START 请求还没被播放任务消费就被取消时，本次实际写入必须报告为 0，
 * 不能沿用上一次测试保留在任务栈上的计数。 */
uint32_t pk_airband_speaker_cancel_written_frames(bool start_pending,
                                                   uint32_t active_written);

/* 48 kHz 下的 1 kHz、-3 dBFS 峰值整周期采样。phase 可任意递增。 */
int16_t pk_airband_speaker_tone_sample(uint32_t phase);

/* 生产端必须零等待：满队列丢最旧；不可播放时清掉所有陈旧音频。 */
pk_airband_speaker_queue_action_t pk_airband_speaker_queue_action(
    size_t queued, size_t capacity, bool playable);

/* 采集门是 fail-closed：超时、零长度、坏帧或零渲染帧都必须覆盖旧的 open。 */
bool pk_airband_speaker_capture_continuous(bool read_ok, bool pcm_valid,
                                           size_t rendered_frames);

bool pk_airband_speaker_capture_playable(bool read_ok, bool pcm_valid,
                                         size_t rendered_frames,
                                         bool requested_playable);

/* 本地诊断音最多一秒，返回指定采样率下允许生成的帧数。 */
uint32_t pk_airband_speaker_tone_frames(uint32_t duration_ms,
                                        uint32_t sample_rate_hz);

/* 已有测试音播放时忽略重入请求，保证单次最多一秒而不是反复续时。 */
uint32_t pk_airband_speaker_tone_begin(uint32_t remaining_frames,
                                       uint32_t sample_rate_hz);

/* “完成”必须同时满足 codec/PA 配置成功、帧数完整且期间无写错误。 */
pk_airband_speaker_test_result_t pk_airband_speaker_tone_result(
    bool setup_ok, uint32_t expected_frames, uint32_t written_frames,
    uint32_t write_errors);

/* PA 只在 codec 在线且节目门已打开时导通；诊断音可临时越过静音/节目门。 */
bool pk_airband_speaker_pa_enabled(bool codec_online, bool muted,
                                   bool program_playable, bool tone_active);

/* 实时接收与历史回放是两个独立音源。历史回放不能继承实时接收的静音或音量。 */
bool pk_airband_speaker_source_audible(
    bool codec_online, bool live_muted, bool program_playable,
    bool playback_active, bool playback_muted, bool tone_active);
uint8_t pk_airband_speaker_source_volume(uint8_t live_volume,
                                         uint8_t playback_volume,
                                         bool playback_active);

/* UI 音量是线性幅度百分比，codec 接口收 dB；0 使用 ES8311 的安全下限。 */
float pk_airband_speaker_volume_db(uint8_t volume_percent);

/* 本地诊断音不继承可能过低的用户音量，使用补偿后不削顶的 codec 请求。 */
float pk_airband_speaker_test_volume_db(float user_volume_db);
