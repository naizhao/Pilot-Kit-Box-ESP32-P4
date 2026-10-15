/* Capacity unknown is a write-policy fallback, not a user space alert.
 * cc -std=c11 -Wall -Wextra -Werror -O2 -pthread -D_DEFAULT_SOURCE \
 * -I firmware/test/record_stubs -I firmware/test/host_stubs -I firmware/main \
 * -I sim/compat -o /tmp/test_rec_store_capacity_alerts \
 * firmware/test/test_rec_store_capacity_alerts.c firmware/main/pk_rec_store.c \
 * firmware/main/pk_rec_format.c firmware/main/pk_rec_idx.c \
 * && /tmp/test_rec_store_capacity_alerts
 */
#include <stdio.h>
#include <string.h>
#include "pk_sdcard.h"
#include "ui_state.h"
#include "esp_app_desc.h"

static bool mounted = true, capacity_known;
static uint64_t available;
static int failures, toasts, info_queries;
static pk_tr_id_t last_toast;
bool pk_sdcard_is_mounted(void) { return mounted; }
bool pk_sdcard_info(uint64_t *total, uint64_t *free_b)
{
    ++info_queries;
    if (!capacity_known) return false;
    *total = 4096ull * 1024 * 1024; *free_b = available;
    return true;
}
const char *pk_sdcard_mount_point(void) { return "/unused-capacity-test"; }
bool pk_sdcard_io_acquire(void) { return mounted; }
void pk_sdcard_io_release(void) {}
bool pk_sdcard_register_pre_unmount_cb(void (*cb)(void)) { (void)cb; return true; }
const char *pk_ble_device_name(void) { return "capacity-test"; }
bool pk_clock_is_synced(void) { return false; }
int64_t esp_timer_get_time(void) { return 1000000; }
const esp_app_desc_t *esp_app_get_description(void) { return NULL; }
void pk_ui_toast_show_blink(pk_tr_id_t id, bool warning, int cycles)
{ (void)warning; (void)cycles; ++toasts; last_toast = id; }
#include "../main/pk_rec_store_fs.c"

#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)
static void check_tier(pk_rec_degrade_t want, const char *msg)
{
    int before = info_queries;
    CHECK(degrade_tier_check_locked() == want, msg);
    CHECK(info_queries == before + 1, "policy and alert must share one capacity observation");
}
static void check_health(pk_rec_degrade_t want, const char *msg)
{
    pk_rec_store_health_t health;
    pk_rec_store_get_health(&health);
    CHECK(health.tier == want, msg);
}
static void test_health_unknown_after_observed_recovery(void)
{
    capacity_known = true; available = 100ull * 1024 * 1024;
    check_tier(PK_REC_DEGRADE_OWN_ONLY, "establish known low space before read-only recovery");
    const int previous_toasts = toasts;
    available = 1000ull * 1024 * 1024;
    check_health(PK_REC_DEGRADE_FULL, "health observes real space recovery without append");
    CHECK(s_last_tier == PK_REC_DEGRADE_OWN_ONLY,
          "read-only health must not mutate space-toast deduplication state");
    capacity_known = false;
    check_health(PK_REC_DEGRADE_FULL,
                 "unknown after observed recovery must not regress to obsolete low-space state");
    CHECK(toasts == previous_toasts, "read-only recovery and unknown health must not issue toast");
}

static void test_new_card_unknown_does_not_inherit_low_space(void)
{
    capacity_known = true; available = 100ull * 1024 * 1024;
    check_tier(PK_REC_DEGRADE_OWN_ONLY, "establish real low space on card A");
    const int previous_toasts = toasts;
    mounted = false; capacity_known = false;
    pk_rec_store_pre_unmount();
    CHECK(s_last_tier == PK_REC_DEGRADE_OWN_ONLY,
          "real lifecycle callback keeps alert deduplication history independent of capacity");
    check_health(PK_REC_DEGRADE_FULL, "removed card has no low-space presentation");
    mounted = true;
    check_health(PK_REC_DEGRADE_FULL,
                 "new card with unknown capacity must not inherit old card low-space state");
    check_tier(PK_REC_DEGRADE_OWN_ONLY, "new card unknown still uses conservative write policy");
    CHECK(toasts == previous_toasts, "new card unknown must not issue space toast");
}

