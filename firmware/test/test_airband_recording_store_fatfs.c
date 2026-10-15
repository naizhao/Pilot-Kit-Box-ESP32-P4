/*
 * cc -std=c11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE -Ifirmware/main \
 *   -o /tmp/test_airband_recording_store_fatfs \
 *   firmware/test/test_airband_recording_store_fatfs.c \
 *   firmware/main/airband_recording_catalog.c \
 *   firmware/main/airband_wav_reader.c && \
 *   /tmp/test_airband_recording_store_fatfs
 */
#include "airband_recording_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

static int path_stat_error;
static bool path_size_changed;
static bool path_is_directory;

/* ESP-IDF vfs_fat_fstat preserves size/type but reports zero timestamps.
 * All file creation, opening, header reads and directory scans remain real. */
static int fatfs_fstat(int fd, struct stat *st)
{
    const int result = fstat(fd, st);
    if (result == 0) {
        st->st_mtime = 0;
        st->st_atime = 0;
        st->st_ctime = 0;
    }
    return result;
}

static int fatfs_stat(const char *path, struct stat *st)
{
    if (path_stat_error != 0) {
        errno = path_stat_error;
        return -1;
    }
    const int result = stat(path, st);
    if (result == 0) {
        if (path_size_changed) st->st_size += 2;
        if (path_is_directory) st->st_mode = S_IFDIR | 0700;
    }
    return result;
}

#define fstat(fd, st) fatfs_fstat(fd, st)
#define stat(path, st) fatfs_stat(path, st)
#include "../main/airband_recording_store.c"
#undef stat
#undef fstat

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

/* Literal PCM WAV: 16000 Hz, one 16-bit mono sample, 46 bytes total. */
static const unsigned char wav[] = {
    'R','I','F','F',38,0,0,0,'W','A','V','E','f','m','t',' ',
    16,0,0,0,1,0,1,0,0x80,0x3e,0,0,0,0x7d,0,0,
    2,0,16,0,'d','a','t','a',2,0,0,0,0,0,
};

static bool create_fixture(char *dir, char *path, size_t capacity,
                           time_t mtime)
{
    if (!mkdtemp(dir)) return false;
    const int n = snprintf(path, capacity,
        "%s/s0000000001-c121500-f121500000-0000000001.wav", dir);
    if (n < 0 || (size_t)n >= capacity) return false;
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    const bool written = fwrite(wav, 1, sizeof(wav), fp) == sizeof(wav);
    if (fclose(fp) != 0 || !written) return false;
    const struct utimbuf times = {.actime = mtime, .modtime = mtime};
    return utime(path, &times) == 0;
}

static void reset_boundary(void)
{
    path_stat_error = 0;
    path_size_changed = false;
    path_is_directory = false;
}

static void release_fixture(const char *dir, const char *path)
{
    /* Only this test's mkdtemp-created file and directory are removed. */
    CHECK(unlink(path) == 0, "owned WAV fixture must be removed");
    CHECK(rmdir(dir) == 0, "owned temporary directory must be removed");
}

static void test_fatfs_path_mtime_is_published(time_t fixture_time,
                                            int64_t expected_time)
{
    char dir[] = "/tmp/pk_airband_fatfs_time_XXXXXX", path[320];
    CHECK(create_fixture(dir, path, sizeof(path), fixture_time),
          "timestamp fixture must be created");
    reset_boundary();
    pk_airband_recording_snapshot_t snap = {0};
    CHECK(pk_airband_recording_scan_directory(dir, NULL, &snap) ==
              PK_AIRBAND_RECORDING_SCAN_OK,
          "FATFS timestamp scan must succeed");
    CHECK(snap.entries && snap.count == 1u &&
              snap.entries[0].mtime_sec == expected_time,
          "snapshot must preserve the literal path mtime, including unknown zero");
    CHECK(snap.entries && snap.entries[0].file_bytes == 46u &&
              snap.entries[0].wav.data_bytes == 2u,
          "descriptor size and real WAV header must still be validated");
    pk_airband_recording_snapshot_release(&snap);
    release_fixture(dir, path);
}

