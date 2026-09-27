/* AIRBAND PCM1808 → ES8311 纯音频转换：
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 *      -o /tmp/test_airband_audio firmware/test/test_airband_audio.c \
 *      firmware/main/airband_audio.c && /tmp/test_airband_audio
 *
 * 每个断言都对应一个会真正伤到喇叭输出的突变：符号扩展错会把负半轴变爆音，
 * 取右声道会全静音，硬门控会产生 click，畸形块若改状态会让下一块音量跳变。
 */
#include "airband_audio.h"

#include <stdio.h>
#include <string.h>

static int fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); fail++; } } while (0)

static void put_s24le(uint8_t out[3], int32_t value)
{
    const uint32_t u = (uint32_t)value & 0x00FFFFFFu;
    out[0] = (uint8_t)u;
    out[1] = (uint8_t)(u >> 8);
    out[2] = (uint8_t)(u >> 16);
}

static void put_frame(uint8_t out[6], int32_t left, int32_t right)
{
    put_s24le(out, left);
    put_s24le(out + 3, right);
}

static void test_exact_signed_conversion_and_left_channel(void)
{
    uint8_t src[24];
    put_frame(src + 0, 0, 0x7FFFFF);
    put_frame(src + 6, 0x7FFFFF, 0);
    put_frame(src + 12, -0x800000, 0x123456);
    put_frame(src + 18, 0x012345, -0x800000);

    int16_t out[8] = {0};
    pk_airband_audio_gate_t gate;
    pk_airband_audio_gate_reset(&gate, true);
    const size_t n = pk_airband_audio_render(src, sizeof(src), out, 4,
                                             true, &gate);
    CHECK(n == 4, "rendered frames=%zu\n", n);
    const int16_t want[] = {
        0, 0,
        32767, 32767,
        -32768, -32768,
        0x048C, 0x048C,
    };
    CHECK(memcmp(out, want, sizeof(want)) == 0,
          "signed conversion/channel duplication mismatch\n");
}

static void test_program_gain_is_four_times_and_saturating(void)
{
    uint8_t src[18];
    put_frame(src + 0, 0x010000, 0);   /* s16=256 -> 1024 */
    put_frame(src + 6, 0x500000, 0);   /* positive clip */
    put_frame(src + 12, -0x500000, 0); /* negative clip */
    int16_t out[6] = {0};
    pk_airband_audio_gate_t gate;
    pk_airband_audio_gate_reset(&gate, true);
    CHECK(pk_airband_audio_render(src, sizeof(src), out, 3, true, &gate) == 3,
          "gain render failed\n");
    CHECK(out[0] == 1024 && out[1] == 1024,
          "quiet AIRBAND PCM was not amplified 4x: %d/%d\n", out[0], out[1]);
    CHECK(out[2] == 32767 && out[3] == 32767,
          "positive gain overflow did not saturate\n");
    CHECK(out[4] == -32768 && out[5] == -32768,
          "negative gain overflow did not saturate\n");
}

static void test_open_and_close_are_ramped(void)
{
    uint8_t src[6 * 140];
    for (size_t i = 0; i < 140; i++)
        put_frame(src + i * 6, 0x7FFFFF, 0);

    int16_t out[140 * 2];
    pk_airband_audio_gate_t gate;
    pk_airband_audio_gate_reset(&gate, false);
    CHECK(pk_airband_audio_render(src, 2 * 6, out, 2, true, &gate) == 2,
          "open ramp render failed\n");
    CHECK(out[0] > 0 && out[0] < out[2] && out[2] < 32767,
          "open gate jumped instead of ramping: %d %d\n", out[0], out[2]);
    CHECK(out[0] == out[1] && out[2] == out[3],
          "ramp output not duplicated to stereo\n");

    pk_airband_audio_gate_reset(&gate, true);
    CHECK(pk_airband_audio_render(src, sizeof(src), out, 140, false, &gate) == 140,
          "close ramp render failed\n");
    CHECK(out[0] < 32767 && out[0] > out[2],
          "close gate did not start with a ramp: %d %d\n", out[0], out[2]);
    CHECK(out[278] == 0 && out[279] == 0 && gate.level_q15 == 0,
          "close gate did not settle at zero: tail=%d/%d level=%u\n",
          out[278], out[279], gate.level_q15);
}

static void test_invalid_input_is_transactional(void)
{
    uint8_t src[7] = {0};
    int16_t out[4] = {111, 222, 333, 444};
    const int16_t before[4] = {111, 222, 333, 444};
    pk_airband_audio_gate_t gate;
    pk_airband_audio_gate_reset(&gate, true);
    const uint16_t level = gate.level_q15;

    CHECK(pk_airband_audio_render(src, sizeof(src), out, 2, false, &gate) == 0,
          "partial packed frame accepted\n");
    CHECK(memcmp(out, before, sizeof(out)) == 0 && gate.level_q15 == level,
          "invalid source changed output or gate state\n");
    CHECK(pk_airband_audio_render(src, 6, out, 0, true, &gate) == 0,
          "insufficient capacity accepted\n");
    CHECK(pk_airband_audio_render(NULL, 6, out, 2, true, &gate) == 0 &&
          pk_airband_audio_render(src, 6, NULL, 2, true, &gate) == 0 &&
          pk_airband_audio_render(src, 6, out, 2, true, NULL) == 0,
          "NULL input accepted\n");
}

static void test_stream_break_discards_stale_release_level(void)
{
    uint8_t src[6];
    put_frame(src, 0x7FFFFF, 0);
    int16_t out[2] = {-1, -1};
    pk_airband_audio_gate_t gate;
    pk_airband_audio_gate_reset(&gate, true);

    pk_airband_audio_gate_stream_break(&gate);
    CHECK(gate.level_q15 == 0,
          "stream break retained the old open ramp level\n");
    CHECK(pk_airband_audio_render(src, sizeof(src), out, 1, false, &gate) == 1,
          "closed block after stream break did not render\n");
    CHECK(out[0] == 0 && out[1] == 0 && gate.level_q15 == 0,
          "closed block after stream break replayed stale release audio\n");
}

static void test_stream_break_forces_next_open_block_through_attack_ramp(void)
{
    uint8_t src[6];
    put_frame(src, 0x7FFFFF, 0);
    int16_t out[2] = {0};
    pk_airband_audio_gate_t gate;
    pk_airband_audio_gate_reset(&gate, true);

    pk_airband_audio_gate_stream_break(&gate);
    CHECK(pk_airband_audio_render(src, sizeof(src), out, 1, true, &gate) == 1,
          "open block after stream break did not render\n");
    CHECK(out[0] > 0 && out[0] < 1024 && out[0] == out[1] &&
          gate.level_q15 == PK_AIRBAND_AUDIO_GATE_STEP_Q15,
          "open block skipped attack ramp: out=%d/%d level=%u\n",
          out[0], out[1], gate.level_q15);
}

int main(void)
{
    test_exact_signed_conversion_and_left_channel();
    test_program_gain_is_four_times_and_saturating();
    test_open_and_close_are_ramped();
    test_invalid_input_is_transactional();
    test_stream_break_discards_stale_release_level();
    test_stream_break_forces_next_open_block_through_attack_ramp();
    puts(fail ? "FAILED" : "OK");
    return fail ? 1 : 0;
}
