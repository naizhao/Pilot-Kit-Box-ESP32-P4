/*
 * cc -std=c11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE -Ifirmware/main \
 *   -o /tmp/test_airband_recording_store \
 *   firmware/test/test_airband_recording_store.c \
 *   firmware/main/airband_recording_store.c \
 *   firmware/main/airband_recording_catalog.c \
 *   firmware/main/airband_wav_reader.c && \
 *   /tmp/test_airband_recording_store
 */
#include "airband_recording_store.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

static void put16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static bool write_valid_wav(const char *path, uint32_t frames)
{
    const uint32_t bytes = frames * 2u;
    uint8_t h[44] = {0};
    memcpy(h, "RIFF", 4); put32(h + 4, 36u + bytes);
    memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16u);
    put16(h + 20, 1u); put16(h + 22, 1u); put32(h + 24, 16000u);
    put32(h + 28, 32000u); put16(h + 32, 2u); put16(h + 34, 16u);
    memcpy(h + 36, "data", 4); put32(h + 40, bytes);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(h, 1, sizeof(h), f) == sizeof(h);
    int16_t zero = 0;
    for (uint32_t i = 0; ok && i < frames; ++i)
        ok = fwrite(&zero, 1, sizeof(zero), f) == sizeof(zero);
    return fclose(f) == 0 && ok;
}

static bool write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    const size_t n = strlen(text);
    bool ok = fwrite(text, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static void path_join(char *out, size_t cap, const char *dir, const char *name)
{ snprintf(out, cap, "%s/%s", dir, name); }

static void clean_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            char path[512]; path_join(path, sizeof(path), dir, ent->d_name);
            (void)unlink(path);
        }
        closedir(d);
    }
    (void)rmdir(dir);
}

static void test_real_directory_returns_all_valid_files_and_isolates_bad_ones(void)
{
    char dir[] = "/tmp/pk_airband_store_all_XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "temporary directory must be created");
    char path[512], name[96];
    for (unsigned session = 1; session <= 53; ++session) {
        snprintf(name, sizeof(name),
                 "s%010u-c121500-f121500000-0000000001%s",
                 session, session == 17 ? "-interrupted.wav" : ".wav");
        path_join(path, sizeof(path), dir, name);
        CHECK(write_valid_wav(path, session), "valid fixture WAV must be written");
    }
    path_join(path, sizeof(path), dir,
              "s0000000054-c121500-f121500000-0000000001.wav");
    CHECK(write_text(path, "broken"), "bad final fixture must be written");
    path_join(path, sizeof(path), dir,
              "s0000000055-c121500-f121500000-0000000001.part.wav");
    CHECK(write_text(path, "still recording"), "part fixture must be written");
    path_join(path, sizeof(path), dir, "foreign.wav");
    CHECK(write_text(path, "not ours"), "foreign WAV fixture must be written");

    pk_airband_recording_snapshot_t snap = {0};
    CHECK(pk_airband_recording_scan_directory(dir, NULL, &snap) ==
              PK_AIRBAND_RECORDING_SCAN_OK,
          "real directory scan must succeed despite isolated bad files");
    CHECK(snap.count == 53u, "all 53 valid recordings must be published");
    CHECK(snap.invalid_files == 2u, "bad project and foreign WAV must be counted");
    CHECK(snap.ignored_parts == 1u, "active part file must be excluded explicitly");
    CHECK(snap.entries && snap.entries[0].name.session == 53u,
          "newest session must be first");
    CHECK(snap.entries && snap.entries[52].name.session == 1u,
          "oldest session must remain reachable");
    CHECK(snap.entries && snap.entries[36].name.session == 17u &&
              snap.entries[36].name.interrupted,
          "interrupted metadata must survive directory publication");
    CHECK(snap.entries && snap.entries[52].wav.duration_ms == 0u,
          "sub-millisecond duration must be reported without rounding up");
    CHECK(snap.entries && snap.entries[0].mtime_sec > 0,
          "filesystem mtime must be published");
    CHECK(snap.entries && strstr(snap.entries[0].path, dir) == snap.entries[0].path,
          "playback path must resolve inside the scanned directory");
    pk_airband_recording_snapshot_release(&snap);
    CHECK(snap.entries == NULL && snap.count == 0u,
          "release must clear caller-visible ownership");
    clean_dir(dir);
}

