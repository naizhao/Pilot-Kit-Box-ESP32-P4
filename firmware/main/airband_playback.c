#include "airband_playback.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "airband_audio.h"
#include "airband_bk4819.h"
#include "airband_playback_policy.h"
#include "airband_record.h"
#include "airband_speaker.h"
#include "pk_sdcard.h"

#define PLAYBACK_SOURCE_FRAMES 64u
#define PLAYBACK_SPEAKER_TIMEOUT_MS 250u
#define PLAYBACK_ACTIVE_DELAY_MS 1u
#define PLAYBACK_IDLE_DELAY_MS 20u
#define PLAYBACK_STDIO_BUFFER_BYTES 4096u

#ifdef PK_AIRBAND_PLAYBACK_TEST_HOOKS
extern bool pk_airband_playback_test_before_read(void);
extern void pk_airband_playback_test_before_submit(void);
extern void pk_airband_playback_test_before_render(void);
extern void pk_airband_playback_test_after_find(void);
#endif

static const char *TAG = "airband_play";
static SemaphoreHandle_t s_lock;
static FILE *s_fp;
static uint32_t s_fp_generation;
static uint32_t s_fp_cursor_bytes = UINT32_MAX;
static uint8_t s_fp_buffer[PLAYBACK_STDIO_BUFFER_BYTES];
static pk_airband_audio_route_t s_route;
static pk_airband_playback_status_t s_status;
static uint32_t s_data_offset = 44u;
/* Changes only when the selected content identity changes, not for pause.
 * This lets a block that really reached I2S commit its cursor even if the user
 * pressed pause while the speaker acknowledgement was returning. */
static uint32_t s_selection_generation;
static bool s_unmount_registered;
#ifdef PK_AIRBAND_PLAYBACK_TEST_HOOKS
static bool s_test_start_failure;
static uint32_t s_test_seek_count;
#endif

static bool same_id(pk_airband_recording_id_t a, pk_airband_recording_id_t b)
{
    return a.session == b.session && a.volume == b.volume &&
           a.interrupted == b.interrupted;
}

static void close_file_locked(void)
{
    if (s_fp != NULL) {
        if (fclose(s_fp) != 0) {
            s_status.io_errors++;
            s_status.pause_reason = PK_AIRBAND_PLAYBACK_PAUSE_IO_ERROR;
        }
        s_fp = NULL;
        s_fp_generation = 0;
        s_fp_cursor_bytes = UINT32_MAX;
    }
    pk_airband_audio_route_reset(&s_route);
}

static void set_pause_locked(pk_airband_playback_pause_reason_t reason)
{
    s_status.playing = false;
    s_status.pause_reason = reason;
    if (reason == PK_AIRBAND_PLAYBACK_PAUSE_LIVE)
        s_status.live_preemptions++;
    else if (reason == PK_AIRBAND_PLAYBACK_PAUSE_RECORD_PRESSURE)
        s_status.pressure_pauses++;
    else if (reason == PK_AIRBAND_PLAYBACK_PAUSE_IO_ERROR ||
             reason == PK_AIRBAND_PLAYBACK_PAUSE_FILE_CHANGED)
        s_status.io_errors++;
}

static void pre_unmount(void)
{
    pk_airband_speaker_playback_stop();
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    close_file_locked();
    s_selection_generation++;
    s_status.selected = false;
    set_pause_locked(PK_AIRBAND_PLAYBACK_PAUSE_SD_UNAVAILABLE);
    xSemaphoreGive(s_lock);
}

static pk_airband_playback_pause_reason_t reason_from_decision(
    pk_airband_playback_decision_t decision)
{
    switch (decision) {
    case PK_AIRBAND_PLAYBACK_DECISION_PAUSE_USER:
        return PK_AIRBAND_PLAYBACK_PAUSE_USER;
    case PK_AIRBAND_PLAYBACK_DECISION_PAUSE_LIVE:
        return PK_AIRBAND_PLAYBACK_PAUSE_LIVE;
    case PK_AIRBAND_PLAYBACK_DECISION_PAUSE_RECORD_PRESSURE:
        return PK_AIRBAND_PLAYBACK_PAUSE_RECORD_PRESSURE;
    case PK_AIRBAND_PLAYBACK_DECISION_PAUSE_TEST_TONE:
        return PK_AIRBAND_PLAYBACK_PAUSE_TEST_TONE;
    case PK_AIRBAND_PLAYBACK_DECISION_PLAY:
    default:
        return PK_AIRBAND_PLAYBACK_PAUSE_NONE;
    }
}

