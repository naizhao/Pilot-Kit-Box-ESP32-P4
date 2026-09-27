/* AIRBAND 诊断详情的产品可达控制闭环：
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 *      -DPK_DIAG_PAGE_HOST_TEST -o /tmp/test_diag_airband_controls \
 *      firmware/test/test_diag_airband_controls.c firmware/main/diag_page.c \
 *      && /tmp/test_diag_airband_controls
 */
#include "diag_page.h"

#include <stdio.h>

static int fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); fail++; } } while (0)

static void test_airband_card_reaches_detail(void)
{
    CHECK(PK_DIAG_CARD_N == 15, "card count=%d\n", PK_DIAG_CARD_N);
    CHECK(pk_diag_card_index_for_cell(7, 0) == PK_DIAG_CARD_AIRBAND,
          "row 7 col 0 does not reach AIRBAND detail\n");
    CHECK(pk_diag_card_index_for_cell(7, 1) == -1,
          "empty row 7 col 1 incorrectly reaches a detail\n");
    CHECK(pk_diag_card_index_for_cell(-1, 0) == -1 &&
          pk_diag_card_index_for_cell(0, 2) == -1,
          "out-of-grid cell accepted\n");
}

static void test_airband_control_hit_map(void)
{
    const int left = PK_DIAG_AIRBAND_BTN_X0 + PK_DIAG_AIRBAND_BTN_W / 2;
    const int right = PK_DIAG_AIRBAND_BTN_X0 + PK_DIAG_AIRBAND_BTN_W +
                      PK_DIAG_AIRBAND_BTN_GAP + PK_DIAG_AIRBAND_BTN_W / 2;
    const int row4 = 4 * PK_DIAG_DETAIL_LINE_H + 16;
    const int row5 = 5 * PK_DIAG_DETAIL_LINE_H + 16;
    const int row6 = 6 * PK_DIAG_DETAIL_LINE_H + 16;
    const int row7 = 7 * PK_DIAG_DETAIL_LINE_H + 16;
    CHECK(pk_diag_airband_action_at(left, row4) ==
              PK_DIAG_AIRBAND_ACT_FREQ_GUARD,
          "left frequency button mismatch\n");
    CHECK(pk_diag_airband_action_at(right, row4) ==
              PK_DIAG_AIRBAND_ACT_FREQ_AIR_AIR,
          "right frequency button mismatch\n");
    CHECK(pk_diag_airband_action_at(left, row5) ==
              PK_DIAG_AIRBAND_ACT_SCAN_TOGGLE,
          "scan button mismatch\n");
    CHECK(pk_diag_airband_action_at(right, row5) ==
              PK_DIAG_AIRBAND_ACT_SQUELCH_TOGGLE,
          "squelch toggle mismatch\n");
    CHECK(pk_diag_airband_action_at(left, row6) ==
              PK_DIAG_AIRBAND_ACT_SQUELCH_DEC &&
          pk_diag_airband_action_at(right, row6) ==
              PK_DIAG_AIRBAND_ACT_SQUELCH_INC,
          "squelch threshold buttons mismatch\n");
    CHECK(pk_diag_airband_action_at(left, row7) ==
              PK_DIAG_AIRBAND_ACT_SPEAKER_TEST,
          "speaker test button mismatch\n");
    CHECK(pk_diag_airband_action_at(right, row7) ==
              PK_DIAG_AIRBAND_ACT_NONE,
          "unused speaker row right half accepted\n");
    CHECK(pk_diag_airband_action_at(PK_DIAG_AIRBAND_BTN_X0 - 1, row4) ==
              PK_DIAG_AIRBAND_ACT_NONE &&
          pk_diag_airband_action_at(left, 3 * PK_DIAG_DETAIL_LINE_H + 16) ==
              PK_DIAG_AIRBAND_ACT_NONE &&
          pk_diag_airband_action_at(left, 4 * PK_DIAG_DETAIL_LINE_H + 35) ==
              PK_DIAG_AIRBAND_ACT_NONE,
          "outside control area accepted\n");
}