static void *always_oom(void *ctx, size_t bytes)
{ (void)ctx; (void)bytes; return NULL; }
static void ignore_free(void *ctx, void *ptr) { (void)ctx; (void)ptr; }

static void test_oom_is_explicit_not_silent_truncation(void)
{
    char dir[] = "/tmp/pk_airband_store_oom_XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "OOM temp directory must be created");
    char path[512]; path_join(path, sizeof(path), dir,
        "s0000000001-c121500-f121500000-0000000001.wav");
    CHECK(write_valid_wav(path, 0), "OOM fixture must be written");
    pk_airband_recording_allocator_t allocator = {
        .alloc = always_oom, .release = ignore_free,
    };
    pk_airband_recording_snapshot_t snap = {0};
    CHECK(pk_airband_recording_scan_directory(dir, &allocator, &snap) ==
              PK_AIRBAND_RECORDING_SCAN_OOM,
          "allocation failure must be explicit");
    CHECK(snap.count == 0u && snap.entries == NULL,
          "OOM must never publish a truncated prefix");
    clean_dir(dir);
}

typedef struct { const char *dir; bool fired; } changing_alloc_ctx_t;
static void *alloc_and_add_file(void *opaque, size_t bytes)
{
    changing_alloc_ctx_t *ctx = opaque;
    if (!ctx->fired) {
        ctx->fired = true;
        char path[512]; path_join(path, sizeof(path), ctx->dir,
            "s0000000002-c121500-f121500000-0000000001.wav");
        (void)write_valid_wav(path, 0);
    }
    return malloc(bytes);
}
static void normal_free(void *ctx, void *ptr) { (void)ctx; free(ptr); }

static void test_directory_change_between_passes_is_not_published(void)
{
    char dir[] = "/tmp/pk_airband_store_change_XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "change temp directory must be created");
    char path[512]; path_join(path, sizeof(path), dir,
        "s0000000001-c121500-f121500000-0000000001.wav");
    CHECK(write_valid_wav(path, 0), "initial change fixture must be written");
    changing_alloc_ctx_t ctx = {.dir = dir};
    pk_airband_recording_allocator_t allocator = {
        .ctx = &ctx, .alloc = alloc_and_add_file, .release = normal_free,
    };
    pk_airband_recording_snapshot_t snap = {0};
    CHECK(pk_airband_recording_scan_directory(dir, &allocator, &snap) ==
              PK_AIRBAND_RECORDING_SCAN_CHANGED,
          "two-pass race must request a rescan instead of publishing partial data");
    CHECK(snap.entries == NULL && snap.count == 0u,
          "changed directory must not leak a partial snapshot");
    clean_dir(dir);
}

typedef struct { const char *dir; bool fired; } replacing_alloc_ctx_t;
static void *alloc_and_replace_file(void *opaque, size_t bytes)
{
    replacing_alloc_ctx_t *ctx = opaque;
    if (!ctx->fired) {
        ctx->fired = true;
        char old_path[512], new_path[512];
        path_join(old_path, sizeof(old_path), ctx->dir,
            "s0000000001-c121500-f121500000-0000000001.wav");
        path_join(new_path, sizeof(new_path), ctx->dir,
            "s0000000002-c121500-f121500000-0000000001.wav");
        (void)unlink(old_path);
        (void)write_valid_wav(new_path, 0);
    }
    return malloc(bytes);
}

