/*
 * AIRBAND BK4819 纯寄存器层。
 *
 * 三线时序、初始化值、AM 选择、频率字和 RSSI/噪声寄存器来自
 * DualTachyon/egzumer uv-k5-firmware-custom 的 Apache-2.0 驱动
 * driver/bk4819.c + bk4819-regs.h（2026-09-22 实读 main）。BK4819 的 AM
 * 模式 7 属未文档化能力，所以这里把它明确当台架待验证项，不冒充原厂保证。
 * PCM1808 时钟/格式则以 TI PCM1808 Rev.B 数据表为准，由 airband_task.c 实现。
 */
#include "airband_bk4819.h"

#include <limits.h>

#define RX_REG30 0xBFF1u /* VCO calibrate + RX link/AF/DISC/PLL + RX DSP；TX/PA/MIC=0 */

static const pk_airband_reg_step_t s_rx_init[] = {
    {0x00, 0x8000, "software reset assert (reference driver)"},
    {0x00, 0x0000, "software reset release (reference driver)"},
    {0x37, 0x1D0F, "DSP/analog/clock LDO setup (reference driver)"},
    {0x36, 0x0022, "PA bias=0 and PA-CTL disabled; RX-only safety"},
    {0x13, 0x03BE, "RX AGC table +3 (reference driver AM setup)"},
    {0x12, 0x037B, "RX AGC table +2 (reference driver AM setup)"},
    {0x11, 0x027B, "RX AGC table +1 (reference driver AM setup)"},
    {0x10, 0x007A, "RX AGC table 0 (reference driver AM setup)"},
    {0x14, 0x0000, "RX AGC table -1 for AM (reference driver)"},
    {0x49, 0x1920, "AM AGC high/low thresholds 50/32"},
    {0x7B, 0x8420, "RSSI table (reference driver)"},
    {0x19, 0x1041, "MIC AGC disabled (RX-only)"},
    {0x7D, 0xE940, "reference initialization value"},
    {0x48, 0xB3A8, "RX AF level"},
    {0x1F, 0x5454, "reference initialization value"},
    {0x3E, 0xA037, "reference initialization value"},
    {0x33, 0x9000, "BK GPIO defaults; board RF input is direct LNAIN"},
    {0x3F, 0x0000, "interrupts disabled; board has no BK IRQ route"},
    {0x43, 0x49A8, "wide receive filter (25/20 kHz profile)"},
    {0x47, 0x6740, "AF output mode 7 = AM (undocumented; bench verify)"},
    {0x37, 0x1F0F, "RX analog/DSP supplies enabled"},
    {0x30, 0x0000, "turn all signal paths off before RX transition"},
    {0x30, RX_REG30, "RX on; PA gain/MIC ADC/TX DSP explicitly disabled"},
};

bool pk_airband_frequency_word(uint32_t hz, uint32_t *word)
{
    if (word == NULL || hz < 118000000u || hz > 137000000u) return false;
    *word = (hz + 5u) / 10u;
    return true;
}

bool pk_airband_frequency_steps(uint32_t hz, pk_airband_reg_step_t out[2])
{
    uint32_t word;
    if (out == NULL || !pk_airband_frequency_word(hz, &word)) return false;
    out[0] = (pk_airband_reg_step_t){0x38, (uint16_t)word,
                                    "frequency word low 16 bits (10 Hz/LSB)"};
    out[1] = (pk_airband_reg_step_t){0x39, (uint16_t)(word >> 16),
                                    "frequency word high 16 bits (10 Hz/LSB)"};
    return true;
}

bool pk_airband_rx_retune_steps(uint32_t hz, pk_airband_reg_step_t out[4])
{
    pk_airband_reg_step_t frequency[2];
    if (out == NULL || !pk_airband_frequency_steps(hz, frequency)) return false;
    out[0] = (pk_airband_reg_step_t){0x30, 0x0000,
                                    "stop all paths before retune"};
    out[1] = frequency[0];
    out[2] = frequency[1];
    out[3] = (pk_airband_reg_step_t){0x30, RX_REG30,
                                    "resume RX only; PA/MIC/TX remain off"};
    return true;
}

const pk_airband_reg_step_t *pk_airband_rx_init_seq(size_t *n)
{
    if (n != NULL) *n = sizeof(s_rx_init) / sizeof(s_rx_init[0]);
    return s_rx_init;
}

void pk_airband_decode_diag(uint16_t reg67, uint16_t reg65, uint16_t reg63,
                            pk_airband_diag_t *out)
{
    if (out == NULL) return;
    out->rssi_raw = reg67 & 0x01FFu;
    out->rssi_dbm_x2 = (int16_t)out->rssi_raw - 320;
    out->noise_raw = reg65 & 0x7Fu;
    out->glitch_raw = reg63 & 0xFFu;
}

bool pk_airband_decode_valid_rf(bool configured,
                                uint16_t reg30_before, uint16_t reg30_after,
                                uint16_t reg67, uint16_t reg65, uint16_t reg63,
                                pk_airband_diag_t *out)
{
    if (out == NULL) return false;
    *out = (pk_airband_diag_t){0};
    if (!configured || reg30_before != RX_REG30 || reg30_after != RX_REG30) {
        return false;
    }
    pk_airband_decode_diag(reg67, reg65, reg63, out);
    return true;
}

