/*
 * cc -std=c11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE \
 *   -DPK_AIRBAND_RECORDING_SERVICE_TEST_HOOKS \
 *   -Ifirmware/test/record_stubs -Ifirmware/test/host_stubs -Ifirmware/main \
 *   -o /tmp/test_airband_recording_service \
 *   firmware/test/test_airband_recording_service.c \
 *   firmware/main/airband_recording_service.c \
 *   firmware/main/airband_recording_store.c \
 *   firmware/main/airband_recording_catalog.c \
 *   firmware/main/airband_wav_reader.c -pthread && \
 *   /tmp/test_airband_recording_service
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "airband_recording_store.h"
#include "pk_sdcard.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%d)\n", m, __LINE__); failures++; } } while (0)
static char root[128];
static bool mounted = true;
static uint32_t generation = 3;
static unsigned leases;
static unsigned acquisitions;
static bool request_during_acquire;
static int64_t now_us;
int64_t esp_timer_get_time(void) { return now_us; }

bool pk_sdcard_is_mounted(void) { return mounted; }
uint32_t pk_sdcard_mount_generation(void) { return generation; }
const char *pk_sdcard_mount_point(void) { return root; }
bool pk_sdcard_io_acquire(void)
{
    if (!mounted) return false;
    leases++; acquisitions++;
    if (request_during_acquire) {
        request_during_acquire = false;
        pk_airband_recording_store_request_refresh();
    }
    return true;
}
void pk_sdcard_io_release(void)
{ CHECK(leases > 0, "lease underflow"); leases--; }

static void put16(unsigned char *p, unsigned v)
{ p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void write_wav(unsigned session)
{
    char path[320];
    snprintf(path, sizeof(path),
             "%s/airband/s%010u-c121500-f121500000-0000000001.wav",
             root, session);
    unsigned char h[44] = {0};
    memcpy(h, "RIFF", 4); put32(h + 4, 36);
    memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
    put16(h + 20, 1); put16(h + 22, 1); put32(h + 24, 16000);
    put32(h + 28, 32000); put16(h + 32, 2); put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    FILE *fp = fopen(path, "wb"); CHECK(fp != NULL, "create service fixture");
    if (fp) { CHECK(fwrite(h, 1, sizeof(h), fp) == sizeof(h), "write service fixture"); fclose(fp); }
}

int main(void)
{
    strcpy(root, "/tmp/pk_airband_service_XXXXXX");
    CHECK(mkdtemp(root) != NULL, "temporary service root");
    char dir[180]; snprintf(dir, sizeof(dir), "%s/airband", root);
    CHECK(mkdir(dir, 0775) == 0, "airband directory");
    write_wav(1);

    pk_airband_recording_snapshot_t not_started = {0};
    CHECK(pk_airband_recording_store_snapshot_acquire_ex(&not_started) ==
              PK_AIRBAND_RECORDING_SNAPSHOT_NOT_STARTED,
          "catalog distinguishes a service that was never started");

    pk_airband_recording_store_test_set_start_failure(true);
    CHECK(!pk_airband_recording_store_start(),
          "injected catalog worker creation failure is reported");
    pk_airband_recording_snapshot_t start_failed = {0};
    CHECK(pk_airband_recording_store_snapshot_acquire_ex(&start_failed) ==
              PK_AIRBAND_RECORDING_SNAPSHOT_START_FAILED,
          "catalog worker creation failure is not reported as scanning or OOM");
    pk_airband_recording_store_test_set_start_failure(false);
    CHECK(pk_airband_recording_store_start(), "catalog service starts");
    pk_airband_recording_store_test_poll();
    CHECK(acquisitions == 0, "hidden catalog performs no SD directory scan");
    pk_airband_recording_store_set_visible(true);
    pk_airband_recording_store_test_set_oom(true);
    pk_airband_recording_store_test_poll();
    pk_airband_recording_snapshot_t oom = {0};
    CHECK(pk_airband_recording_store_snapshot_acquire(&oom) && oom.count == 0 &&
              oom.catalog_state == PK_AIRBAND_RECORDING_CATALOG_OOM && !oom.stale,
          "initial PSRAM OOM is explicit rather than a normal empty catalog");
    pk_airband_recording_snapshot_release(&oom);
    pk_airband_recording_store_test_set_oom(false);
    const unsigned failed_attempts = acquisitions;
    pk_airband_recording_store_request_refresh();
    pk_airband_recording_store_set_visible(false);
    pk_airband_recording_store_set_visible(true);
    pk_airband_recording_store_test_poll();
    CHECK(acquisitions == failed_attempts,
          "refresh and reopen cannot bypass failed-scan retry backoff");
    now_us += 10 * 1000 * 1000;
    pk_airband_recording_store_test_poll();
    CHECK(leases == 0, "scan releases its SD lease");
    pk_airband_recording_snapshot_t first = {0};
    CHECK(pk_airband_recording_store_snapshot_acquire(&first) && first.count == 1,
          "first immutable snapshot is available");
    const uint32_t unchanged_revision =
        pk_airband_recording_store_revision();
    const unsigned before_idle_polls = acquisitions;
    for (unsigned i = 0; i < 4; ++i) {
        now_us += 2 * 1000 * 1000;
        pk_airband_recording_store_test_poll();
    }
    CHECK(acquisitions == before_idle_polls,
          "visible unchanged catalog performs no periodic full scan");
    CHECK(pk_airband_recording_store_revision() == unchanged_revision,
          "unchanged scans keep the published revision stable");

    write_wav(2);
    pk_airband_recording_store_set_visible(false);
    pk_airband_recording_store_request_refresh();
    pk_airband_recording_store_test_poll();
    CHECK(acquisitions == before_idle_polls,
          "file change while hidden defers scan until a visible consumer exists");
    pk_airband_recording_store_set_visible(true);
    pk_airband_recording_store_test_poll();
    CHECK(pk_airband_recording_store_revision() != unchanged_revision,
          "a changed catalog advances the published revision");
    pk_airband_recording_snapshot_t second = {0};
    CHECK(pk_airband_recording_store_snapshot_acquire(&second) && second.count == 2,
          "new finalized file appears without a mount-generation change");
    CHECK(first.count == 1 && first.entries[0].id.session == 1,
          "previous caller snapshot remains immutable after publication");
    CHECK(second.entries[0].id.session == 2, "latest recording sorts first");
    pk_airband_recording_entry_t found;
    CHECK(pk_airband_recording_store_find(second.entries[1].id, &found) &&
              found.id.session == 1,
          "stable ID resolves through the current snapshot");

    char saved_dir[180]; snprintf(saved_dir, sizeof(saved_dir), "%s/airband_saved", root);
    CHECK(rename(dir, saved_dir) == 0 && symlink("airband", dir) == 0,
          "replace catalog directory with an ELOOP fixture");
    pk_airband_recording_store_request_refresh();
    pk_airband_recording_store_test_poll();
    pk_airband_recording_snapshot_t io_error = {0};
    CHECK(pk_airband_recording_store_snapshot_acquire(&io_error) &&
              io_error.count == 2 && io_error.stale &&
              io_error.catalog_state == PK_AIRBAND_RECORDING_CATALOG_SCAN_ERROR,
          "opendir failure preserves but explicitly marks the stale catalog");
    pk_airband_recording_snapshot_release(&io_error);
    const unsigned errored_attempts = acquisitions;
    now_us += 2 * 1000 * 1000;
    pk_airband_recording_store_test_poll();
    CHECK(acquisitions == errored_attempts,
          "directory IO failure does not retry every worker poll");
    CHECK(unlink(dir) == 0 && rename(saved_dir, dir) == 0,
          "restore real catalog directory");

    mounted = false;
    pk_airband_recording_snapshot_t immediate_absent = {0};
    CHECK(pk_airband_recording_store_snapshot_acquire(&immediate_absent) &&
              immediate_absent.count == 0 &&
              immediate_absent.catalog_state ==
                  PK_AIRBAND_RECORDING_CATALOG_SD_UNAVAILABLE,
          "snapshot reports SD unavailable immediately, before the 2s poll");
    pk_airband_recording_snapshot_release(&immediate_absent);
    pk_airband_recording_store_test_poll();
    pk_airband_recording_snapshot_t absent = {0};
    CHECK(pk_airband_recording_store_snapshot_acquire(&absent) && absent.count == 0,
          "card removal clears published paths before another card appears");
    CHECK(!pk_airband_recording_store_find(found.id, &found),
          "removed-card IDs cannot resolve to stale paths");

    pk_airband_recording_snapshot_release(&absent);
    absent = (pk_airband_recording_snapshot_t){0};
    mounted = true;
    generation++;
    pk_airband_recording_store_test_poll();
    pk_airband_recording_snapshot_t remounted = {0};
    CHECK(pk_airband_recording_store_snapshot_acquire(&remounted) &&
          remounted.count == 2 &&
          remounted.catalog_state == PK_AIRBAND_RECORDING_CATALOG_READY,
          "visible new mount scans immediately despite previous-card failure backoff");
    pk_airband_recording_snapshot_release(&remounted);
    const unsigned before_reopen = acquisitions;
    pk_airband_recording_store_set_visible(false);
    pk_airband_recording_store_set_visible(true);
    pk_airband_recording_store_test_poll();
    CHECK(acquisitions == before_reopen + 1,
          "reopening the page requests one fresh directory scan");
    const unsigned before_concurrent_request = acquisitions;
    pk_airband_recording_store_request_refresh();
    request_during_acquire = true;
    pk_airband_recording_store_test_poll();
    pk_airband_recording_store_test_poll();
    pk_airband_recording_store_test_poll();
    CHECK(acquisitions == before_concurrent_request + 2,
          "file change requested during a scan remains pending for one follow-up scan");
    pk_airband_recording_snapshot_release(&first);
    pk_airband_recording_snapshot_release(&second);
    pk_airband_recording_snapshot_release(&absent);
    char path[320];
    for (unsigned i = 1; i <= 2; ++i) {
        snprintf(path, sizeof(path),
                 "%s/airband/s%010u-c121500-f121500000-0000000001.wav", root, i);
        unlink(path);
    }
    rmdir(dir); rmdir(root);
    if (failures) return 1;
    puts("airband recording service tests PASS");
    return 0;
}