static pk_diag_airband_command_t make(pk_diag_airband_action_t action,
                                      bool scanning, bool squelch,
                                      int16_t threshold)
{
    const pk_diag_airband_control_state_t state = {
        .scanning = scanning,
        .squelch_enabled = squelch,
        .squelch_open_dbm_x2 = threshold,
    };
    pk_diag_airband_command_t cmd = {0};
    CHECK(pk_diag_airband_make_command(action, &state, &cmd),
          "action %d did not produce command\n", action);
    return cmd;
}

static void test_speaker_test_is_local_only(void)
{
    const pk_diag_airband_command_t c =
        make(PK_DIAG_AIRBAND_ACT_SPEAKER_TEST, false, true, -180);
    CHECK(c.kind == PK_DIAG_AIRBAND_CMD_SPEAKER_TEST,
          "speaker test command=%d\n", c.kind);
    CHECK(c.frequency_hz == 0 && c.scan_start_hz == 0 &&
          c.scan_end_hz == 0 && c.scan_step_hz == 0 &&
          c.scan_dwell_ms == 0,
          "speaker test leaked RF/scan parameters\n");
}

static void test_frequency_and_scan_commands(void)
{
    pk_diag_airband_command_t c =
        make(PK_DIAG_AIRBAND_ACT_FREQ_GUARD, true, true, -180);
    CHECK(c.kind == PK_DIAG_AIRBAND_CMD_SET_FREQUENCY &&
          c.frequency_hz == 121500000u,
          "guard frequency command=%d/%lu\n", c.kind,
          (unsigned long)c.frequency_hz);
    c = make(PK_DIAG_AIRBAND_ACT_FREQ_AIR_AIR, false, true, -180);
    CHECK(c.kind == PK_DIAG_AIRBAND_CMD_SET_FREQUENCY &&
          c.frequency_hz == 123450000u,
          "air-air frequency command=%d/%lu\n", c.kind,
          (unsigned long)c.frequency_hz);

    c = make(PK_DIAG_AIRBAND_ACT_SCAN_TOGGLE, false, true, -180);
    CHECK(c.kind == PK_DIAG_AIRBAND_CMD_START_SCAN &&
          c.scan_start_hz == 118000000u && c.scan_end_hz == 137000000u &&
          c.scan_step_hz == 25000u && c.scan_dwell_ms == 250u,
          "scan start plan is not 118-137 MHz RX-only plan\n");
    c = make(PK_DIAG_AIRBAND_ACT_SCAN_TOGGLE, true, true, -180);
    CHECK(c.kind == PK_DIAG_AIRBAND_CMD_STOP_SCAN,
          "running scan did not map to stop\n");
}

static void test_squelch_commands(void)
{
    pk_diag_airband_command_t c =
        make(PK_DIAG_AIRBAND_ACT_SQUELCH_TOGGLE, false, true, -180);
    CHECK(c.kind == PK_DIAG_AIRBAND_CMD_SET_SQUELCH &&
          !c.squelch_enabled && c.squelch_open_dbm_x2 == -180,
          "squelch off command wrong\n");
    c = make(PK_DIAG_AIRBAND_ACT_SQUELCH_TOGGLE, false, false, -180);
    CHECK(c.squelch_enabled, "squelch on command wrong\n");
    c = make(PK_DIAG_AIRBAND_ACT_SQUELCH_DEC, false, true, -180);
    CHECK(c.squelch_open_dbm_x2 == -184,
          "squelch decrement=%d\n", c.squelch_open_dbm_x2);
    c = make(PK_DIAG_AIRBAND_ACT_SQUELCH_INC, false, true, -180);
    CHECK(c.squelch_open_dbm_x2 == -176,
          "squelch increment=%d\n", c.squelch_open_dbm_x2);
    c = make(PK_DIAG_AIRBAND_ACT_SQUELCH_DEC, false, true, -240);
    CHECK(c.squelch_open_dbm_x2 == -240, "lower clamp failed\n");
    c = make(PK_DIAG_AIRBAND_ACT_SQUELCH_INC, false, true, -80);
    CHECK(c.squelch_open_dbm_x2 == -80, "upper clamp failed\n");
}

int main(void)
{
    test_airband_card_reaches_detail();
    test_airband_control_hit_map();
    test_frequency_and_scan_commands();
    test_squelch_commands();
    test_speaker_test_is_local_only();
    puts(fail ? "FAILED" : "OK");
    return fail ? 1 : 0;
}
