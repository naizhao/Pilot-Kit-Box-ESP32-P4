/* Portable directory snapshot core for finalized AIRBAND WAV recordings. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "airband_recording_catalog.h"
#include "airband_wav_reader.h"

#define PK_AIRBAND_RECORDING_FILENAME_MAX 64u
#define PK_AIRBAND_RECORDING_PATH_MAX     320u

typedef struct {
    uint32_t session;
    uint32_t volume;
    bool interrupted;
} pk_airband_recording_id_t;

typedef struct {
    pk_airband_recording_id_t id;
    pk_airband_recording_name_t name;
    pk_airband_wav_info_t wav;
    uint64_t file_bytes;
    int64_t mtime_sec;
    char filename[PK_AIRBAND_RECORDING_FILENAME_MAX];
    char path[PK_AIRBAND_RECORDING_PATH_MAX];
} pk_airband_recording_entry_t;

typedef struct {
    void *ctx;
    void *(*alloc)(void *ctx, size_t bytes);
    void (*release)(void *ctx, void *ptr);
} pk_airband_recording_allocator_t;

typedef enum {
    PK_AIRBAND_RECORDING_CATALOG_NOT_STARTED = 0,
    PK_AIRBAND_RECORDING_CATALOG_START_FAILED,
    PK_AIRBAND_RECORDING_CATALOG_NOT_READY,
    PK_AIRBAND_RECORDING_CATALOG_READY,
    PK_AIRBAND_RECORDING_CATALOG_SD_UNAVAILABLE,
    PK_AIRBAND_RECORDING_CATALOG_SCAN_ERROR,
    PK_AIRBAND_RECORDING_CATALOG_OOM,
} pk_airband_recording_catalog_state_t;

typedef struct {
    pk_airband_recording_entry_t *entries;
    size_t count;
    size_t invalid_files;
    size_t ignored_parts;
    pk_airband_recording_allocator_t allocator;
    /* Runtime service publication state.  The portable scanner publishes
     * READY on success; the async service may return a stale last-known-good
     * list together with an explicit scan/OOM error. */
    pk_airband_recording_catalog_state_t catalog_state;
    bool stale;
    /* Runtime publication revision. Portable directory scans leave this 0. */
    uint32_t revision;
} pk_airband_recording_snapshot_t;

typedef struct {
    uint32_t revision;
    uint32_t mount_generation;
    bool mounted;
} pk_airband_recording_store_token_t;

typedef enum {
    PK_AIRBAND_RECORDING_SNAPSHOT_OK = 0,
    PK_AIRBAND_RECORDING_SNAPSHOT_INVALID_ARGUMENT,
    PK_AIRBAND_RECORDING_SNAPSHOT_NOT_STARTED,
    PK_AIRBAND_RECORDING_SNAPSHOT_START_FAILED,
    PK_AIRBAND_RECORDING_SNAPSHOT_OOM,
} pk_airband_recording_snapshot_result_t;

typedef enum {
    PK_AIRBAND_RECORDING_SCAN_OK = 0,
    PK_AIRBAND_RECORDING_SCAN_INVALID,
    PK_AIRBAND_RECORDING_SCAN_NOT_FOUND,
    PK_AIRBAND_RECORDING_SCAN_OPEN_FAILED,
    PK_AIRBAND_RECORDING_SCAN_IO_ERROR,
    PK_AIRBAND_RECORDING_SCAN_OOM,
    PK_AIRBAND_RECORDING_SCAN_CHANGED,
} pk_airband_recording_scan_result_t;

/* Runs two complete passes. A directory that changes between passes is not
 * published. allocator==NULL uses malloc/free. out must be zero-initialized. */
pk_airband_recording_scan_result_t pk_airband_recording_scan_directory(
    const char *directory, const pk_airband_recording_allocator_t *allocator,
    pk_airband_recording_snapshot_t *out);

void pk_airband_recording_snapshot_release(
    pk_airband_recording_snapshot_t *snapshot);

/* Runtime catalog service. start() is idempotent and must run after
 * pk_sdcard_init().  Acquired snapshots are immutable caller-owned copies and
 * use the normal snapshot_release() lifecycle. */
bool pk_airband_recording_store_start(void);
/* Only visible consumers trigger SD scans. Entering the page refreshes once;
 * finalized/recovered/deleted files mark the catalog dirty without SD IO.
 * Requests while hidden are coalesced until the page is opened. Failed scans
 * retain their explicit error/stale state and retry at most once per 10 s. */
void pk_airband_recording_store_set_visible(bool visible);
void pk_airband_recording_store_request_refresh(void);
pk_airband_recording_snapshot_result_t
pk_airband_recording_store_snapshot_acquire_ex(
    pk_airband_recording_snapshot_t *out);
bool pk_airband_recording_store_snapshot_acquire(
    pk_airband_recording_snapshot_t *out);
/* Cheap allocation-free token for UI polling. The revision changes only when
 * published catalog data/state changes; mount fields make card removal and
 * replacement observable before the background scan runs. */
uint32_t pk_airband_recording_store_revision(void);
pk_airband_recording_store_token_t pk_airband_recording_store_token(void);
bool pk_airband_recording_store_find(pk_airband_recording_id_t id,
                                     pk_airband_recording_entry_t *out);

#ifdef PK_AIRBAND_RECORDING_SERVICE_TEST_HOOKS
void pk_airband_recording_store_test_poll(void);
void pk_airband_recording_store_test_set_oom(bool fail);
void pk_airband_recording_store_test_set_start_failure(bool fail);
#endif
