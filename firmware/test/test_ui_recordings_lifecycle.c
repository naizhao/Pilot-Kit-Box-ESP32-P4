/*
 * cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/test/host_stubs/ui_toast \
 *    -I firmware/test/host_stubs -I firmware/main -o /tmp/test_ui_recordings_lifecycle \
 *    firmware/test/test_ui_recordings_lifecycle.c firmware/main/ui_state.c \
 *    firmware/main/pk_cal_advisor.c && /tmp/test_ui_recordings_lifecycle
 */
#include <stdio.h>
#include "ui_state.h"
#include "pk_cal_advisor.h"

static int failures;
static int64_t now_us;
static unsigned opens, leaves;
static bool catalog_visible;
int64_t esp_timer_get_time(void) { return now_us; }
void pk_airband_recordings_page_open(void)
{ opens++; catalog_visible = true; }
void pk_airband_recordings_page_leave(void)
{ leaves++; catalog_visible = false; }
void pk_config_own_icao_set(uint32_t value) { (void)value; }
void pk_config_own_icao_clear(void) {}
#define CHECK(c, message) do { if (!(c)) { \
    fprintf(stderr, "FAIL: %s (%d)\n", message, __LINE__); failures++; \
} } while (0)

static void reset_ui(pk_ui_mode_t mode)
{
    pk_ui_set_mode(PK_UI_MODE_PFD);
    CHECK(pk_ui_init() == ESP_OK, "UI state initialized");
    now_us = 0;
    opens = leaves = 0;
    catalog_visible = false;
    if (mode == PK_UI_MODE_AIRBAND_RECORDINGS)
        pk_airband_recordings_page_open(); /* actual nav-grid activation order */
    pk_ui_set_mode(mode);
}

static void tick(uint32_t ms, uint8_t accuracy)
{
    now_us = (int64_t)ms * 1000;
    pk_ui_cal_wizard_tick(true, accuracy, PK_PHASE_GROUND_STOPPED, 125);
}

static void auto_enter(void)
{
    tick(1, 0);
    tick(1 + PK_CAL_COLDSTART_GRACE_MS, 0);
    CHECK(pk_ui_get_mode() == PK_UI_MODE_CAL_WIZARD,
          "real calibration advisor automatically enters wizard");
}

static void test_auto_return_reopens_recordings_catalog(void)
{
    reset_ui(PK_UI_MODE_AIRBAND_RECORDINGS);
    auto_enter();
    CHECK(leaves == 1 && !catalog_visible,
          "auto calibration entry closes recordings catalog");
    tick(2 + PK_CAL_COLDSTART_GRACE_MS, PK_CAL_EXIT_ACCURACY);
    tick(2 + PK_CAL_COLDSTART_GRACE_MS + PK_CAL_EXIT_MS,
         PK_CAL_EXIT_ACCURACY);
    CHECK(pk_ui_get_mode() == PK_UI_MODE_AIRBAND_RECORDINGS,
          "fusion convergence restores recordings mode");
    CHECK(opens == 2 && catalog_visible,
          "automatic restoration reopens recordings for catalog refresh");
}

static void test_dismiss_reopens_recordings_catalog(void)
{
    reset_ui(PK_UI_MODE_AIRBAND_RECORDINGS);
    auto_enter();
    pk_ui_cal_wizard_dismiss();
    CHECK(pk_ui_get_mode() == PK_UI_MODE_AIRBAND_RECORDINGS,
          "wizard dismissal restores recordings mode");
    CHECK(opens == 2 && leaves == 1 && catalog_visible,
          "manual dismissal reopens the previously closed recordings catalog");
}

static void test_user_calibration_entry_closes_recordings_catalog(void)
{
    reset_ui(PK_UI_MODE_AIRBAND_RECORDINGS);
    pk_ui_cal_wizard_enter();
    CHECK(pk_ui_get_mode() == PK_UI_MODE_CAL_WIZARD,
          "explicit calibration entry changes mode");
    CHECK(leaves == 1 && !catalog_visible,
          "explicit calibration entry also closes the hidden recordings catalog");
    tick(1, PK_CAL_EXIT_ACCURACY);
    tick(1 + PK_CAL_EXIT_MS, PK_CAL_EXIT_ACCURACY);
    CHECK(pk_ui_get_mode() == PK_UI_MODE_CAL_WIZARD,
          "explicit calibration retains the existing no-auto-exit behavior");
    pk_ui_cal_wizard_dismiss();
    CHECK(opens == 2 && catalog_visible,
          "explicit calibration dismissal reopens recordings");
}

static void test_other_pages_do_not_open_recordings(void)
{
    reset_ui(PK_UI_MODE_MAP);
    auto_enter();
    pk_ui_cal_wizard_dismiss();
    CHECK(pk_ui_get_mode() == PK_UI_MODE_MAP && opens == 0 && leaves == 0,
          "returning to map never opens recordings catalog");
    reset_ui(PK_UI_MODE_MAP);
    auto_enter();
    tick(2 + PK_CAL_COLDSTART_GRACE_MS, PK_CAL_EXIT_ACCURACY);
    tick(2 + PK_CAL_COLDSTART_GRACE_MS + PK_CAL_EXIT_MS,
         PK_CAL_EXIT_ACCURACY);
    CHECK(pk_ui_get_mode() == PK_UI_MODE_MAP && opens == 0 && leaves == 0,
          "automatic return to map never opens recordings catalog");
}

int main(void)
{
    test_auto_return_reopens_recordings_catalog();
    test_dismiss_reopens_recordings_catalog();
    test_user_calibration_entry_closes_recordings_catalog();
    test_other_pages_do_not_open_recordings();
    if (failures) return 1;
    puts("UI recordings lifecycle tests PASS");
    return 0;
}