static void test_path_metadata_failure_is_not_published(int error,
    pk_airband_recording_scan_result_t expected)
{
    char dir[] = "/tmp/pk_airband_fatfs_error_XXXXXX", path[320];
    CHECK(create_fixture(dir, path, sizeof(path), 1700000000),
          "metadata failure fixture must be created");
    reset_boundary();
    path_stat_error = error;
    pk_airband_recording_snapshot_t snap = {0};
    CHECK(pk_airband_recording_scan_directory(dir, NULL, &snap) == expected,
          "path metadata failure must retain its I/O or changed classification");
    CHECK(snap.entries == NULL && snap.count == 0u,
          "metadata failure must not publish zero-time or partial entries");
    pk_airband_recording_snapshot_release(&snap);
    release_fixture(dir, path);
}

typedef struct { const char *path; } time_change_ctx_t;
static void *alloc_and_change_time(void *opaque, size_t bytes)
{
    const time_change_ctx_t *ctx = opaque;
    const struct utimbuf times = {.actime = 1700000002, .modtime = 1700000002};
    CHECK(utime(ctx->path, &times) == 0, "mtime must change between passes");
    return malloc(bytes);
}

static void release_allocation(void *ctx, void *ptr)
{ (void)ctx; free(ptr); }

static void test_time_change_between_passes_is_not_published(void)
{
    char dir[] = "/tmp/pk_airband_fatfs_change_XXXXXX", path[320];
    CHECK(create_fixture(dir, path, sizeof(path), 1700000000),
          "mtime change fixture must be created");
    reset_boundary();
    time_change_ctx_t ctx = {.path = path};
    const pk_airband_recording_allocator_t allocator = {
        .ctx = &ctx, .alloc = alloc_and_change_time, .release = release_allocation,
    };
    pk_airband_recording_snapshot_t snap = {0};
    CHECK(pk_airband_recording_scan_directory(dir, &allocator, &snap) ==
              PK_AIRBAND_RECORDING_SCAN_CHANGED,
          "path mtime must participate in both-pass stability detection");
    CHECK(snap.entries == NULL && snap.count == 0u,
          "mtime race must not publish an inconsistent snapshot");
    pk_airband_recording_snapshot_release(&snap);
    release_fixture(dir, path);
}

static void test_path_no_longer_matches_descriptor(bool directory)
{
    char dir[] = "/tmp/pk_airband_fatfs_mismatch_XXXXXX", path[320];
    CHECK(create_fixture(dir, path, sizeof(path), 1700000000),
          "path mismatch fixture must be created");
    reset_boundary();
    path_size_changed = !directory;
    path_is_directory = directory;
    pk_airband_recording_snapshot_t snap = {0};
    CHECK(pk_airband_recording_scan_directory(dir, NULL, &snap) ==
              PK_AIRBAND_RECORDING_SCAN_CHANGED,
          "path size or type mismatch must request rescan");
    CHECK(snap.entries == NULL && snap.count == 0u,
          "descriptor/path mismatch must not publish mixed metadata");
    pk_airband_recording_snapshot_release(&snap);
    release_fixture(dir, path);
}

int main(void)
{
    test_fatfs_path_mtime_is_published(1700000000, INT64_C(1700000000));
    test_fatfs_path_mtime_is_published(0, INT64_C(0));
    test_path_metadata_failure_is_not_published(EIO,
        PK_AIRBAND_RECORDING_SCAN_IO_ERROR);
    test_path_metadata_failure_is_not_published(ENOENT,
        PK_AIRBAND_RECORDING_SCAN_CHANGED);
    test_time_change_between_passes_is_not_published();
    test_path_no_longer_matches_descriptor(false);
    test_path_no_longer_matches_descriptor(true);
    if (failures) return 1;
    puts("airband recording store FATFS boundary tests PASS");
    return 0;
}
