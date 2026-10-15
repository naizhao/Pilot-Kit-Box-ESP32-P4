/* AIRBAND finalized-recording playback service.  All FAT I/O lives in its
 * worker; UI callers only pass stable catalog IDs and consume snapshots. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airband_recording_store.h"

typedef enum {
    PK_AIRBAND_PLAYBACK_PAUSE_NONE = 0,
    PK_AIRBAND_PLAYBACK_PAUSE_USER,
    PK_AIRBAND_PLAYBACK_PAUSE_LIVE,
    PK_AIRBAND_PLAYBACK_PAUSE_RECORD_PRESSURE,
    PK_AIRBAND_PLAYBACK_PAUSE_TEST_TONE,
    PK_AIRBAND_PLAYBACK_PAUSE_EOF,
    PK_AIRBAND_PLAYBACK_PAUSE_SD_UNAVAILABLE,
    PK_AIRBAND_PLAYBACK_PAUSE_FILE_CHANGED,
    PK_AIRBAND_PLAYBACK_PAUSE_IO_ERROR,
    PK_AIRBAND_PLAYBACK_PAUSE_SPEAKER_ERROR,
} pk_airband_playback_pause_reason_t;

typedef enum {
    PK_AIRBAND_PLAYBACK_SERVICE_NOT_STARTED = 0,
    PK_AIRBAND_PLAYBACK_SERVICE_READY,
    PK_AIRBAND_PLAYBACK_SERVICE_START_FAILED,
} pk_airband_playback_service_state_t;

typedef enum {
    PK_AIRBAND_PLAYBACK_SELECT_OK = 0,
    PK_AIRBAND_PLAYBACK_SELECT_SERVICE_UNAVAILABLE,
    PK_AIRBAND_PLAYBACK_SELECT_SD_UNAVAILABLE,
    PK_AIRBAND_PLAYBACK_SELECT_FILE_CHANGED,
} pk_airband_playback_select_result_t;

typedef struct {
    bool started;
    pk_airband_playback_service_state_t service_state;
    bool selected;
    bool playing;
    pk_airband_recording_id_t id;
    pk_airband_playback_pause_reason_t pause_reason;
    uint32_t mount_generation;
    uint32_t cursor_bytes;
    uint32_t data_bytes;
    uint32_t cursor_ms;
    uint32_t duration_ms;
    uint32_t blocks_written;
    uint32_t live_preemptions;
    uint32_t pressure_pauses;
    uint32_t io_errors;
} pk_airband_playback_status_t;

/* Idempotent. Must run after pk_sdcard_init(), recording store and speaker. */
bool pk_airband_playback_start(void);

/* Selection is validated against the current immutable catalog snapshot. */
pk_airband_playback_select_result_t pk_airband_playback_select_ex(
    pk_airband_recording_id_t id);
bool pk_airband_playback_select(pk_airband_recording_id_t id);
void pk_airband_playback_toggle(void);
void pk_airband_playback_pause(pk_airband_playback_pause_reason_t reason);
void pk_airband_playback_stop(void);
bool pk_airband_playback_get(pk_airband_playback_status_t *out);

#ifdef PK_AIRBAND_PLAYBACK_TEST_HOOKS
void pk_airband_playback_test_poll(void);
int pk_airband_playback_test_fd(void);
void pk_airband_playback_test_set_start_failure(bool fail);
uint32_t pk_airband_playback_test_seek_count(void);
#endif
