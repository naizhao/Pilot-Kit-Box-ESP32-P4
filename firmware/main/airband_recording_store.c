#include "airband_recording_store.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    size_t valid;
    size_t invalid;
    size_t parts;
    uint64_t fingerprint_xor;
    uint64_t fingerprint_sum;
} scan_counts_t;

typedef enum {
    ENTRY_VALID = 0,
    ENTRY_INVALID,
    ENTRY_CHANGED,
    ENTRY_IO_ERROR,
} entry_result_t;

static void *default_alloc(void *ctx, size_t bytes)
{ (void)ctx; return malloc(bytes); }

static void default_release(void *ctx, void *ptr)
{ (void)ctx; free(ptr); }

static bool has_suffix(const char *name, const char *suffix)
{
    const size_t name_len = strlen(name), suffix_len = strlen(suffix);
    return name_len >= suffix_len &&
           strcmp(name + name_len - suffix_len, suffix) == 0;
}

static entry_result_t read_entry(const char *directory, const char *filename,
                                 pk_airband_recording_entry_t *out)
{
    pk_airband_recording_name_t parsed;
    if (!pk_airband_recording_name_parse(filename, &parsed)) return ENTRY_INVALID;
    char path[PK_AIRBAND_RECORDING_PATH_MAX];
    const int n = snprintf(path, sizeof(path), "%s/%s", directory, filename);
    if (n < 0 || (size_t)n >= sizeof(path)) return ENTRY_INVALID;

    errno = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return errno == ENOENT ? ENTRY_CHANGED : ENTRY_IO_ERROR;
    struct stat st;
    uint8_t header[44];
    if (fstat(fileno(fp), &st) != 0) {
        (void)fclose(fp);
        return ENTRY_IO_ERROR;
    }
    if (!S_ISREG(st.st_mode)) {
        (void)fclose(fp);
        return ENTRY_INVALID;
    }
    const size_t got = fread(header, 1, sizeof(header), fp);
    if (got != sizeof(header)) {
        const bool io_error = ferror(fp) != 0;
        (void)fclose(fp);
        if (io_error) return ENTRY_IO_ERROR;
        return st.st_size >= (off_t)sizeof(header) ? ENTRY_CHANGED : ENTRY_INVALID;
    }
    if (fclose(fp) != 0) return ENTRY_IO_ERROR;

    pk_airband_wav_info_t wav;
    if (pk_airband_wav_read_header(header, (uint64_t)st.st_size, &wav) !=
        PK_AIRBAND_WAV_READ_OK) return ENTRY_INVALID;
    /* ESP-IDF FATFS fstat() reports zero timestamps; stat() reads the FAT
     * directory timestamp. Keep the opened file's size for WAV validation. */
    struct stat path_st;
    if (stat(path, &path_st) != 0)
        return errno == ENOENT ? ENTRY_CHANGED : ENTRY_IO_ERROR;
    if (!S_ISREG(path_st.st_mode) || path_st.st_size != st.st_size)
        return ENTRY_CHANGED;
    if (out) {
        *out = (pk_airband_recording_entry_t){
            .id = {
                .session = parsed.session,
                .volume = parsed.volume,
                .interrupted = parsed.interrupted,
            },
            .name = parsed,
            .wav = wav,
            .file_bytes = (uint64_t)st.st_size,
            .mtime_sec = (int64_t)path_st.st_mtime,
        };
        snprintf(out->filename, sizeof(out->filename), "%s", filename);
        snprintf(out->path, sizeof(out->path), "%s", path);
    }
    return ENTRY_VALID;
}

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t bytes)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < bytes; ++i) {
        hash ^= p[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t mix64(uint64_t value)
{
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static void fingerprint_add(scan_counts_t *counts, char kind, const char *name,
                            const pk_airband_recording_entry_t *entry)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = hash_bytes(hash, &kind, sizeof(kind));
    hash = hash_bytes(hash, name, strlen(name));
    if (entry) {
        hash = hash_bytes(hash, &entry->file_bytes, sizeof(entry->file_bytes));
        hash = hash_bytes(hash, &entry->mtime_sec, sizeof(entry->mtime_sec));
    }
    hash = mix64(hash);
    counts->fingerprint_xor ^= hash;
    counts->fingerprint_sum += hash;
}

static pk_airband_recording_scan_result_t scan_pass(
    const char *directory, pk_airband_recording_entry_t *entries,
    size_t entry_capacity, scan_counts_t *counts)
{
    *counts = (scan_counts_t){0};
    DIR *dir = opendir(directory);
    if (!dir) return errno == ENOENT ? PK_AIRBAND_RECORDING_SCAN_NOT_FOUND
                                     : PK_AIRBAND_RECORDING_SCAN_OPEN_FAILED;
    pk_airband_recording_scan_result_t result = PK_AIRBAND_RECORDING_SCAN_OK;
    errno = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) {
            errno = 0;
            continue;
        }
        if (has_suffix(name, ".part.wav")) {
            counts->parts++;
            fingerprint_add(counts, 'P', name, NULL);
            errno = 0;
            continue;
        }
        if (!has_suffix(name, ".wav")) {
            errno = 0;
            continue;
        }
        pk_airband_recording_entry_t entry;
        const entry_result_t entry_result = read_entry(directory, name, &entry);
        if (entry_result == ENTRY_IO_ERROR) {
            result = PK_AIRBAND_RECORDING_SCAN_IO_ERROR;
            break;
        }
        if (entry_result == ENTRY_CHANGED) {
            result = PK_AIRBAND_RECORDING_SCAN_CHANGED;
            break;
        }
        if (entry_result == ENTRY_INVALID) {
            counts->invalid++;
            fingerprint_add(counts, 'I', name, NULL);
        } else {
            if (entries) {
                if (counts->valid >= entry_capacity) {
                    result = PK_AIRBAND_RECORDING_SCAN_CHANGED;
                    break;
                }
                entries[counts->valid] = entry;
            }
            fingerprint_add(counts, 'V', name, &entry);
            counts->valid++;
        }
        errno = 0;
    }
    if (result == PK_AIRBAND_RECORDING_SCAN_OK && errno != 0)
        result = PK_AIRBAND_RECORDING_SCAN_IO_ERROR;
    if (closedir(dir) != 0 && result == PK_AIRBAND_RECORDING_SCAN_OK)
        result = PK_AIRBAND_RECORDING_SCAN_IO_ERROR;
    return result;
}

