/*
 * cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/test/host_stubs/ui_toast \
 *    -I firmware/test/host_stubs -I firmware/main -o /tmp/test_ui_toast \
 *    firmware/test/test_ui_toast.c firmware/main/ui_state.c \
 *    firmware/main/pk_cal_advisor.c && /tmp/test_ui_toast
 */
#include <stdio.h>

#include "ui_state.h"

static int64_t now_us;
static int failures;
int64_t esp_timer_get_time(void) { return now_us; }
void pk_airband_recordings_page_open(void) {}
void pk_airband_recordings_page_leave(void) {}
void pk_config_own_icao_set(uint32_t icao) { (void)icao; }
void pk_config_own_icao_clear(void) {}

#define CHECK(value) do { if (!(value)) { \
    printf("FAIL line %d: %s\n", __LINE__, #value); failures++; \
} } while (0)

static void test_playback_notice_remains_readable_without_refreshing_deadline(void)
{
    now_us = 1000000;
    pk_ui_toast_show(PK_TR_RECORDINGS_PLAYBACK_NOTICE, false);
    pk_tr_id_t text_id = PK_TR_RECORDINGS_READY;
    bool is_error = true;
    now_us = 8000000;
    CHECK(pk_ui_toast_get(&text_id, &is_error));
    CHECK(text_id == PK_TR_RECORDINGS_PLAYBACK_NOTICE);
    CHECK(!is_error);
    CHECK(pk_ui_toast_blink_visible());
    now_us = 8999999;
    CHECK(pk_ui_toast_get(NULL, NULL));
    now_us = 9000000;
    CHECK(!pk_ui_toast_get(NULL, NULL));
}

static void test_other_toasts_keep_their_short_deadline(void)
{
    now_us = 10000000;
    pk_ui_toast_show(PK_TR_RECORDINGS_FILE_CHANGED, true);
    now_us = 11499999;
    CHECK(pk_ui_toast_get(NULL, NULL));
    now_us = 11500000;
    CHECK(!pk_ui_toast_get(NULL, NULL));
}

static void test_blinking_toasts_keep_their_complete_cycle(void)
{
    now_us = 20000000;
    pk_ui_toast_show_blink(PK_TR_RECORDINGS_FILE_CHANGED, true, 3);
    now_us = 20400000;
    CHECK(pk_ui_toast_get(NULL, NULL));
    CHECK(!pk_ui_toast_blink_visible());
    now_us = 22399999;
    CHECK(pk_ui_toast_get(NULL, NULL));
    now_us = 22400000;
    CHECK(!pk_ui_toast_get(NULL, NULL));
}

int main(void)
{
    CHECK(pk_ui_init() == ESP_OK);
    test_playback_notice_remains_readable_without_refreshing_deadline();
    test_other_toasts_keep_their_short_deadline();
    test_blinking_toasts_keep_their_complete_cycle();
    if (failures) return 1;
    puts("PASS UI toast timing");
    return 0;
}
