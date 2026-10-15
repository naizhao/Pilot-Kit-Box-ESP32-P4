/* AIRBAND 已完成录音浏览与回放页面。 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airband_recording_store.h"

#define PK_AIRBAND_RECORDINGS_LIST_TOP    82
#define PK_AIRBAND_RECORDINGS_ROW_H       60
#define PK_AIRBAND_RECORDINGS_LIST_BOTTOM 382
#define PK_AIRBAND_RECORDINGS_PREV_X0      278
#define PK_AIRBAND_RECORDINGS_PREV_X1      350
#define PK_AIRBAND_RECORDINGS_TOGGLE_X0    364
#define PK_AIRBAND_RECORDINGS_TOGGLE_X1    436
#define PK_AIRBAND_RECORDINGS_NEXT_X0      450
#define PK_AIRBAND_RECORDINGS_NEXT_X1      522
#define PK_AIRBAND_RECORDINGS_VOL_DOWN_X0   16
#define PK_AIRBAND_RECORDINGS_VOL_DOWN_X1   88
#define PK_AIRBAND_RECORDINGS_MUTE_X0      102
#define PK_AIRBAND_RECORDINGS_MUTE_X1      174
#define PK_AIRBAND_RECORDINGS_VOL_UP_X0    188
#define PK_AIRBAND_RECORDINGS_VOL_UP_X1    260
#define PK_AIRBAND_RECORDINGS_CONTROL_Y0   398
#define PK_AIRBAND_RECORDINGS_CONTROL_Y1   470

typedef enum {
    PK_AIRBAND_RECORDINGS_ACT_NONE = 0,
    PK_AIRBAND_RECORDINGS_ACT_SELECT,
    PK_AIRBAND_RECORDINGS_ACT_PREVIOUS,
    PK_AIRBAND_RECORDINGS_ACT_TOGGLE,
    PK_AIRBAND_RECORDINGS_ACT_NEXT,
    PK_AIRBAND_RECORDINGS_ACT_VOLUME_DOWN,
    PK_AIRBAND_RECORDINGS_ACT_MUTE,
    PK_AIRBAND_RECORDINGS_ACT_VOLUME_UP,
} pk_airband_recordings_action_kind_t;

typedef enum {
    PK_AIRBAND_RECORDINGS_CATALOG_READY = 0,
    PK_AIRBAND_RECORDINGS_CATALOG_SCANNING,
    PK_AIRBAND_RECORDINGS_CATALOG_SD_UNAVAILABLE,
    PK_AIRBAND_RECORDINGS_CATALOG_SCAN_ERROR,
    PK_AIRBAND_RECORDINGS_CATALOG_MEMORY_ERROR,
    PK_AIRBAND_RECORDINGS_CATALOG_UNAVAILABLE,
} pk_airband_recordings_catalog_view_t;

typedef struct {
    pk_airband_recordings_action_kind_t kind;
    int catalog_index;
} pk_airband_recordings_action_t;

typedef struct {
    bool active;
    bool moved;
    bool recording_at_press;
    int start_x;
    int start_y;
    int last_y;
    int last_x;
    int start_scroll;
    size_t catalog_count_at_press;
} pk_airband_recordings_gesture_t;

size_t pk_airband_recordings_row_count(size_t catalog_count, bool recording);
int pk_airband_recordings_max_scroll(size_t rows);
size_t pk_airband_recordings_row_at_y(int y, int scroll, size_t rows);
bool pk_airband_recordings_row_playable(size_t row, bool recording,
                                        size_t catalog_count);
int pk_airband_recordings_catalog_index(size_t row, bool recording,
                                        size_t catalog_count);
bool pk_airband_recordings_can_previous(size_t catalog_count, int selected);
bool pk_airband_recordings_can_next(size_t catalog_count, int selected);
bool pk_airband_recordings_start_after_select(bool select_succeeded,
                                              void (*start_playback)(void));
bool pk_airband_recordings_controls_available(
    pk_airband_recordings_catalog_view_t catalog_view, bool stale,
    bool playback_service_available);
pk_airband_recordings_catalog_view_t pk_airband_recordings_catalog_view(
    pk_airband_recording_snapshot_result_t snapshot_result,
    int catalog_state);
bool pk_airband_recordings_refresh_allowed(bool gesture_active);
bool pk_airband_recordings_snapshot_refresh_needed(
    bool force, pk_airband_recording_snapshot_result_t previous_result,
    pk_airband_recording_store_token_t observed,
    pk_airband_recording_store_token_t current);
bool pk_airband_recordings_snapshot_commit_allowed(
    bool page_active, uint32_t acquire_epoch, uint32_t current_epoch);
void pk_airband_recordings_gesture_press(pk_airband_recordings_gesture_t *g,
                                         int x, int y, int scroll,
                                         bool recording,
                                         size_t catalog_count);
int pk_airband_recordings_gesture_drag(pk_airband_recordings_gesture_t *g,
                                       int x, int y, int scroll);
pk_airband_recordings_action_t pk_airband_recordings_gesture_release(
    pk_airband_recordings_gesture_t *g, int selected,
    bool playback_available);
bool pk_airband_recordings_audio_action_enabled(
    pk_airband_recordings_action_kind_t action, bool speaker_available,
    uint8_t playback_volume);
pk_airband_recordings_action_t pk_airband_recordings_gesture_release_ex(
    pk_airband_recordings_gesture_t *g, int selected,
    bool playback_available, bool audio_controls_available,
    uint8_t playback_volume);
pk_airband_recordings_action_kind_t pk_airband_recordings_feedback(
    const pk_airband_recordings_gesture_t *gesture, int selected,
    bool playback_available, bool audio_available, uint8_t volume,
    pk_airband_recordings_action_kind_t latched, bool latch_active,
    uint32_t now, uint32_t started, uint32_t duration);
void pk_airband_recordings_format_duration(char *out, size_t capacity,
                                          uint32_t milliseconds);
bool pk_airband_recordings_time_valid(int64_t timestamp);

#ifndef PK_AIRBAND_RECORDINGS_PAGE_HOST_TEST
void pk_airband_recordings_page_init(void);
void pk_airband_recordings_page_open(void);
void pk_airband_recordings_page_leave(void);
void pk_airband_recordings_page_render(uint16_t *fb);
bool pk_airband_recordings_page_touch(int x, int y);
bool pk_airband_recordings_page_drag(int x, int y);
void pk_airband_recordings_page_touch_up(void);
#endif
