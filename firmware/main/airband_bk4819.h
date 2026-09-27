/* AIRBAND BK4819 的纯寄存器层；无 ESP-IDF 依赖，host 可测。 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ESP-IDF 6.0.1 对 ESP32-P4 的 24-bit I2S DMA 使用紧凑 3 byte/sample。
 * 192 帧 × 2 声道 × 3 byte = 1152 byte：既不切断 packed stereo 帧，
 * 又正好按 P4 的 64-byte cache line 对齐，驱动不会再暗改 frame_num。 */
#define PK_AIRBAND_I2S_DMA_FRAME_NUM 192u
#define PK_AIRBAND_PACKED_STEREO_FRAME_BYTES 6u
#define PK_AIRBAND_SCAN_MIN_STEP_HZ 10u
/* RX 重调固定等待约 10 ms；至少再保留 10 ms 的有效接收驻留。 */
#define PK_AIRBAND_SCAN_MIN_DWELL_MS 20u

typedef struct {
    uint8_t reg;
    uint16_t val;
    const char *why;
} pk_airband_reg_step_t;

typedef struct {
    uint16_t rssi_raw;       /* REG67[8:0]，0.5 dB/LSB */
    int16_t  rssi_dbm_x2;    /* dBm×2 = raw - 320 */
    uint8_t  noise_raw;      /* REG65[6:0] */
    uint8_t  glitch_raw;     /* REG63[7:0] */
} pk_airband_diag_t;

typedef enum {
    PK_AIRBAND_HEALTH_RF_OFFLINE = 0,
    PK_AIRBAND_HEALTH_AUDIO_IDLE,
    PK_AIRBAND_HEALTH_DEGRADED,
    PK_AIRBAND_HEALTH_READY,
} pk_airband_health_t;

typedef struct {
    uint32_t frames;
    uint32_t left_nonzero;
    uint32_t right_nonzero;
    int32_t left_min;
    int32_t left_max;
    int32_t right_min;
    int32_t right_max;
    uint64_t left_sum_sq;
    uint64_t right_sum_sq;
} pk_airband_audio_stats_t;

typedef struct {
    uint32_t start_hz;
    uint32_t end_hz;
    uint32_t step_hz;
    uint32_t dwell_ms;
} pk_airband_scan_plan_t;

/* 本板只承诺 118.000000–137.000000 MHz AM 话音接收。频率字单位 10 Hz。 */
bool pk_airband_frequency_word(uint32_t hz, uint32_t *word);
bool pk_airband_frequency_steps(uint32_t hz, pk_airband_reg_step_t out[2]);
bool pk_airband_rx_retune_steps(uint32_t hz, pk_airband_reg_step_t out[4]);

/* RX-only 初始化表：不含频率两寄存器；调用方在 AF 配置后插入频率，再执行
 * 表尾的 RX enable。表内明确关闭 PA/MIC ADC/TX DSP。 */
const pk_airband_reg_step_t *pk_airband_rx_init_seq(size_t *n);

void pk_airband_decode_diag(uint16_t reg67, uint16_t reg65, uint16_t reg63,
                            pk_airband_diag_t *out);

/* 只有配置状态与诊断窗口前后两次 RX 控制寄存器都可信时才发布 RF 数值；
 * 失败时清零 out，避免把 FFFF/4819 解码成伪造的强信号。 */
bool pk_airband_decode_valid_rf(bool configured,
                                uint16_t reg30_before, uint16_t reg30_after,
                                uint16_t reg67, uint16_t reg65, uint16_t reg63,
                                pk_airband_diag_t *out);

/* 把 RF 寄存器回读与 PCM1808 实际采集结果合成诊断结论。室内没有话音并不
 * 等于 AUDIO_IDLE：只要 I2S 持续交付帧，模拟链和数字时钟闭环就已成立。 */
pk_airband_health_t pk_airband_assess(bool rf_online, uint32_t audio_frames,
                                      uint32_t read_errors);

/* ESP32-P4/ESP-IDF 6 的 24-bit 标准 I2S DMA 数据每声道紧凑占 3 byte，左右
 * 声道交错，每帧 6 byte。输入不是完整帧时返回 false 且不修改累计值。 */
void pk_airband_audio_stats_reset(pk_airband_audio_stats_t *stats);
bool pk_airband_audio_accumulate_packed24(const uint8_t *data, size_t bytes,
                                          pk_airband_audio_stats_t *stats);
uint32_t pk_airband_audio_rms(uint64_t sum_sq, uint32_t samples);

/* 扫描只生成合法的 RX 调谐频率；不涉及发射寄存器。 */
bool pk_airband_scan_plan_valid(const pk_airband_scan_plan_t *plan);
bool pk_airband_scan_next(const pk_airband_scan_plan_t *plan,
                          uint32_t current_hz, uint32_t *next_hz,
                          bool *wrapped);

/* RSSI 门限带迟滞：达到 open 门限开，低于 open-hysteresis 才关。 */
bool pk_airband_squelch_update(bool was_open, int16_t rssi_dbm_x2,
                               int16_t open_dbm_x2,
                               uint16_t hysteresis_db_x2);
bool pk_airband_squelch_gate(bool enabled, bool was_open,
                             int16_t rssi_dbm_x2, int16_t open_dbm_x2,
                             uint16_t hysteresis_db_x2);