static void test_equal_count_replacement_between_passes_is_detected(void)
{
    char dir[] = "/tmp/pk_airband_store_replace_XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "replacement temp directory must be created");
    char path[512]; path_join(path, sizeof(path), dir,
        "s0000000001-c121500-f121500000-0000000001.wav");
    CHECK(write_valid_wav(path, 0), "replacement source must be written");
    replacing_alloc_ctx_t ctx = {.dir = dir};
    pk_airband_recording_allocator_t allocator = {
        .ctx = &ctx, .alloc = alloc_and_replace_file, .release = normal_free,
    };
    pk_airband_recording_snapshot_t snap = {0};
    CHECK(pk_airband_recording_scan_directory(dir, &allocator, &snap) ==
              PK_AIRBAND_RECORDING_SCAN_CHANGED,
          "equal-count file replacement must invalidate the two-pass scan");
    CHECK(snap.entries == NULL && snap.count == 0u,
          "replacement race must not publish the second pass as stable");
    clean_dir(dir);
}

static void test_filesystem_error_is_not_reported_as_a_bad_wav(void)
{
    char dir[] = "/tmp/pk_airband_store_io_XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "I/O temp directory must be created");
    const char *name = "s0000000001-c121500-f121500000-0000000001.wav";
    char path[512]; path_join(path, sizeof(path), dir, name);
    CHECK(symlink(name, path) == 0, "self-referential symlink must be created");
    pk_airband_recording_snapshot_t snap = {0};
    CHECK(pk_airband_recording_scan_directory(dir, NULL, &snap) ==
              PK_AIRBAND_RECORDING_SCAN_IO_ERROR,
          "ELOOP opening a valid recording name must be an I/O error");
    CHECK(snap.entries == NULL && snap.count == 0u,
          "I/O failure must not publish a silently incomplete snapshot");
    clean_dir(dir);
}

static void test_complete_and_interrupted_variants_have_distinct_ids(void)
{
    char dir[] = "/tmp/pk_airband_store_ids_XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "ID temp directory must be created");
    char normal[512], interrupted[512];
    path_join(normal, sizeof(normal), dir,
        "s0000000007-c121500-f121500000-0000000002.wav");
    path_join(interrupted, sizeof(interrupted), dir,
        "s0000000007-c121500-f121500000-0000000002-interrupted.wav");
    CHECK(write_valid_wav(normal, 0) && write_valid_wav(interrupted, 0),
          "both terminal variants must be written");
    pk_airband_recording_snapshot_t snap = {0};
    CHECK(pk_airband_recording_scan_directory(dir, NULL, &snap) ==
              PK_AIRBAND_RECORDING_SCAN_OK && snap.count == 2u,
          "both valid variants must be visible");
    CHECK(snap.entries &&
              snap.entries[0].id.session == snap.entries[1].id.session &&
              snap.entries[0].id.volume == snap.entries[1].id.volume &&
              snap.entries[0].id.interrupted != snap.entries[1].id.interrupted,
          "stable ID must distinguish complete and interrupted files");
    pk_airband_recording_snapshot_release(&snap);
    clean_dir(dir);
}

static void test_empty_directory_is_a_valid_empty_snapshot(void)
{
    char dir[] = "/tmp/pk_airband_store_empty_XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "empty temp directory must be created");
    pk_airband_recording_snapshot_t snap = {0};
    CHECK(pk_airband_recording_scan_directory(dir, NULL, &snap) ==
              PK_AIRBAND_RECORDING_SCAN_OK,
          "empty directory must not be an error");
    CHECK(snap.count == 0u && snap.entries == NULL,
          "empty directory must publish an empty snapshot");
    pk_airband_recording_snapshot_release(&snap);
    clean_dir(dir);
}

int main(void)
{
    test_real_directory_returns_all_valid_files_and_isolates_bad_ones();
    test_oom_is_explicit_not_silent_truncation();
    test_directory_change_between_passes_is_not_published();
    test_equal_count_replacement_between_passes_is_detected();
    test_filesystem_error_is_not_reported_as_a_bad_wav();
    test_complete_and_interrupted_variants_have_distinct_ids();
    test_empty_directory_is_a_valid_empty_snapshot();
    if (failures) return 1;
    puts("airband recording store tests PASS");
    return 0;
}