int main(int argc, char **argv)
{
    const uint64_t mib = 1024ull * 1024;
    s_lock = xSemaphoreCreateMutex();
    if (argc == 2 && strcmp(argv[1], "--health-recovery") == 0) {
        test_health_unknown_after_observed_recovery();
        return failures != 0;
    }
    if (argc == 2 && strcmp(argv[1], "--health-new-card") == 0) {
        test_new_card_unknown_does_not_inherit_low_space();
        return failures != 0;
    }
    check_tier(PK_REC_DEGRADE_OWN_ONLY, "unknown capacity keeps conservative write policy");
    CHECK(toasts == 0 && s_last_tier == PK_REC_DEGRADE_FULL,
          "initial unknown capacity must not alert or change known space state");
    check_health(PK_REC_DEGRADE_FULL, "unknown capacity must not invent diagnostic low-space state");
    CHECK(!pk_rec_store_append_adsb_line("test\n", 5), "unknown capacity must suppress raw writing");
    CHECK(toasts == 0, "unknown raw append must not issue space toast");
    capacity_known = true; available = 500 * mib;
    check_tier(PK_REC_DEGRADE_NO_RAW, "known 500 MiB retains existing no-raw boundary");
    CHECK(toasts == 1 && last_toast == PK_TR_TOAST_REC_SD_NORAW,
          "first real low-space transition must alert even after unknown capacity");
    check_tier(PK_REC_DEGRADE_NO_RAW, "same known tier stays stable");
    CHECK(toasts == 1, "known stable tier must not repeat toast");
    capacity_known = false;
    check_tier(PK_REC_DEGRADE_OWN_ONLY, "unknown after no-raw stays conservative");
    CHECK(toasts == 1 && s_last_tier == PK_REC_DEGRADE_NO_RAW,
          "unknown must not change remembered low-space state");
    check_health(PK_REC_DEGRADE_FULL, "unknown must not infer current capacity from toast history");
    capacity_known = true; available = 100 * mib;
    check_tier(PK_REC_DEGRADE_OWN_ONLY, "known 100 MiB retains own-only boundary");
    CHECK(toasts == 2 && last_toast == PK_TR_TOAST_REC_SD_OWNONLY,
          "known severe low space must alert");
    check_health(PK_REC_DEGRADE_OWN_ONLY, "known severe space remains visible in health");
    available = 501 * mib;
    check_tier(PK_REC_DEGRADE_FULL, "space recovery restores full writing");
    CHECK(toasts == 2, "space recovery does not alert");
    available = 0;
    check_tier(PK_REC_DEGRADE_OWN_ONLY, "real zero free is severe known space");
    CHECK(toasts == 3 && last_toast == PK_TR_TOAST_REC_SD_OWNONLY,
          "real full card must alert");
    test_health_unknown_after_observed_recovery();
    test_new_card_unknown_does_not_inherit_low_space();
    mounted = false; capacity_known = false;
    pk_rec_store_pre_unmount();
    check_tier(PK_REC_DEGRADE_OWN_ONLY, "no-card write policy stays conservative");
    CHECK(toasts == 3, "no card must not become space toast");
    check_health(PK_REC_DEGRADE_FULL, "no card uses existing dedicated missing-card presentation");
    for (uint32_t i = 1; i < PK_REC_STORE_FAIL_THRESHOLD; ++i)
        sink_write_result_locked(REC_SINK_OWN_TRK, false);
    CHECK(toasts == 3 && !s_sink_disabled[REC_SINK_OWN_TRK], "transient writes stay below sink failure threshold");
    sink_write_result_locked(REC_SINK_OWN_TRK, false);
    CHECK(toasts == 4 && last_toast == PK_TR_TOAST_REC_SINK_FAIL && s_sink_disabled[REC_SINK_OWN_TRK],
          "persistent sink write failure remains visible");
    sink_write_result_locked(REC_SINK_OWN_TRK, false);
    CHECK(toasts == 4, "disabled sink does not repeat alert");
    printf("%s rec store capacity alerts\n", failures ? "FAILED" : "PASS");
    return failures != 0;
}
