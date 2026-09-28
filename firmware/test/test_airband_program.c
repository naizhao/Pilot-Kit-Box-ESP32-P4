/*
 * cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 * -o /tmp/test_airband_program firmware/test/test_airband_program.c \
 * firmware/main/airband_audio.c -lm && /tmp/test_airband_program
 */
#include "airband_audio.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
static int fail;
#define CHECK(c, m) do { if (!(c)) { puts("FAIL: " m); fail++; } } while (0)
static void test_mute_and_rate(void)
{
    int16_t live[1600], muted[1600], out[9600] = {0}, silent[9600] = {0};
    for (unsigned i = 0; i < 1600; i++) live[i] = muted[i] = lround(10000*sin(6.283185307179586*i/16));
    pk_airband_audio_route_t a = {0}, b = {0};
    pk_airband_audio_route_reset(&a); pk_airband_audio_route_reset(&b);
    CHECK(pk_airband_audio_route16(&a, live, 1600, true, false, out, 4800) == 4800,
          "16k program was sent at 48k without interpolation (3x playback speed)");
    CHECK(pk_airband_audio_route16(&b, muted, 1600, true, true, silent, 4800) == 4800,
          "muted speaker branch changed duration");
    CHECK(memcmp(live, muted, sizeof(live)) == 0 && live[1596] == -10000,
          "speaker mute changed common record/BLE program");
    for (size_t i = 0; i < 9600; i++) CHECK(silent[i] == 0, "speaker mute output was audible");
    int crossings = 0;
    for (size_t i = 480; i < 4800; i++) {
        CHECK(out[i*2] == out[i*2+1], "speaker stereo duplication differs");
        if (out[(i-1)*2] <= 0 && out[i*2] > 0) crossings++;
    }
    CHECK(crossings == 90, "1kHz tone frequency changed during 16->48 resampling");
    pk_airband_audio_route_reset(&a);
    memset(live, 0, sizeof(live));
    (void)pk_airband_audio_route16(&a, live, 1600, false, false, out, 4800);
    for (size_t i = 0; i < 9600; i++) CHECK(out[i] == 0, "stream reset leaked old interpolated audio");
}
static void test_continuity_and_reject(void)
{
    int16_t p[128], q[128], a[768] = {0}, b[768] = {0};
    for (unsigned i = 0; i < 128; i++) p[i] = q[i] = (int16_t)(i*300-18000);
    pk_airband_audio_route_t x = {0}, y = {0};
    pk_airband_audio_route_reset(&x); pk_airband_audio_route_reset(&y);
    (void)pk_airband_audio_route16(&x, p, 128, true, false, a, 384);
    size_t n = pk_airband_audio_route16(&y, q, 13, true, false, b, 39);
    n += pk_airband_audio_route16(&y, q+13, 115, true, false, b+n*2, 345);
    CHECK(n == 384 && memcmp(a,b,sizeof(a)) == 0 && memcmp(p,q,sizeof(p)) == 0,
          "resampler/ramp is not continuous across blocks");
    const pk_airband_audio_route_t before = x;
    const int16_t first = p[0];
    CHECK(pk_airband_audio_route16(&x, p, 128, false, false, a, 383) == 0 &&
          memcmp(&x,&before,sizeof(x)) == 0 && p[0] == first,
          "capacity error partially modified route state/program");
}
int main(void)
{ test_mute_and_rate(); test_continuity_and_reject(); puts(fail ? "FAILED" : "OK"); return fail != 0; }
