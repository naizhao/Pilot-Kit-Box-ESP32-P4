/*
 * cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 * -o /tmp/test_airband_am_gain firmware/test/test_airband_am_gain.c \
 * firmware/main/airband_am_gain.c firmware/main/airband_bk4819.c && /tmp/test_airband_am_gain
 */
#include "airband_am_gain.h"
#include "airband_bk4819.h"
#include <stdio.h>

static int fail;
#define CHECK(c, m) do { if (!(c)) { puts("FAIL: " m); fail++; } } while (0)

static void confirm(pk_airband_am_gain_t *s, uint64_t now)
{
    pk_airband_am_gain_commit(s, s->requested_index, now);
}

static void test_attack_and_release(void)
{
    pk_airband_am_gain_t s = {0};
    pk_airband_am_gain_reset(&s, PK_AIRBAND_AM_GAIN_MAX_INDEX, 0);
    CHECK(pk_airband_am_gain_update(&s, true, 143, 10), "0.5dB over target must attack");
    CHECK(s.requested_index == PK_AIRBAND_AM_GAIN_MAX_INDEX - 1, "small overrun must drop one step");
    CHECK(s.applied_index == PK_AIRBAND_AM_GAIN_MAX_INDEX, "request committed before readback");
    confirm(&s, 10);
    CHECK(pk_airband_am_gain_update(&s, true, 182, 20), "strong peak delayed by hold/filter");
    CHECK(pk_airband_am_gain_db(s.requested_index) <= -29, "20dB overrun did not skip steps");
    confirm(&s, 20);
    const unsigned low = s.applied_index;
    for (uint64_t t = 30; t <= 320; t += 10)
        CHECK(!pk_airband_am_gain_update(&s, true, 129, t), "released before 300ms continuous low");
    CHECK(pk_airband_am_gain_update(&s, true, 129, 330), "missing release at low-duration boundary");
    CHECK(s.requested_index == low + 1, "release must increase one step");
    confirm(&s, 330);
    for (uint64_t t = 340; t < 430; t += 10)
        CHECK(!pk_airband_am_gain_update(&s, true, 129, t), "release exceeded one step per 100ms");
    CHECK(pk_airband_am_gain_update(&s, true, 129, 430), "100ms release boundary missed");
    confirm(&s, 430);
    CHECK(!pk_airband_am_gain_update(&s, true, 130, 440), "6dB boundary should hold");
    CHECK(!s.low_tracking, "6dB boundary did not cancel low dwell");
    CHECK(!pk_airband_am_gain_update(&s, true, 129, 450), "hysteresis did not restart low duration");
    CHECK(!pk_airband_am_gain_update(&s, true, 129, 10000), "scheduler gap counted as continuous low");
    CHECK(!pk_airband_am_gain_update(&s, false, 511, 10010), "invalid RF triggered an attack");
    CHECK(!s.low_tracking, "RF invalid retained release timer");
    pk_airband_am_gain_reset(&s, PK_AIRBAND_AM_GAIN_MAX_INDEX, 10020);
    CHECK(s.applied_index == PK_AIRBAND_AM_GAIN_MAX_INDEX && !s.low_tracking,
          "reset retained previous station state");
    CHECK(!pk_airband_am_gain_update(&s, true, 0, 10010), "backwards time accepted");
}

static void test_table_and_reference(void)
{
    CHECK(pk_airband_am_gain_reg13(PK_AIRBAND_AM_GAIN_MAX_INDEX) == 0x03BE,
          "maximum gain exceeds existing safe default");
    for (unsigned i = 1; i <= PK_AIRBAND_AM_GAIN_MAX_INDEX; i++)
        CHECK(pk_airband_am_gain_db(i) > pk_airband_am_gain_db(i - 1), "gain table not monotonic");
    const int16_t reference = pk_airband_am_reference_rssi_x2(110, 31);
    CHECK(reference == -178, "reference correction does not recover -89dBm at -23dB gain");
    CHECK(pk_airband_squelch_gate(true, true, reference, -180, 12),
          "manual gain reduction falsely closed squelch on strong station");
}

