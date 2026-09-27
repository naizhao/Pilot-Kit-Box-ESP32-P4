/*
 * BK4819 AIRBAND 纯寄存器层：
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 *      -o /tmp/test_airband_bk4819 firmware/test/test_airband_bk4819.c \
 *      firmware/main/airband_bk4819.c && /tmp/test_airband_bk4819
 */
#include "airband_bk4819.h"

#include <stdio.h>
#include <string.h>

static int fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); fail++; } } while (0)

static void test_frequency_word(void)
{
    uint32_t word = 0;
    CHECK(pk_airband_frequency_word(121500000u, &word), "121.5 MHz rejected\n");
    CHECK(word == 12150000u, "frequency word=%lu\n", (unsigned long)word);
    CHECK(pk_airband_frequency_word(118008330u, &word), "8.33 kHz channel rejected\n");
    CHECK(word == 11800833u, "8.33 kHz rounded word=%lu\n", (unsigned long)word);
    CHECK(!pk_airband_frequency_word(117999990u, &word), "below band accepted\n");
    CHECK(!pk_airband_frequency_word(137000010u, &word), "above band accepted\n");
}

static void test_rx_only_sequence(void)
{
    size_t n = 0;
    const pk_airband_reg_step_t *s = pk_airband_rx_init_seq(&n);
    CHECK(s != NULL && n >= 16, "init sequence too short (%zu)\n", n);
    bool saw_am = false, saw_rx = false, saw_pa_safe = false;
    for (size_t i = 0; i < n; i++) {
        CHECK(s[i].why != NULL && s[i].why[0], "step %zu missing evidence\n", i);
        if (s[i].reg == 0x47 && s[i].val == 0x6740) saw_am = true;
        if (s[i].reg == 0x36) {
            saw_pa_safe = true;
            CHECK((s[i].val & 0xFF80u) == 0,
                  "REG36 enables PA bias/control: 0x%04X\n", s[i].val);
        }
        if (s[i].reg == 0x30 && s[i].val != 0) {
            saw_rx = true;
            CHECK((s[i].val & ((1u << 3) | (1u << 2) | (1u << 1))) == 0,
                  "REG30 enables PA/MIC/TX: 0x%04X\n", s[i].val);
            CHECK((s[i].val & (1u << 0)) != 0, "REG30 RX DSP disabled\n");
        }
    }
    CHECK(saw_am, "AM audio path not selected\n");
    CHECK(saw_rx, "RX enable step missing\n");
    CHECK(saw_pa_safe, "explicit PA-safe REG36 step missing\n");
}

static void test_frequency_registers(void)
{
    pk_airband_reg_step_t out[2] = {0};
    CHECK(pk_airband_frequency_steps(121500000u, out), "frequency steps failed\n");
    CHECK(out[0].reg == 0x38 && out[0].val == 0x64F0,
          "REG38 got 0x%02X=0x%04X\n", out[0].reg, out[0].val);
    CHECK(out[1].reg == 0x39 && out[1].val == 0x00B9,
          "REG39 got 0x%02X=0x%04X\n", out[1].reg, out[1].val);

    pk_airband_reg_step_t tune[4] = {0};
    CHECK(pk_airband_rx_retune_steps(121500000u, tune),
          "RX retune steps failed\n");
    CHECK(tune[0].reg == 0x30 && tune[0].val == 0,
          "retune does not stop RX first\n");
    CHECK(tune[1].reg == 0x38 && tune[1].val == out[0].val &&
          tune[2].reg == 0x39 && tune[2].val == out[1].val,
          "retune frequency words differ\n");
    CHECK(tune[3].reg == 0x30 && tune[3].val != 0,
          "retune RX enable missing\n");
    CHECK((tune[3].val & ((1u << 3) | (1u << 2) | (1u << 1))) == 0 &&
          (tune[3].val & 1u) != 0,
          "retune enables PA/MIC/TX or disables RX: 0x%04X\n", tune[3].val);
    CHECK(!pk_airband_rx_retune_steps(117000000u, tune),
          "out-of-band retune accepted\n");
}

