/*
 * cc -std=c11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE \
 *   -DPK_AIRBAND_PLAYBACK_TEST_HOOKS \
 *   -Ifirmware/test/record_stubs -Ifirmware/test/host_stubs -Ifirmware/main \
 *   -o /tmp/test_airband_playback firmware/test/test_airband_playback.c \
 *   firmware/main/airband_playback.c firmware/main/airband_audio.c \
 *   firmware/main/airband_playback_policy.c -pthread && \
 *   /tmp/test_airband_playback
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "airband_playback.h"
#include "airband_record.h"
#include "airband_speaker.h"
#include "pk_sdcard.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s (%d)\n", m, __LINE__); failures++; } } while (0)

static char root[128];
static pk_airband_recording_entry_t fixture_a, fixture_b;
static atomic_bool mounted = true;
static uint32_t mount_generation = 7;
static atomic_uint leases;
static atomic_bool removal_announced;
static void (*unmount_cb)(void);
static bool register_ok = true;
static pk_airband_record_status_t record_status;
static pk_airband_speaker_status_t speaker_status = {.online = true};
static pk_airband_speaker_playback_result_t submit_result =
    PK_AIRBAND_SPEAKER_PLAYBACK_WRITTEN;
static unsigned submit_calls;
static int16_t submitted_first_sample;
static int16_t submitted_last_sample;
static int16_t submitted_pcm[192u * 2u];
static unsigned speaker_stop_calls;
static atomic_uint speaker_generation = 1;
static pthread_mutex_t read_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t read_cond = PTHREAD_COND_INITIALIZER;
static bool stall_read, read_entered, release_read, force_short_read;
static pthread_mutex_t submit_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t submit_cond = PTHREAD_COND_INITIALIZER;
static bool stall_submit, submit_entered, release_submit;
static pthread_mutex_t render_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t render_cond = PTHREAD_COND_INITIALIZER;
static bool stall_render, render_entered, release_render;
static pthread_mutex_t find_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t find_cond = PTHREAD_COND_INITIALIZER;
static bool stall_find, find_entered, release_find;
static bool select_thread_result;

bool pk_airband_playback_test_before_read(void)
{
    pthread_mutex_lock(&read_lock);
    if (stall_read) {
        read_entered = true;
        pthread_cond_broadcast(&read_cond);
        while (!release_read) pthread_cond_wait(&read_cond, &read_lock);
    }
    pthread_mutex_unlock(&read_lock);
    return force_short_read;
}

void pk_airband_playback_test_before_submit(void)
{
    pthread_mutex_lock(&submit_lock);
    if (stall_submit) {
        submit_entered = true;
        pthread_cond_broadcast(&submit_cond);
        while (!release_submit) pthread_cond_wait(&submit_cond, &submit_lock);
    }
    pthread_mutex_unlock(&submit_lock);
}

void pk_airband_playback_test_before_render(void)
{
    pthread_mutex_lock(&render_lock);
    if (stall_render) {
        render_entered = true;
        pthread_cond_broadcast(&render_cond);
        while (!release_render) pthread_cond_wait(&render_cond, &render_lock);
    }
    pthread_mutex_unlock(&render_lock);
}

void pk_airband_playback_test_after_find(void)
{
    pthread_mutex_lock(&find_lock);
    if (stall_find) {
        find_entered = true;
        pthread_cond_broadcast(&find_cond);
        while (!release_find) pthread_cond_wait(&find_cond, &find_lock);
    }
    pthread_mutex_unlock(&find_lock);
}

bool pk_airband_recording_store_find(pk_airband_recording_id_t id,
                                     pk_airband_recording_entry_t *out)
{
    if (id.session == fixture_a.id.session && id.volume == fixture_a.id.volume &&
        id.interrupted == fixture_a.id.interrupted) { *out = fixture_a; return true; }
    if (id.session == fixture_b.id.session && id.volume == fixture_b.id.volume &&
        id.interrupted == fixture_b.id.interrupted) { *out = fixture_b; return true; }
    return false;
}

bool pk_sdcard_is_mounted(void) { return atomic_load(&mounted); }
uint32_t pk_sdcard_mount_generation(void) { return mount_generation; }
const char *pk_sdcard_mount_point(void) { return root; }
bool pk_sdcard_io_acquire(void)
{
    if (!atomic_load(&mounted)) return false;
    atomic_fetch_add(&leases, 1);
    return true;
}
void pk_sdcard_io_release(void)
{
    CHECK(atomic_load(&leases) > 0, "lease underflow");
    atomic_fetch_sub(&leases, 1);
}
bool pk_sdcard_register_pre_unmount_cb(void (*cb)(void))
{ if (!register_ok) return false; unmount_cb = cb; return true; }

void pk_airband_record_get(pk_airband_record_status_t *out) { *out = record_status; }
bool pk_airband_speaker_get(pk_airband_speaker_status_t *out)
{ *out = speaker_status; return true; }
pk_airband_speaker_playback_result_t pk_airband_speaker_playback_submit(
    const int16_t *stereo, size_t frames, uint32_t timeout_ms)
{
    return pk_airband_speaker_playback_submit_checked(
        stereo, frames, timeout_ms, atomic_load(&speaker_generation));
}
uint32_t pk_airband_speaker_playback_token(void)
{ return atomic_load(&speaker_generation); }
pk_airband_speaker_playback_result_t pk_airband_speaker_playback_submit_checked(
    const int16_t *stereo, size_t frames, uint32_t timeout_ms,
    uint32_t expected_generation)
{
    if (expected_generation != atomic_load(&speaker_generation))
        return PK_AIRBAND_SPEAKER_PLAYBACK_STOPPED;
    CHECK(frames == 192, "one 16 kHz block must become 192 stereo frames");
    CHECK(timeout_ms > 0, "speaker submit must be bounded");
    submit_calls++;
    submitted_first_sample = stereo[0];
    submitted_last_sample = stereo[(frames - 1u) * 2u];
    memcpy(submitted_pcm, stereo, frames * 2u * sizeof(*stereo));
    return submit_result;
}
void pk_airband_speaker_playback_stop(void)
{ speaker_stop_calls++; atomic_fetch_add(&speaker_generation, 1); }

static void *run_poll(void *unused)
{ (void)unused; pk_airband_playback_test_poll(); return NULL; }

static void *run_remove(void *unused)
{
    (void)unused;
    atomic_store(&mounted, false);
    atomic_store(&removal_announced, true);
    while (atomic_load(&leases) != 0) sched_yield();
    unmount_cb();
    return NULL;
}

static void *run_select(void *unused)
{
    (void)unused;
    select_thread_result = pk_airband_playback_select(fixture_a.id);
    return NULL;
}

static void put16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static bool write_fixture(const char *path, unsigned frames, int16_t base)
{
    const uint32_t bytes = frames * 2u;
    uint8_t h[44] = {0};
    memcpy(h, "RIFF", 4); put32(h + 4, 36u + bytes);
    memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16u);
    put16(h + 20, 1); put16(h + 22, 1); put32(h + 24, 16000);
    put32(h + 28, 32000); put16(h + 32, 2); put16(h + 34, 16);
    memcpy(h + 36, "data", 4); put32(h + 40, bytes);
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    bool ok = fwrite(h, 1, sizeof(h), fp) == sizeof(h);
    for (unsigned i = 0; ok && i < frames; ++i) {
        const int16_t sample = (int16_t)(base + (int16_t)i);
        ok = fwrite(&sample, sizeof(sample), 1, fp) == 1;
    }
    return fclose(fp) == 0 && ok;
}

static pk_airband_playback_status_t status(void)
{
    pk_airband_playback_status_t out = {0};
    CHECK(pk_airband_playback_get(&out), "playback status available");
    return out;
}

static void select_and_start(void)
{
    CHECK(pk_airband_playback_select(fixture_a.id), "select fixture");
    pk_airband_playback_toggle();
    CHECK(status().playing, "toggle starts selected recording");
}

static void test_written_advances_but_preempted_replays_same_block(void)
{
    select_and_start();
    submit_result = PK_AIRBAND_SPEAKER_PLAYBACK_WRITTEN;
    pk_airband_playback_test_poll();
    CHECK(status().cursor_bytes == 128, "written 64-sample block advances cursor");
    CHECK(submitted_first_sample == 0, "playback route starts with attack ramp");

    submit_result = PK_AIRBAND_SPEAKER_PLAYBACK_LIVE_PREEMPTED;
    pk_airband_playback_test_poll();
    pk_airband_playback_status_t paused = status();
    CHECK(paused.cursor_bytes == 128, "preempted block must not advance cursor");
    CHECK(!paused.playing && paused.pause_reason == PK_AIRBAND_PLAYBACK_PAUSE_LIVE,
          "live preemption requires explicit user resume");

    submit_result = PK_AIRBAND_SPEAKER_PLAYBACK_WRITTEN;
    pk_airband_playback_toggle();
    pk_airband_playback_test_poll();
    CHECK(status().cursor_bytes == 256, "manual resume retries the same block once");
}

static void test_contiguous_playback_does_not_seek_every_four_ms(void)
{
    pk_airband_playback_stop(); pk_airband_playback_test_poll();
    select_and_start();
    submit_result = PK_AIRBAND_SPEAKER_PLAYBACK_WRITTEN;
    const uint32_t before = pk_airband_playback_test_seek_count();
    pk_airband_playback_test_poll();
    pk_airband_playback_test_poll();
    CHECK(pk_airband_playback_test_seek_count() == before + 1,
          "contiguous 4 ms blocks must share one sequential buffered read");

    submit_result = PK_AIRBAND_SPEAKER_PLAYBACK_LIVE_PREEMPTED;
    pk_airband_playback_test_poll();
    submit_result = PK_AIRBAND_SPEAKER_PLAYBACK_WRITTEN;
    pk_airband_playback_toggle();
    pk_airband_playback_test_poll();
    CHECK(pk_airband_playback_test_seek_count() == before + 2,
          "an uncommitted block must seek back exactly once before retry");
}

static void test_uncommitted_block_retries_identical_pcm(void)
{
    pk_airband_playback_stop(); pk_airband_playback_test_poll();
    select_and_start();
    submit_result = PK_AIRBAND_SPEAKER_PLAYBACK_LIVE_PREEMPTED;
    pk_airband_playback_test_poll();
    int16_t first_attempt[192u * 2u];
    memcpy(first_attempt, submitted_pcm, sizeof(first_attempt));
    CHECK(status().cursor_bytes == 0, "preempted first block keeps file cursor");

    submit_result = PK_AIRBAND_SPEAKER_PLAYBACK_WRITTEN;
    pk_airband_playback_toggle();
    pk_airband_playback_test_poll();
    CHECK(memcmp(first_attempt, submitted_pcm, sizeof(first_attempt)) == 0,
          "retry of an uncommitted block must restore the DSP route state");
}

static void test_record_pressure_uses_current_queue_boundary(void)
{
    pk_airband_playback_stop();
    pk_airband_playback_test_poll();
    record_status.queued_blocks = 127;
    select_and_start();
    unsigned before = submit_calls;
    pk_airband_playback_test_poll();
    CHECK(submit_calls == before + 1, "127 queued blocks still permits playback");

    record_status.queued_blocks = 128;
    pk_airband_playback_test_poll();
    CHECK(submit_calls == before + 1, "128 queued blocks pauses before speaker submit");
    CHECK(!status().playing &&
              status().pause_reason == PK_AIRBAND_PLAYBACK_PAUSE_RECORD_PRESSURE,
          "record pressure pause reason must be observable");
    record_status.queued_blocks = 0;
}

static void test_live_gate_before_read_does_not_touch_cursor(void)
{
    pk_airband_playback_stop(); pk_airband_playback_test_poll();
    select_and_start();
    speaker_status.playback_active = false;
    speaker_status.tone_active = false;
    speaker_status.program_playable = true;
    unsigned before = submit_calls;
    pk_airband_playback_test_poll();
    CHECK(submit_calls == before, "known live audio pauses before historical submit");
    CHECK(status().cursor_bytes == 0 &&
              status().pause_reason == PK_AIRBAND_PLAYBACK_PAUSE_LIVE,
          "early live gate preserves selected-file cursor");
    speaker_status.program_playable = false;
}

static void test_switching_selection_reopens_the_new_file(void)
{
    pk_airband_playback_stop(); pk_airband_playback_test_poll();
    select_and_start();
    submit_result = PK_AIRBAND_SPEAKER_PLAYBACK_WRITTEN;
    pk_airband_playback_test_poll();
    const int16_t a_tail = submitted_last_sample;

    CHECK(pk_airband_playback_select(fixture_b.id), "select second recording");
    pk_airband_playback_toggle();
    pk_airband_playback_test_poll();
    CHECK(status().id.session == fixture_b.id.session &&
              status().cursor_bytes == 128,
          "new selection has its own cursor");
    CHECK(submitted_last_sample > a_tail + 2000,
          "speaker PCM comes from the newly selected file, not the old fd");
}

static void test_pause_between_render_and_submit_cancels_the_block(void)
{
    pk_airband_playback_stop(); pk_airband_playback_test_poll();
    select_and_start();
    pthread_mutex_lock(&submit_lock);
    stall_submit = true; submit_entered = false; release_submit = false;
    pthread_mutex_unlock(&submit_lock);
    const unsigned before = submit_calls;
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL, run_poll, NULL) == 0,
          "start render-to-submit race worker");
    pthread_mutex_lock(&submit_lock);
    while (!submit_entered) pthread_cond_wait(&submit_cond, &submit_lock);
    pthread_mutex_unlock(&submit_lock);
    pk_airband_playback_pause(PK_AIRBAND_PLAYBACK_PAUSE_USER);
    pthread_mutex_lock(&submit_lock);
    release_submit = true; pthread_cond_broadcast(&submit_cond);
    pthread_mutex_unlock(&submit_lock);
    pthread_join(worker, NULL);
    stall_submit = false;
    CHECK(submit_calls == before && status().cursor_bytes == 0,
          "pause invalidates the rendered block before speaker acceptance");
}

static void test_old_worker_cannot_pause_a_new_selection(void)
{
    pk_airband_playback_stop(); pk_airband_playback_test_poll();
    select_and_start();
    pthread_mutex_lock(&render_lock);
    stall_render = true; render_entered = false; release_render = false;
    pthread_mutex_unlock(&render_lock);
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL, run_poll, NULL) == 0,
          "start stale worker race");
    pthread_mutex_lock(&render_lock);
    while (!render_entered) pthread_cond_wait(&render_cond, &render_lock);
    pthread_mutex_unlock(&render_lock);
    CHECK(pk_airband_playback_select(fixture_b.id), "select B during old A render");
    pk_airband_playback_toggle();
    pthread_mutex_lock(&render_lock);
    release_render = true; pthread_cond_broadcast(&render_cond);
    pthread_mutex_unlock(&render_lock);
    pthread_join(worker, NULL); stall_render = false;
    pk_airband_playback_status_t current = status();
    CHECK(current.id.session == fixture_b.id.session && current.playing &&
              current.pause_reason == PK_AIRBAND_PLAYBACK_PAUSE_NONE &&
              current.cursor_bytes == 0,
          "stale A worker cannot mark newly selected B as IO_ERROR");
}

static void test_select_is_serialized_with_pre_unmount(void)
{
    pk_airband_playback_stop(); pk_airband_playback_test_poll();
    pthread_mutex_lock(&find_lock);
    stall_find = true; find_entered = false; release_find = false;
    pthread_mutex_unlock(&find_lock);
    pthread_t selector, remover;
    CHECK(pthread_create(&selector, NULL, run_select, NULL) == 0,
          "start select/unmount race");
    pthread_mutex_lock(&find_lock);
    while (!find_entered) pthread_cond_wait(&find_cond, &find_lock);
    pthread_mutex_unlock(&find_lock);
    CHECK(atomic_load(&leases) == 1, "selection owns a lifecycle lease");
    CHECK(pthread_create(&remover, NULL, run_remove, NULL) == 0,
          "start unmount behind selection lease");
    while (!atomic_load(&removal_announced)) sched_yield();
    pthread_mutex_lock(&find_lock);
    release_find = true; pthread_cond_broadcast(&find_cond);
    pthread_mutex_unlock(&find_lock);
    pthread_join(selector, NULL); pthread_join(remover, NULL); stall_find = false;
    CHECK(!select_thread_result && !status().selected,
          "unmount wins without allowing stale selection to republish");
    atomic_store(&mounted, true); mount_generation++;
    atomic_store(&removal_announced, false);
}

static void test_eof_and_short_read_stop_the_speaker(void)
{
    pk_airband_playback_stop(); pk_airband_playback_test_poll();
    select_and_start();
    const unsigned eof_stops = speaker_stop_calls;
    for (unsigned i = 0; i < 4; ++i) pk_airband_playback_test_poll();
    CHECK(status().pause_reason == PK_AIRBAND_PLAYBACK_PAUSE_EOF &&
              speaker_stop_calls > eof_stops,
          "last acknowledged block transitions speaker/PA to stopped");

    pk_airband_playback_stop(); pk_airband_playback_test_poll();
    select_and_start(); pk_airband_playback_test_poll();
    force_short_read = true;
    const unsigned error_stops = speaker_stop_calls;
    pk_airband_playback_test_poll();
    force_short_read = false;
    CHECK(status().pause_reason == PK_AIRBAND_PLAYBACK_PAUSE_FILE_CHANGED &&
              speaker_stop_calls > error_stops,
          "short read pauses playback and stops speaker/PA");
    pk_airband_playback_stop(); pk_airband_playback_test_poll();
}

static void test_pre_unmount_closes_persistent_fd_and_releases_all_leases(void)
{
    pk_airband_playback_stop(); pk_airband_playback_test_poll();
    select_and_start();
    submit_result = PK_AIRBAND_SPEAKER_PLAYBACK_WRITTEN;
    pk_airband_playback_test_poll();
    const int fd = pk_airband_playback_test_fd();
    CHECK(fd >= 0 && fcntl(fd, F_GETFD) != -1, "playback keeps one validated fd open");
    CHECK(atomic_load(&leases) == 0,
          "persistent fd never retains an SD I/O lease between blocks");

    CHECK(unmount_cb != NULL, "pre-unmount callback registered");
    pthread_mutex_lock(&read_lock);
    stall_read = true; read_entered = false; release_read = false;
    pthread_mutex_unlock(&read_lock);
    pthread_t reader, remover;
    CHECK(pthread_create(&reader, NULL, run_poll, NULL) == 0,
          "start in-flight playback read");
    pthread_mutex_lock(&read_lock);
    while (!read_entered) pthread_cond_wait(&read_cond, &read_lock);
    pthread_mutex_unlock(&read_lock);
    CHECK(atomic_load(&leases) == 1,
          "in-flight playback read owns exactly one lease");
    CHECK(pthread_create(&remover, NULL, run_remove, NULL) == 0,
          "start card-removal barrier");
    sched_yield();
    CHECK(fcntl(fd, F_GETFD) != -1,
          "descriptor stays valid while the in-flight lease is held");
    pthread_mutex_lock(&read_lock);
    release_read = true; pthread_cond_broadcast(&read_cond);
    pthread_mutex_unlock(&read_lock);
    pthread_join(reader, NULL); pthread_join(remover, NULL);
    CHECK(atomic_load(&leases) == 0, "pre-unmount leaves no lease");
    CHECK(fcntl(fd, F_GETFD) == -1 && errno == EBADF,
          "pre-unmount synchronously closes the actual OS descriptor");
    CHECK(!status().selected &&
              status().pause_reason == PK_AIRBAND_PLAYBACK_PAUSE_SD_UNAVAILABLE,
          "removed media invalidates the selection");
    CHECK(speaker_stop_calls > 0, "pre-unmount stops queued speaker playback");
}

int main(void)
{
    strcpy(root, "/tmp/pk_airband_playback_XXXXXX");
    CHECK(mkdtemp(root) != NULL, "temporary playback root");
    char path_a[320], path_b[320];
    snprintf(path_a, sizeof(path_a), "%s/s0000000042-c121500-f121500000-0000000003.wav", root);
    snprintf(path_b, sizeof(path_b), "%s/s0000000043-c121500-f121500000-0000000001.wav", root);
    CHECK(write_fixture(path_a, 256, 1000), "first playback WAV fixture");
    CHECK(write_fixture(path_b, 256, 6000), "second playback WAV fixture");
    fixture_a = (pk_airband_recording_entry_t){
        .id = {.session = 42, .volume = 3, .interrupted = false},
        .wav = {.data_offset = 44, .data_bytes = 512, .sample_frames = 256,
                .duration_ms = 16},
        .file_bytes = 556,
    };
    fixture_b = fixture_a;
    fixture_b.id = (pk_airband_recording_id_t){.session = 43, .volume = 1};
    snprintf(fixture_a.path, sizeof(fixture_a.path), "%s", path_a);
    snprintf(fixture_b.path, sizeof(fixture_b.path), "%s", path_b);
    CHECK(status().service_state == PK_AIRBAND_PLAYBACK_SERVICE_NOT_STARTED,
          "playback status distinguishes a service that was never started");
    register_ok = false;
    CHECK(!pk_airband_playback_start() && !pk_airband_playback_start(),
          "failed pre-unmount registration cannot be bypassed by retry");
    CHECK(!status().started &&
              status().service_state == PK_AIRBAND_PLAYBACK_SERVICE_START_FAILED,
          "registration failure exposes service unavailable state");
    CHECK(pk_airband_playback_select_ex(fixture_a.id) ==
              PK_AIRBAND_PLAYBACK_SELECT_SERVICE_UNAVAILABLE,
          "selection reports unavailable worker instead of file changed");
    CHECK(!pk_airband_playback_select(fixture_a.id),
          "failed playback service rejects public selection");
    register_ok = true;
    pk_airband_playback_test_set_start_failure(true);
    CHECK(!pk_airband_playback_start(), "worker creation failure is reported");
    CHECK(!status().started &&
              status().service_state == PK_AIRBAND_PLAYBACK_SERVICE_START_FAILED,
          "worker creation failure remains observable to the page");
    CHECK(!pk_airband_playback_select(fixture_a.id),
          "unavailable playback worker rejects selection without changing state");
    pk_airband_playback_test_set_start_failure(false);
    CHECK(pk_airband_playback_start(), "playback worker initialization");
    CHECK(status().service_state == PK_AIRBAND_PLAYBACK_SERVICE_READY,
          "successful retry exposes ready playback service");
    pk_airband_recording_id_t missing = fixture_a.id;
    missing.session++;
    CHECK(pk_airband_playback_select_ex(missing) ==
              PK_AIRBAND_PLAYBACK_SELECT_FILE_CHANGED,
          "missing stable ID is specifically reported as file changed");
    atomic_store(&mounted, false);
    CHECK(pk_airband_playback_select_ex(fixture_a.id) ==
              PK_AIRBAND_PLAYBACK_SELECT_SD_UNAVAILABLE,
          "SD lifecycle failure is not mislabeled file changed");
    atomic_store(&mounted, true);

    test_written_advances_but_preempted_replays_same_block();
    test_contiguous_playback_does_not_seek_every_four_ms();
    test_uncommitted_block_retries_identical_pcm();
    test_record_pressure_uses_current_queue_boundary();
    test_live_gate_before_read_does_not_touch_cursor();
    test_switching_selection_reopens_the_new_file();
    test_pause_between_render_and_submit_cancels_the_block();
    test_old_worker_cannot_pause_a_new_selection();
    test_eof_and_short_read_stop_the_speaker();
    test_select_is_serialized_with_pre_unmount();
    test_pre_unmount_closes_persistent_fd_and_releases_all_leases();

    unlink(path_a); unlink(path_b); rmdir(root);
    if (failures) return 1;
    puts("airband playback tests PASS");
    return 0;
}