static bool read_block(uint32_t generation, int16_t pcm[PLAYBACK_SOURCE_FRAMES],
                       size_t *frames)
{
    *frames = 0;
    if (!pk_sdcard_io_acquire()) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_status.selected || !s_status.playing ||
        generation != s_selection_generation ||
        s_status.mount_generation != pk_sdcard_mount_generation()) {
        xSemaphoreGive(s_lock);
        pk_sdcard_io_release();
        return false;
    }

    bool ok = true;
    if (s_fp != NULL && s_fp_generation != generation)
        close_file_locked();
    if (s_fp == NULL) {
        pk_airband_recording_entry_t entry;
        if (!pk_airband_recording_store_find(s_status.id, &entry) ||
            entry.wav.data_bytes != s_status.data_bytes) {
            set_pause_locked(PK_AIRBAND_PLAYBACK_PAUSE_FILE_CHANGED);
            ok = false;
        } else {
            s_fp = fopen(entry.path, "rb");
            struct stat st;
            if (s_fp == NULL || fstat(fileno(s_fp), &st) != 0 ||
                (uint64_t)st.st_size != entry.file_bytes ||
                setvbuf(s_fp, (char *)s_fp_buffer, _IOFBF,
                        sizeof(s_fp_buffer)) != 0) {
                close_file_locked();
                set_pause_locked(PK_AIRBAND_PLAYBACK_PAUSE_FILE_CHANGED);
                ok = false;
            } else {
                s_fp_generation = generation;
                s_fp_cursor_bytes = UINT32_MAX;
            }
        }
    }
    if (ok) {
        const uint32_t remaining = s_status.data_bytes - s_status.cursor_bytes;
        const size_t wanted = remaining / sizeof(int16_t) < PLAYBACK_SOURCE_FRAMES
            ? remaining / sizeof(int16_t) : PLAYBACK_SOURCE_FRAMES;
        if (wanted == 0) {
            set_pause_locked(PK_AIRBAND_PLAYBACK_PAUSE_EOF);
        } else if (s_fp_cursor_bytes != s_status.cursor_bytes &&
                   (
#ifdef PK_AIRBAND_PLAYBACK_TEST_HOOKS
                    s_test_seek_count++,
#endif
                    fseek(s_fp, (long)(s_data_offset + s_status.cursor_bytes),
                          SEEK_SET)) != 0) {
            set_pause_locked(PK_AIRBAND_PLAYBACK_PAUSE_IO_ERROR);
            ok = false;
        } else {
#ifdef PK_AIRBAND_PLAYBACK_TEST_HOOKS
            const bool force_short_read = pk_airband_playback_test_before_read();
            *frames = force_short_read
                ? 0 : fread(pcm, sizeof(int16_t), wanted, s_fp);
#else
            *frames = fread(pcm, sizeof(int16_t), wanted, s_fp);
#endif
            if (*frames != wanted) {
                set_pause_locked(ferror(s_fp)
                    ? PK_AIRBAND_PLAYBACK_PAUSE_IO_ERROR
                    : PK_AIRBAND_PLAYBACK_PAUSE_FILE_CHANGED);
                ok = false;
                *frames = 0;
            } else {
                s_fp_cursor_bytes = s_status.cursor_bytes +
                                    (uint32_t)(*frames * sizeof(int16_t));
            }
        }
    }
    xSemaphoreGive(s_lock);
    pk_sdcard_io_release();
    return ok && *frames != 0;
}

static void poll_once(void)
{
    if (s_lock == NULL) return;

    pk_airband_record_status_t record = {0};
    pk_airband_speaker_status_t speaker = {0};
    pk_airband_record_get(&record);
    (void)pk_airband_speaker_get(&speaker);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_status.selected) {
        const bool need_close = s_fp != NULL;
        xSemaphoreGive(s_lock);
        if (need_close && pk_sdcard_io_acquire()) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            close_file_locked();
            xSemaphoreGive(s_lock);
            pk_sdcard_io_release();
        }
        return;
    }
    const pk_airband_playback_decision_t decision = pk_airband_playback_decide(
        s_status.playing, speaker.program_playable,
        record.queued_blocks, speaker.tone_active);
    if (decision != PK_AIRBAND_PLAYBACK_DECISION_PLAY) {
        /* A previously explicit/automatic pause keeps its exact reason. */
        if (s_status.playing)
            set_pause_locked(reason_from_decision(decision));
        xSemaphoreGive(s_lock);
        if (decision != PK_AIRBAND_PLAYBACK_DECISION_PAUSE_USER)
            pk_airband_speaker_playback_stop();
        return;
    }
    const uint32_t generation = s_selection_generation;
    xSemaphoreGive(s_lock);

    int16_t mono[PLAYBACK_SOURCE_FRAMES];
    int16_t stereo[PK_AIRBAND_I2S_DMA_FRAME_NUM * 2u];
    size_t source_frames = 0;
    if (!read_block(generation, mono, &source_frames)) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool paused = s_status.selected && !s_status.playing;
        xSemaphoreGive(s_lock);
        if (paused) pk_airband_speaker_playback_stop();
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool still_current = s_status.selected && s_status.playing &&
                               generation == s_selection_generation;
    xSemaphoreGive(s_lock);
    if (!still_current) return;