static void test_invalid_clears_observations_and_extremes(void)
{
    pk_airband_am_gain_t s = {0};
    pk_airband_am_gain_reset(&s, 10, 0);
    for (uint64_t t = 10; t <= 290; t += 10)
        (void)pk_airband_am_gain_update(&s, true, 0, t);
    CHECK(!pk_airband_am_gain_update(&s, false, 0, 300) && !s.low_tracking,
          "invalid sample retained a nearly expired low timer");
    for (uint64_t t = 310; t < 610; t += 10)
        CHECK(!pk_airband_am_gain_update(&s, true, 0, t), "release reused pre-invalid low time");
    CHECK(pk_airband_am_gain_update(&s, true, 0, 610) && s.requested_index == 11,
          "release did not recover after valid low dwell");
    pk_airband_am_gain_reset(&s, 0, 1000);
    CHECK(!pk_airband_am_gain_update(&s, true, 511, 1010) && s.requested_index == 0,
          "attack underflowed minimum gain");
    pk_airband_am_gain_reset(&s, PK_AIRBAND_AM_GAIN_MAX_INDEX, 2000);
    for (uint64_t t = 2010; t <= 3000; t += 10)
        CHECK(!pk_airband_am_gain_update(&s, true, 0, t), "release exceeded maximum safe gain");
}

typedef struct { uint16_t regs[128]; int writes; bool fail_write; bool fail_read; bool mismatch; } bus_t;
static bool read_reg(void *context, uint8_t reg, uint16_t *value)
{
    bus_t *b = context;
    if (b->fail_read) return false;
    *value = b->regs[reg];
    if (b->mismatch && b->writes && reg == 0x13) *value ^= 1;
    return true;
}
static bool write_reg(void *context, uint8_t reg, uint16_t value)
{
    bus_t *b = context;
    if (b->fail_write) return false;
    CHECK(reg == 0x13 || reg == 0x7e, "gain transaction touched a non-gain/TX register");
    b->writes++;
    b->regs[reg] = value;
    return true;
}
static void test_fixed_gain_transaction(void)
{
    bus_t b = {.regs = {[0x30] = 0xBFF1, [0x7e] = 0x2A5C, [0x13] = 0xABBE}};
    const pk_airband_bk_io_t io = {&b, read_reg, write_reg};
    pk_airband_bk_gain_diag_t d = {0};
    CHECK(pk_airband_bk_apply_gain(&io, 0x035E, &d), "fixed gain write failed");
    CHECK(b.regs[0x7e] == 0xBA5C, "REG7E not fixed REG13 or clobbered low 12 bits");
    CHECK(b.regs[0x13] == 0xAB5E, "REG13 clobbered upper six bits");
    CHECK(d.reg7e == 0xBA5C && d.reg13 == 0xAB5E && d.changes == 1, "diagnostic not actual readback");
    b.fail_write = true;
    CHECK(!pk_airband_bk_apply_gain(&io, 0x037F, &d) && d.write_errors == 1, "write error swallowed");
    b.fail_write = false; b.fail_read = true;
    CHECK(!pk_airband_bk_apply_gain(&io, 0x037F, &d) && d.read_errors == 1, "read error swallowed");
    b.fail_read = false; b.mismatch = true;
    CHECK(!pk_airband_bk_apply_gain(&io, 0x037F, &d) && d.read_errors == 2 && d.changes == 1,
          "readback mismatch committed applied gain");
    b.mismatch = false; b.regs[0x30] = 0;
    const int before = b.writes;
    CHECK(!pk_airband_bk_apply_gain(&io, 0x037F, &d) && b.writes == before, "RF-off gain transaction wrote registers");
}

int main(void)
{
    test_attack_and_release();
    test_table_and_reference();
    test_fixed_gain_transaction();
    test_invalid_clears_observations_and_extremes();
    puts(fail ? "FAILED" : "OK");
    return fail != 0;
}
