/* Same mounted card keeps last successful capacity through FAT lock timeouts.
 * cc -std=c11 -Wall -Wextra -Werror -O2 -pthread \
 * -I firmware/test/record_stubs -I firmware/test/host_stubs -I firmware/main \
 * -o /tmp/test_sd_info_cache firmware/test/test_sd_info_cache.c \
 * firmware/main/pk_sd_transaction_safety.c \
 * && /tmp/test_sd_info_cache
 */
#include <errno.h>
#include <setjmp.h>
#include <stdio.h>
#include "pk_sdcard.h"
#include "esp_vfs_fat.h"
#include "freertos/task.h"

static esp_err_t info_result = ESP_OK;
static int info_errno;
static bool writes_errno = true;
static uint64_t next_total = 4096, next_free = 2048;
static int failures, info_calls, delays;
static jmp_buf detect_done;
static esp_err_t injected_info(const char *path, uint64_t *total, uint64_t *free_b)
{
    (void)path;
    ++info_calls;
    if (writes_errno) errno = info_errno;
    if (info_result == ESP_OK) { *total = next_total; *free_b = next_free; }
    return info_result;
}
esp_err_t esp_vfs_fat_sdcard_format(const char *path, sdmmc_card_t *card)
{ (void)path; (void)card; return ESP_OK; }
#define esp_vfs_fat_info injected_info
#include "../main/pk_sdcard.c"
#undef esp_vfs_fat_info

#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); ++failures; } } while (0)
static void check_info(bool known, uint64_t total, uint64_t free_b, const char *msg)
{
    uint64_t actual_total = UINT64_MAX, actual_free = UINT64_MAX;
    bool actual_known = pk_sdcard_info(&actual_total, &actual_free);
    CHECK(actual_known == known && (!known ||
          (actual_total == total && actual_free == free_b)), msg);
}
static void one_detect_delay(unsigned ticks)
{ (void)ticks; if (++delays == 2) longjmp(detect_done, 1); }
static void detect_once(void)
{
    delays = 0;
    pk_host_delay_hook = one_detect_delay;
    if (setjmp(detect_done) == 0) pk_host_created_task(NULL);
    pk_host_delay_hook = NULL;
}
int main(void)
{
    /* Timeout before first success remains unknown. */
    info_result = ESP_FAIL; info_errno = ETIMEDOUT;
    pk_sdcard_init();
    check_info(false, 0, 0, "initial timeout must be unknown");
    info_result = ESP_OK; info_errno = 0;
    detect_once();
    check_info(true, 4096, 2048, "background probe publishes successful query");
    info_result = ESP_FAIL; info_errno = ETIMEDOUT;
    int before = info_calls;
    detect_once();
    check_info(true, 4096, 2048, "timeout must retain same-card capacity");
    CHECK(info_calls == before + 1, "background probe must keep retrying");
    info_result = ESP_OK; info_errno = ETIMEDOUT;
    next_total = 8192; next_free = 0;
    detect_once();
    check_info(true, 8192, 0, "success refreshes capacity and real zero free stays known");
    info_result = ESP_FAIL; info_errno = ETIMEDOUT;
    detect_once();
    check_info(true, 8192, 0, "timeout retains real full-card state");
    info_errno = EIO;
    detect_once();
    check_info(false, 0, 0, "non-timeout failure clears known capacity");
    info_errno = ETIMEDOUT;
    detect_once();
    check_info(false, 0, 0, "timeout after invalidating failure stays unknown");
    info_result = ESP_OK; next_free = 4000;
    detect_once();
    check_info(true, 8192, 4000, "success restores known capacity");
    info_result = ESP_ERR_INVALID_STATE; writes_errno = false;
    errno = ETIMEDOUT;
    detect_once();
    check_info(false, 0, 0, "invalid context must not inherit stale timeout errno");
    writes_errno = true; info_result = ESP_OK;
    detect_once();
    check_info(true, 8192, 4000, "success after invalid context restores known capacity");
    sd_unmount_locked();
    check_info(false, 0, 0, "unmount clears cache");
    info_result = ESP_FAIL; info_errno = ETIMEDOUT;
    CHECK(sd_mount_locked(), "replacement card mounts");
    check_info(false, 0, 0, "remount timeout must not reuse old card capacity");
    info_result = ESP_OK; next_total = 16384; next_free = 12000;
    detect_once();
    check_info(true, 16384, 12000, "replacement success publishes new capacity");
    info_result = ESP_FAIL; info_errno = ETIMEDOUT;
    uint32_t generation = pk_sdcard_mount_generation();
    CHECK(pk_sdcard_format() == ESP_OK, "stubbed lifecycle transition succeeds");
    CHECK(pk_sdcard_mount_generation() == generation + 1, "new filesystem increments generation");
    check_info(false, 0, 0, "post-format timeout must not reuse previous filesystem capacity");
    info_result = ESP_OK; next_total = 32768; next_free = 24000;
    detect_once();
    check_info(true, 32768, 24000, "fresh filesystem query succeeds");
    /* Independently exercise the new-mount boundary, including stale cache
     * left by a lifecycle caller. The platform mount is a host-only stub. */
    info_result = ESP_FAIL; info_errno = ETIMEDOUT;
    CHECK(sd_mount_locked(), "stubbed new mount transition succeeds");
    check_info(false, 0, 0, "new mount must clear previous generation capacity");
    printf("%s SD info cache\n", failures ? "FAILED" : "PASS");
    return failures != 0;
}
