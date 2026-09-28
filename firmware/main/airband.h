/* V4.9 AIRBAND（BK4819 + PCM1808）最小接收闭环。 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "airband_bk4819.h"
#include "airband_am_gain.h"
#include "airband_pcm16.h"
#include "airband_speaker_policy.h"

typedef struct {
    bool online;
    uint32_t frequency_hz;
    pk_airband_diag_t rf;
    int16_t reference_rssi_dbm_x2;
    uint8_t gain_index;
    int8_t gain_db;
    pk_airband_am_gain_reason_t gain_reason;
    pk_airband_bk_gain_diag_t gain_regs;
    uint32_t gain_control_max_us;
    uint32_t gain_control_overruns;
    uint32_t gain_control_max_late_us;
    pk_airband_pcm16_stats_t pcm16;
    pk_airband_health_t health;
    uint32_t audio_frames;
    uint32_t audio_nonzero;
    int32_t audio_min;
    int32_t audio_max;
    uint32_t audio_rms;
    uint32_t audio_right_nonzero;
    int32_t audio_right_min;
    int32_t audio_right_max;
    uint32_t audio_right_rms;
    uint32_t read_errors;
    bool speaker_online;
    bool speaker_muted;
    bool speaker_tone_active;
    bool speaker_pa_enabled;
    uint8_t speaker_volume;
    pk_airband_speaker_test_result_t speaker_test_result;
    uint32_t speaker_test_expected_frames;
    uint32_t speaker_test_written_frames;
    uint32_t speaker_output_frames;
    uint32_t speaker_queue_drops;
    uint32_t speaker_write_errors;
    uint32_t speaker_codec_restarts;
    uint32_t speaker_queued_blocks;
    bool squelch_enabled;
    bool squelch_open;
    int16_t squelch_open_dbm_x2;
    uint16_t squelch_hysteresis_db_x2;
    bool scanning;
    pk_airband_scan_plan_t scan_plan;
    uint32_t scan_wraps;
    int64_t updated_us;
} pk_airband_state_t;

typedef struct {
    uint32_t requested_hz;
    uint32_t requested_channel_khz;
    bool scanning;
    pk_airband_scan_plan_t scan_plan;
    bool squelch_enabled;
    int16_t squelch_open_dbm_x2;
    uint16_t squelch_hysteresis_db_x2;
    bool speaker_muted;
    uint8_t speaker_volume;
} pk_airband_control_status_t;

/* 起 BK4819 RX-only 配置/状态轮询与 PCM1808 I2S 采集统计任务。器件或音频链
 * 缺席时降级为周期日志，不阻塞整机其它功能。 */
esp_err_t pk_airband_start(void);

/* 最近一个 1 s 采集窗口的原子快照；尚未形成首个窗口时返回 false 并清零。 */
bool pk_airband_get(pk_airband_state_t *out);

/* 运行时调谐接口。设置单频会停止扫描；带外频率被拒绝且不改变当前状态。 */
bool pk_airband_set_frequency(uint32_t frequency_hz);

/* 航空频道入口：保留驾驶舱显示的频道标识，并把 8.33 kHz 频道换算成载频。 */
bool pk_airband_set_channel(uint32_t channel_khz);

/* 在合法航空话音频段内循环扫描；每个 dwell 到期只执行 RX 重新调谐。 */
bool pk_airband_scan_start(const pk_airband_scan_plan_t *plan);
void pk_airband_scan_stop(void);

/* RSSI 静噪门限，单位均为 dB×2；默认 -90 dBm，迟滞 6 dB。 */
bool pk_airband_set_squelch(int16_t open_dbm_x2,
                            uint16_t hysteresis_db_x2);
void pk_airband_set_squelch_enabled(bool enabled);
bool pk_airband_control_get(pk_airband_control_status_t *out);

/* 微加载板 ES8311/NS4150B/H4 输出控制。测试音是本地 PCM，不碰 RF。 */
void pk_airband_set_speaker_muted(bool muted);
void pk_airband_set_speaker_volume(uint8_t volume);
pk_airband_speaker_test_click_t pk_airband_speaker_test_action(void);
bool pk_airband_speaker_test_active(void);
bool pk_airband_speaker_test(void);