#ifdef PK_AIRBAND_PLAYBACK_TEST_HOOKS
    pk_airband_playback_test_before_render();
#endif
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint32_t speaker_token = pk_airband_speaker_playback_token();
    const pk_airband_audio_route_t route_before = s_route;
    const size_t rendered = generation == s_selection_generation &&
                            s_status.selected && s_status.playing
        ? pk_airband_audio_route16(
              &s_route, mono, source_frames, true, false, stereo,
              PK_AIRBAND_I2S_DMA_FRAME_NUM)
        : 0;
    xSemaphoreGive(s_lock);
    if (rendered == 0) {
        bool stop = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (generation == s_selection_generation && s_status.selected &&
            s_status.playing) {
            set_pause_locked(PK_AIRBAND_PLAYBACK_PAUSE_IO_ERROR);
            stop = true;
        }
        xSemaphoreGive(s_lock);
        if (stop) pk_airband_speaker_playback_stop();
        return;
    }
#ifdef PK_AIRBAND_PLAYBACK_TEST_HOOKS
    pk_airband_playback_test_before_submit();
#endif
    const pk_airband_speaker_playback_result_t result =
        pk_airband_speaker_playback_submit_checked(
            stereo, rendered, PLAYBACK_SPEAKER_TIMEOUT_MS, speaker_token);

    bool stop_after = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (generation == s_selection_generation && s_status.selected) {
        if (pk_airband_playback_result_commits_cursor(result)) {
            s_status.cursor_bytes += (uint32_t)(source_frames * sizeof(int16_t));
            s_status.cursor_ms = (uint32_t)(
                (uint64_t)s_status.cursor_bytes * 1000u / 32000u);
            s_status.blocks_written++;
            if (s_status.cursor_bytes == s_status.data_bytes) {
                set_pause_locked(PK_AIRBAND_PLAYBACK_PAUSE_EOF);
                stop_after = true;
            }
        } else if (result == PK_AIRBAND_SPEAKER_PLAYBACK_LIVE_PREEMPTED) {
            s_route = route_before;
            set_pause_locked(PK_AIRBAND_PLAYBACK_PAUSE_LIVE);
        } else if (result == PK_AIRBAND_SPEAKER_PLAYBACK_TONE_BUSY) {
            s_route = route_before;
            set_pause_locked(PK_AIRBAND_PLAYBACK_PAUSE_TEST_TONE);
        } else if (result == PK_AIRBAND_SPEAKER_PLAYBACK_STOPPED) {
            s_route = route_before;
            set_pause_locked(PK_AIRBAND_PLAYBACK_PAUSE_USER);
        } else {
            s_route = route_before;
            set_pause_locked(PK_AIRBAND_PLAYBACK_PAUSE_SPEAKER_ERROR);
        }
    }
    xSemaphoreGive(s_lock);
    if (stop_after) pk_airband_speaker_playback_stop();
}

static void playback_task(void *arg)
{
    (void)arg;
    for (;;) {
        poll_once();
        pk_airband_playback_status_t status;
        (void)pk_airband_playback_get(&status);
        vTaskDelay(pdMS_TO_TICKS(status.playing
            ? PLAYBACK_ACTIVE_DELAY_MS : PLAYBACK_IDLE_DELAY_MS));
    }
}

bool pk_airband_playback_start(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            s_status.service_state =
                PK_AIRBAND_PLAYBACK_SERVICE_START_FAILED;
            return false;
        }
        pk_airband_audio_route_reset(&s_route);
    }
    if (!s_unmount_registered) {
        if (!pk_sdcard_register_pre_unmount_cb(pre_unmount)) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.service_state =
                PK_AIRBAND_PLAYBACK_SERVICE_START_FAILED;
            xSemaphoreGive(s_lock);
            return false;
        }
        s_unmount_registered = true;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.started) {
        xSemaphoreGive(s_lock);
        return true;
    }
    xSemaphoreGive(s_lock);
#ifdef PK_AIRBAND_PLAYBACK_TEST_HOOKS
    const bool task_failed = s_test_start_failure;
#else
    const bool task_failed = false;
#endif
    if (task_failed ||
        xTaskCreate(playback_task, "airband_play", 5120, NULL, 2, NULL) != pdPASS) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.service_state = PK_AIRBAND_PLAYBACK_SERVICE_START_FAILED;
        xSemaphoreGive(s_lock);
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status.started = true;
    s_status.service_state = PK_AIRBAND_PLAYBACK_SERVICE_READY;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "history playback worker started");
    return true;
}

