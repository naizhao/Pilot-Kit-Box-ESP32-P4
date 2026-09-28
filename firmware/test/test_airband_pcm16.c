/*
 * cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 * -o /tmp/test_airband_pcm16 firmware/test/test_airband_pcm16.c \
 * firmware/main/airband_pcm16.c -lm && /tmp/test_airband_pcm16
 */
#include "airband_pcm16.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#define N 4800u
static int fail;
#define CHECK(c, m) do { if (!(c)) { puts("FAIL: " m); fail++; } } while (0)
static uint8_t input[N * 6];
static int16_t whole[N / 3], chunks[N / 3];
static void put24(uint8_t *p, int32_t v)
{ p[0] = v; p[1] = (uint32_t)v >> 8; p[2] = (uint32_t)v >> 16; }
static void tone(unsigned hz, int32_t amplitude)
{
    for (unsigned i = 0; i < N; i++) {
        put24(input + 6*i, (int32_t)lround(amplitude * sin(6.283185307179586 * hz * i / 48000)));
        put24(input + 6*i + 3, -1234567); /* right must never leak into program */
    }
}
static double rms(const int16_t *p, size_t n)
{ double sum = 0; for (size_t i = 0; i < n; i++) sum += (double)p[i] * p[i]; return sqrt(sum/n); }
static void test_filter_and_chunks(void)
{
    pk_airband_pcm16_t a = {0}, b = {0};
    pk_airband_pcm16_reset(&a); pk_airband_pcm16_reset(&b);
    tone(1000, 1000000);
    CHECK(pk_airband_pcm16_process(&a, input, sizeof(input), whole, N/3, 1) == N/3, "wrong 3:1 duration");
    size_t written = 0;
    for (size_t i = 0; i < N;) {
        size_t take = 1 + i % 197;
        if (take > N-i) take = N-i;
        written += pk_airband_pcm16_process(&b, input + i*6, take*6, chunks+written, N/3-written, 1);
        i += take;
    }
    CHECK(written == N/3 && memcmp(whole, chunks, sizeof(whole)) == 0,
          "arbitrary chunks changed samples or dropped decimation phase");
    const double level = rms(whole+100, N/3-100);
    CHECK(level > 2710 && level < 2810, "1k amplitude changed or right channel selected");
    tone(12000, 1000000); pk_airband_pcm16_reset(&a);
    (void)pk_airband_pcm16_process(&a, input, sizeof(input), whole, N/3, 1);
    CHECK(rms(whole+100, N/3-100) < 5, "12k aliased into 4k voice band");
    /* Same high-frequency input above old 4x clip threshold must be filtered
     * before gain, so clipping cannot create in-band distortion. */
    tone(12000, 7000000); pk_airband_pcm16_reset(&a);
    (void)pk_airband_pcm16_process(&a, input, sizeof(input), whole, N/3, 4);
    CHECK(rms(whole+100, N/3-100) < 100, "gain clipped before anti-alias filter");
    /* A pure 12k sine has only {0,+A,0,-A} at 48k: hard clipping it is still
     * a 12k sine. Add a quiet 1k voice to expose pre-FIR clipping damage. */
    for (unsigned i = 0; i < N; i++)
        put24(input+i*6, (int32_t)lround(6000000*sin(6.283185307179586*12000*i/48000) +
                                       1000000*sin(6.283185307179586*1000*i/48000)));
    pk_airband_pcm16_reset(&a);
    (void)pk_airband_pcm16_process(&a, input, sizeof(input), whole, N/3, 4);
    CHECK(rms(whole+100, N/3-100) > 10800 && rms(whole+100, N/3-100) < 11300,
          "strong out-of-band carrier damaged quiet in-band voice before FIR");
}
static void test_reset_stats_and_reject(void)
{
    pk_airband_pcm16_t s = {0}; pk_airband_pcm16_reset(&s);
    for (size_t i = 0; i < N; i++) { put24(input+i*6, 0x7fffff); put24(input+i*6+3, -0x800000); }
    (void)pk_airband_pcm16_process(&s, input, sizeof(input), whole, N/3, 4);
    CHECK(s.stats.input_frames == N && s.stats.output_frames == N/3, "stats frame counts wrong");
    CHECK(s.stats.raw_peak == 8388607 && s.stats.raw_clips == N && s.stats.post_clips > 1500,
          "strong input clips were not counted");
    CHECK(s.stats.pre_peak >= 32767 && s.stats.post_peak == 32767 && s.stats.post_sum_sq > 0,
          "pre/post peak or RMS statistics missing");
    CHECK(whole[N/3-1] == 32767, "post-gain saturated sample wrapped");
    for (size_t i = 0; i < N; i++) put24(input+i*6, -0x800000);
    pk_airband_pcm16_reset(&s);
    (void)pk_airband_pcm16_process(&s, input, sizeof(input), whole, N/3, 4);
    CHECK(whole[N/3-1] == -32768 && s.stats.raw_peak == 8388608 &&
          s.stats.raw_clips == N && s.stats.post_clips > 1500,
          "signed24 negative full scale decoded or saturated incorrectly");
    pk_airband_pcm16_t before = s;
    CHECK(pk_airband_pcm16_process(&s, input, 7, whole, N/3, 4) == 0 &&
          memcmp(&s, &before, sizeof(s)) == 0, "malformed frame modified FIR history");
    CHECK(pk_airband_pcm16_process(&s, input, 18, whole, 0, 4) == 0 &&
          memcmp(&s, &before, sizeof(s)) == 0, "capacity reject modified FIR history");
    pk_airband_pcm16_reset(&s);
    memset(input, 0, sizeof(input));
    (void)pk_airband_pcm16_process(&s, input, sizeof(input), whole, N/3, 4);
    for (size_t i = 0; i < N/3; i++) CHECK(whole[i] == 0, "reset leaked previous station tail");
}
int main(void)
{ test_filter_and_chunks(); test_reset_stats_and_reject(); puts(fail ? "FAILED" : "OK"); return fail != 0; }