static void test_diagnostics_decode(void)
{
    pk_airband_diag_t d;
    pk_airband_decode_diag(0x0064, 0x127E, 0x03F2, &d);
    CHECK(d.rssi_raw == 100, "rssi=%u\n", d.rssi_raw);
    CHECK(d.rssi_dbm_x2 == -220, "rssi x2=%d\n", d.rssi_dbm_x2);
    CHECK(d.noise_raw == 126, "noise=%u\n", d.noise_raw);
    CHECK(d.glitch_raw == 242, "glitch=%u\n", d.glitch_raw);

    memset(&d, 0xA5, sizeof(d));
    CHECK(pk_airband_decode_valid_rf(true, 0xBFF1, 0xBFF1,
                                     0x0064, 0x127E, 0x03F2, &d),
          "valid RF window rejected\n");
    CHECK(d.rssi_raw == 100 && d.rssi_dbm_x2 == -220 &&
          d.noise_raw == 126 && d.glitch_raw == 242,
          "valid RF window decoded incorrectly\n");

    const uint16_t bad_r30[] = {0x4819, 0xFFFF, 0x0000};
    for (size_t i = 0; i < sizeof(bad_r30) / sizeof(bad_r30[0]); i++) {
        memset(&d, 0xA5, sizeof(d));
        CHECK(!pk_airband_decode_valid_rf(true, bad_r30[i], 0xBFF1,
                                          0x01FF, 0x007F, 0x00FF, &d),
              "invalid pre-window r30=%04X accepted\n", bad_r30[i]);
        CHECK(d.rssi_raw == 0 && d.rssi_dbm_x2 == 0 &&
              d.noise_raw == 0 && d.glitch_raw == 0,
              "invalid RF window leaked decoded values\n");
    }
    memset(&d, 0xA5, sizeof(d));
    CHECK(!pk_airband_decode_valid_rf(false, 0xBFF1, 0xBFF1,
                                      0x01FF, 0x007F, 0x00FF, &d),
          "unconfigured RF accepted\n");
    CHECK(d.rssi_raw == 0 && d.rssi_dbm_x2 == 0 &&
          d.noise_raw == 0 && d.glitch_raw == 0,
          "unconfigured RF leaked decoded values\n");
}

static void test_health_assessment(void)
{
    CHECK(pk_airband_assess(false, 0, 0) == PK_AIRBAND_HEALTH_RF_OFFLINE,
          "offline RF reported healthy\n");
    CHECK(pk_airband_assess(true, 0, 0) == PK_AIRBAND_HEALTH_AUDIO_IDLE,
          "missing audio frames not reported idle\n");
    CHECK(pk_airband_assess(true, 36096, 0) == PK_AIRBAND_HEALTH_READY,
          "live RF/audio path not ready\n");
    CHECK(pk_airband_assess(true, 36096, 1) == PK_AIRBAND_HEALTH_DEGRADED,
          "I2S read error not reported degraded\n");
}

static void put_s24le(uint8_t out[3], int32_t value)
{
    const uint32_t u = (uint32_t)value & 0x00FFFFFFu;
    out[0] = (uint8_t)u;
    out[1] = (uint8_t)(u >> 8);
    out[2] = (uint8_t)(u >> 16);
}

static void test_packed24_stereo_stats(void)
{
    CHECK(PK_AIRBAND_I2S_DMA_FRAME_NUM % 3u == 0,
          "24-bit DMA frame count is not divisible by 3\n");
    CHECK(PK_AIRBAND_PACKED_STEREO_FRAME_BYTES == 6u,
          "packed stereo frame width is not 6 bytes\n");
    CHECK((PK_AIRBAND_I2S_DMA_FRAME_NUM *
           PK_AIRBAND_PACKED_STEREO_FRAME_BYTES) % 64u == 0,
          "DMA buffer is not ESP32-P4 64-byte cache-line aligned\n");
    uint8_t data[18];
    const int32_t left[] = {3, 4, -8388608};
    const int32_t right[] = {-5, 12, 8388607};
    for (size_t i = 0; i < 3; i++) {
        put_s24le(&data[i * 6], left[i]);
        put_s24le(&data[i * 6 + 3], right[i]);
    }

    pk_airband_audio_stats_t s;
    pk_airband_audio_stats_reset(&s);
    CHECK(pk_airband_audio_accumulate_packed24(data, sizeof(data), &s),
          "packed 24-bit stereo rejected\n");
    CHECK(s.frames == 3, "packed frames=%lu\n", (unsigned long)s.frames);
    CHECK(s.left_nonzero == 3 && s.right_nonzero == 3,
          "nonzero L/R=%lu/%lu\n", (unsigned long)s.left_nonzero,
          (unsigned long)s.right_nonzero);
    CHECK(s.left_min == -8388608 && s.left_max == 4,
          "left range=%ld..%ld\n", (long)s.left_min, (long)s.left_max);
    CHECK(s.right_min == -5 && s.right_max == 8388607,
          "right range=%ld..%ld\n", (long)s.right_min, (long)s.right_max);
    CHECK(s.left_sum_sq == 70368744177689ULL,
          "left sumsq=%llu\n", (unsigned long long)s.left_sum_sq);
    CHECK(s.right_sum_sq == 70368727400618ULL,
          "right sumsq=%llu\n", (unsigned long long)s.right_sum_sq);

    const pk_airband_audio_stats_t before = s;
    CHECK(!pk_airband_audio_accumulate_packed24(data, sizeof(data) - 1, &s),
          "partial stereo frame accepted\n");
    CHECK(memcmp(&before, &s, sizeof(s)) == 0,
          "partial frame changed accumulated stats\n");
}