pk_airband_playback_select_result_t pk_airband_playback_select_ex(
    pk_airband_recording_id_t id)
{
    if (s_lock == NULL)
        return PK_AIRBAND_PLAYBACK_SELECT_SERVICE_UNAVAILABLE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool started = s_status.started;
    xSemaphoreGive(s_lock);
    if (!started)
        return PK_AIRBAND_PLAYBACK_SELECT_SERVICE_UNAVAILABLE;
    if (!pk_sdcard_io_acquire())
        return PK_AIRBAND_PLAYBACK_SELECT_SD_UNAVAILABLE;
    const uint32_t mount_generation = pk_sdcard_mount_generation();
    pk_airband_recording_entry_t entry;
    if (!pk_airband_recording_store_find(id, &entry)) {
        pk_sdcard_io_release();
        return PK_AIRBAND_PLAYBACK_SELECT_FILE_CHANGED;
    }
#ifdef PK_AIRBAND_PLAYBACK_TEST_HOOKS
    pk_airband_playback_test_after_find();
#endif
    pk_airband_speaker_playback_stop();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!pk_sdcard_is_mounted() ||
        mount_generation != pk_sdcard_mount_generation()) {
        xSemaphoreGive(s_lock);
        pk_sdcard_io_release();
        return PK_AIRBAND_PLAYBACK_SELECT_SD_UNAVAILABLE;
    }
    if (!s_status.selected || !same_id(s_status.id, id)) {
        s_selection_generation++;
        s_status.cursor_bytes = 0;
        s_status.cursor_ms = 0;
        pk_airband_audio_route_reset(&s_route);
    }
    s_status.selected = true;
    s_status.playing = false;
    s_status.id = id;
    s_status.mount_generation = mount_generation;
    s_status.data_bytes = entry.wav.data_bytes;
    s_data_offset = entry.wav.data_offset;
    s_status.duration_ms = entry.wav.duration_ms;
    s_status.pause_reason = PK_AIRBAND_PLAYBACK_PAUSE_USER;
    xSemaphoreGive(s_lock);
    pk_sdcard_io_release();
    return PK_AIRBAND_PLAYBACK_SELECT_OK;
}

bool pk_airband_playback_select(pk_airband_recording_id_t id)
{
    return pk_airband_playback_select_ex(id) ==
           PK_AIRBAND_PLAYBACK_SELECT_OK;
}

void pk_airband_playback_toggle(void)
{
    if (s_lock == NULL) return;
    bool stopping = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.selected) {
        if (s_status.playing) {
            set_pause_locked(PK_AIRBAND_PLAYBACK_PAUSE_USER);
            stopping = true;
        } else {
            if (s_status.cursor_bytes >= s_status.data_bytes) {
                s_status.cursor_bytes = 0;
                s_status.cursor_ms = 0;
                pk_airband_audio_route_reset(&s_route);
            }
            s_status.playing = true;
            s_status.pause_reason = PK_AIRBAND_PLAYBACK_PAUSE_NONE;
        }
    }
    xSemaphoreGive(s_lock);
    if (stopping) pk_airband_speaker_playback_stop();
}

void pk_airband_playback_pause(pk_airband_playback_pause_reason_t reason)
{
    if (s_lock == NULL) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.selected)
        set_pause_locked(reason == PK_AIRBAND_PLAYBACK_PAUSE_NONE
            ? PK_AIRBAND_PLAYBACK_PAUSE_USER : reason);
    xSemaphoreGive(s_lock);
    pk_airband_speaker_playback_stop();
}

void pk_airband_playback_stop(void)
{
    if (s_lock == NULL) return;
    pk_airband_speaker_playback_stop();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_selection_generation++;
    s_status.selected = false;
    s_status.playing = false;
    s_status.cursor_bytes = 0;
    s_status.cursor_ms = 0;
    s_status.data_bytes = 0;
    s_status.duration_ms = 0;
    s_status.pause_reason = PK_AIRBAND_PLAYBACK_PAUSE_USER;
    xSemaphoreGive(s_lock);
}

bool pk_airband_playback_get(pk_airband_playback_status_t *out)
{
    if (out == NULL) return false;
    if (s_lock == NULL) {
        *out = (pk_airband_playback_status_t){
            .service_state = PK_AIRBAND_PLAYBACK_SERVICE_NOT_STARTED,
        };
        return true;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
    return true;
}

#ifdef PK_AIRBAND_PLAYBACK_TEST_HOOKS
void pk_airband_playback_test_poll(void) { poll_once(); }
int pk_airband_playback_test_fd(void)
{
    if (s_lock == NULL) return -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int fd = s_fp ? fileno(s_fp) : -1;
    xSemaphoreGive(s_lock);
    return fd;
}
void pk_airband_playback_test_set_start_failure(bool fail)
{
    s_test_start_failure = fail;
}
uint32_t pk_airband_playback_test_seek_count(void)
{
    return s_test_seek_count;
}
#endif