static int entry_newest_first(const void *lhs, const void *rhs)
{
    const pk_airband_recording_entry_t *a = lhs;
    const pk_airband_recording_entry_t *b = rhs;
    return pk_airband_recording_newest_first(&a->name, &b->name);
}

void pk_airband_recording_snapshot_release(
    pk_airband_recording_snapshot_t *snapshot)
{
    if (!snapshot) return;
    if (snapshot->entries && snapshot->allocator.release)
        snapshot->allocator.release(snapshot->allocator.ctx, snapshot->entries);
    *snapshot = (pk_airband_recording_snapshot_t){0};
}

pk_airband_recording_scan_result_t pk_airband_recording_scan_directory(
    const char *directory, const pk_airband_recording_allocator_t *allocator,
    pk_airband_recording_snapshot_t *out)
{
    if (!directory || !out || out->entries) return PK_AIRBAND_RECORDING_SCAN_INVALID;
    pk_airband_recording_allocator_t hooks = allocator ? *allocator :
        (pk_airband_recording_allocator_t){
            .alloc = default_alloc, .release = default_release,
        };
    if (!hooks.alloc || !hooks.release) return PK_AIRBAND_RECORDING_SCAN_INVALID;

    scan_counts_t first;
    pk_airband_recording_scan_result_t result =
        scan_pass(directory, NULL, 0, &first);
    if (result != PK_AIRBAND_RECORDING_SCAN_OK) return result;
    if (first.valid > SIZE_MAX / sizeof(pk_airband_recording_entry_t))
        return PK_AIRBAND_RECORDING_SCAN_OOM;

    pk_airband_recording_entry_t *entries = NULL;
    if (first.valid != 0) {
        entries = hooks.alloc(hooks.ctx,
                              first.valid * sizeof(pk_airband_recording_entry_t));
        if (!entries) return PK_AIRBAND_RECORDING_SCAN_OOM;
    }

    scan_counts_t second;
    result = scan_pass(directory, entries, first.valid, &second);
    const bool changed = result == PK_AIRBAND_RECORDING_SCAN_CHANGED ||
                         first.valid != second.valid ||
                         first.invalid != second.invalid ||
                         first.parts != second.parts ||
                         first.fingerprint_xor != second.fingerprint_xor ||
                         first.fingerprint_sum != second.fingerprint_sum;
    if (result != PK_AIRBAND_RECORDING_SCAN_OK || changed) {
        if (entries) hooks.release(hooks.ctx, entries);
        return changed ? PK_AIRBAND_RECORDING_SCAN_CHANGED : result;
    }

    if (second.valid != 0)
        qsort(entries, second.valid, sizeof(entries[0]), entry_newest_first);
    *out = (pk_airband_recording_snapshot_t){
        .entries = entries,
        .count = second.valid,
        .invalid_files = second.invalid,
        .ignored_parts = second.parts,
        .allocator = hooks,
        .catalog_state = PK_AIRBAND_RECORDING_CATALOG_READY,
    };
    return PK_AIRBAND_RECORDING_SCAN_OK;
}