static void test_audio_rms(void)
{
    CHECK(pk_airband_audio_rms(25, 2) == 3,
          "left RMS should floor sqrt(12.5) to 3\n");
    CHECK(pk_airband_audio_rms(169, 2) == 9,
          "right RMS should floor sqrt(84.5) to 9\n");
    CHECK(pk_airband_audio_rms(123, 0) == 0, "empty RMS nonzero\n");
}

static void test_scan_plan(void)
{
    const pk_airband_scan_plan_t p = {
        .start_hz = 118000000u,
        .end_hz = 118050000u,
        .step_hz = 25000u,
        .dwell_ms = 200u,
    };
    uint32_t next = 0;
    bool wrapped = true;
    CHECK(pk_airband_scan_plan_valid(&p), "valid scan rejected\n");
    CHECK(pk_airband_scan_next(&p, 118000000u, &next, &wrapped) &&
          next == 118025000u && !wrapped,
          "scan advance got %lu wrapped=%d\n", (unsigned long)next, wrapped);
    CHECK(pk_airband_scan_next(&p, 118025000u, &next, &wrapped) &&
          next == 118050000u && !wrapped,
          "scan skipped exact end got %lu wrapped=%d\n",
          (unsigned long)next, wrapped);
    CHECK(pk_airband_scan_next(&p, 118050000u, &next, &wrapped) &&
          next == 118000000u && wrapped,
          "scan wrap got %lu wrapped=%d\n", (unsigned long)next, wrapped);

    pk_airband_scan_plan_t bad = p;
    bad.start_hz = 117999990u;
    CHECK(!pk_airband_scan_plan_valid(&bad), "below-band scan accepted\n");
    bad = p; bad.end_hz = 137000010u;
    CHECK(!pk_airband_scan_plan_valid(&bad), "above-band scan accepted\n");
    bad = p; bad.start_hz = bad.end_hz + 1u;
    CHECK(!pk_airband_scan_plan_valid(&bad), "reversed scan accepted\n");
    bad = p; bad.step_hz = 0;
    CHECK(!pk_airband_scan_plan_valid(&bad), "zero step accepted\n");
    bad = p; bad.dwell_ms = 0;
    CHECK(!pk_airband_scan_plan_valid(&bad), "zero dwell accepted\n");
    bad = p; bad.step_hz = PK_AIRBAND_SCAN_MIN_STEP_HZ - 1u;
    CHECK(!pk_airband_scan_plan_valid(&bad),
          "sub-resolution scan step accepted\n");
    bad = p; bad.dwell_ms = PK_AIRBAND_SCAN_MIN_DWELL_MS - 1u;
    CHECK(!pk_airband_scan_plan_valid(&bad),
          "dwell shorter than RX retune accepted\n");
    bad = p; bad.step_hz = PK_AIRBAND_SCAN_MIN_STEP_HZ;
    bad.dwell_ms = PK_AIRBAND_SCAN_MIN_DWELL_MS;
    CHECK(pk_airband_scan_plan_valid(&bad),
          "minimum realizable scan plan rejected\n");
}

static void test_squelch_hysteresis(void)
{
    CHECK(pk_airband_squelch_gate(false, false, -280, -180, 12),
          "disabled squelch did not leave audio gate open\n");
    CHECK(!pk_airband_squelch_gate(true, false, -181, -180, 12),
          "enabled squelch ignored threshold\n");
    CHECK(!pk_airband_squelch_update(false, -181, -180, 12),
          "squelch opened below threshold\n");
    CHECK(pk_airband_squelch_update(false, -180, -180, 12),
          "squelch did not open at threshold\n");
    CHECK(pk_airband_squelch_update(true, -191, -180, 12),
          "squelch ignored hysteresis hold\n");
    CHECK(!pk_airband_squelch_update(true, -193, -180, 12),
          "squelch did not close below hysteresis\n");
}

int main(void)
{
    test_frequency_word();
    test_rx_only_sequence();
    test_frequency_registers();
    test_diagnostics_decode();
    test_health_assessment();
    test_packed24_stereo_stats();
    test_audio_rms();
    test_scan_plan();
    test_squelch_hysteresis();
    if (fail) return 1;
    puts("OK");
    return 0;
}