pk_airband_health_t pk_airband_assess(bool rf_online, uint32_t audio_frames,
                                      uint32_t read_errors)
{
    if (!rf_online) return PK_AIRBAND_HEALTH_RF_OFFLINE;
    if (audio_frames == 0) return PK_AIRBAND_HEALTH_AUDIO_IDLE;
    if (read_errors != 0) return PK_AIRBAND_HEALTH_DEGRADED;
    return PK_AIRBAND_HEALTH_READY;
}

void pk_airband_audio_stats_reset(pk_airband_audio_stats_t *stats)
{
    if (stats == NULL) return;
    *stats = (pk_airband_audio_stats_t){
        .left_min = INT32_MAX,
        .left_max = INT32_MIN,
        .right_min = INT32_MAX,
        .right_max = INT32_MIN,
    };
}

static int32_t decode_s24le(const uint8_t data[3])
{
    uint32_t raw = (uint32_t)data[0] |
                   ((uint32_t)data[1] << 8) |
                   ((uint32_t)data[2] << 16);
    if ((raw & 0x00800000u) != 0) raw |= 0xFF000000u;
    return (int32_t)raw;
}

static void accumulate_sample(int32_t value, uint32_t *nonzero,
                              int32_t *min_value, int32_t *max_value,
                              uint64_t *sum_sq)
{
    if (value != 0) (*nonzero)++;
    if (value < *min_value) *min_value = value;
    if (value > *max_value) *max_value = value;
    const int64_t wide = value;
    *sum_sq += (uint64_t)(wide * wide);
}

bool pk_airband_audio_accumulate_packed24(const uint8_t *data, size_t bytes,
                                          pk_airband_audio_stats_t *stats)
{
    if (data == NULL || stats == NULL || bytes == 0 || bytes % 6u != 0) {
        return false;
    }
    pk_airband_audio_stats_t next = *stats;
    for (size_t offset = 0; offset < bytes; offset += 6u) {
        const int32_t left = decode_s24le(&data[offset]);
        const int32_t right = decode_s24le(&data[offset + 3u]);
        accumulate_sample(left, &next.left_nonzero, &next.left_min,
                          &next.left_max, &next.left_sum_sq);
        accumulate_sample(right, &next.right_nonzero, &next.right_min,
                          &next.right_max, &next.right_sum_sq);
        next.frames++;
    }
    *stats = next;
    return true;
}

static uint32_t integer_sqrt_u64(uint64_t value)
{
    uint64_t root = 0;
    uint64_t bit = UINT64_C(1) << 62;
    while (bit > value) bit >>= 2;
    while (bit != 0) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root > UINT32_MAX ? UINT32_MAX : (uint32_t)root;
}

uint32_t pk_airband_audio_rms(uint64_t sum_sq, uint32_t samples)
{
    return samples == 0 ? 0 : integer_sqrt_u64(sum_sq / samples);
}

bool pk_airband_scan_plan_valid(const pk_airband_scan_plan_t *plan)
{
    uint32_t unused;
    return plan != NULL && plan->start_hz <= plan->end_hz &&
           plan->step_hz >= PK_AIRBAND_SCAN_MIN_STEP_HZ &&
           plan->dwell_ms >= PK_AIRBAND_SCAN_MIN_DWELL_MS &&
           pk_airband_frequency_word(plan->start_hz, &unused) &&
           pk_airband_frequency_word(plan->end_hz, &unused);
}

bool pk_airband_scan_next(const pk_airband_scan_plan_t *plan,
                          uint32_t current_hz, uint32_t *next_hz,
                          bool *wrapped)
{
    if (!pk_airband_scan_plan_valid(plan) || next_hz == NULL ||
        wrapped == NULL || current_hz < plan->start_hz ||
        current_hz > plan->end_hz) {
        return false;
    }
    if (plan->end_hz - current_hz < plan->step_hz) {
        *next_hz = plan->start_hz;
        *wrapped = true;
    } else {
        *next_hz = current_hz + plan->step_hz;
        *wrapped = false;
    }
    return true;
}

bool pk_airband_squelch_update(bool was_open, int16_t rssi_dbm_x2,
                               int16_t open_dbm_x2,
                               uint16_t hysteresis_db_x2)
{
    if (!was_open) return rssi_dbm_x2 >= open_dbm_x2;
    const int32_t close_dbm_x2 = (int32_t)open_dbm_x2 - hysteresis_db_x2;
    return (int32_t)rssi_dbm_x2 >= close_dbm_x2;
}

bool pk_airband_squelch_gate(bool enabled, bool was_open,
                             int16_t rssi_dbm_x2, int16_t open_dbm_x2,
                             uint16_t hysteresis_db_x2)
{
    return !enabled || pk_airband_squelch_update(
                           was_open, rssi_dbm_x2, open_dbm_x2,
                           hysteresis_db_x2);
}
