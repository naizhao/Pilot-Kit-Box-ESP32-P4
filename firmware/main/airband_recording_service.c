#include "airband_recording_store.h"

#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "pk_sdcard.h"

#define RECORDING_SCAN_INTERVAL_MS 2000u
#define RECORDING_SCAN_RETRY_US (10LL * 1000 * 1000)

static const char *TAG = "airband_catalog";
static SemaphoreHandle_t s_lock;
static pk_airband_recording_snapshot_t s_current;
static uint32_t s_generation;
static bool s_started;
static atomic_int s_catalog_state = PK_AIRBAND_RECORDING_CATALOG_NOT_STARTED;
static atomic_uint_fast32_t s_revision;
static bool s_stale;
static atomic_bool s_visible;
static atomic_uint_fast32_t s_refresh_requested;
/* Only the catalog worker owns scan/retry bookkeeping. Requests made during
 * a scan remain pending because we acknowledge its captured request token. */
static uint32_t s_refresh_serviced;
static uint32_t s_attempt_generation;
static bool s_retry_pending;
static int64_t s_retry_after_us;
#ifdef PK_AIRBAND_RECORDING_SERVICE_TEST_HOOKS
static bool s_test_oom;
static bool s_test_start_failure;
#endif

static void *psram_alloc(void *ctx, size_t bytes)
{
    (void)ctx;
#ifdef PK_AIRBAND_RECORDING_SERVICE_TEST_HOOKS
    if (s_test_oom) return NULL;
#endif
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
}

static void psram_release(void *ctx, void *ptr)
{
    (void)ctx;
    heap_caps_free(ptr);
}

static const pk_airband_recording_allocator_t s_psram_allocator = {
    .alloc = psram_alloc,
    .release = psram_release,
};

static bool same_id(pk_airband_recording_id_t a, pk_airband_recording_id_t b)
{
    return a.session == b.session && a.volume == b.volume &&
           a.interrupted == b.interrupted;
}

static bool same_entry(const pk_airband_recording_entry_t *a,
                       const pk_airband_recording_entry_t *b)
{
    return same_id(a->id, b->id) &&
           a->name.session == b->name.session &&
           a->name.channel_khz == b->name.channel_khz &&
           a->name.carrier_hz == b->name.carrier_hz &&
           a->name.volume == b->name.volume &&
           a->name.interrupted == b->name.interrupted &&
           a->wav.data_offset == b->wav.data_offset &&
           a->wav.data_bytes == b->wav.data_bytes &&
           a->wav.sample_frames == b->wav.sample_frames &&
           a->wav.duration_ms == b->wav.duration_ms &&
           a->file_bytes == b->file_bytes && a->mtime_sec == b->mtime_sec &&
           strcmp(a->filename, b->filename) == 0 &&
           strcmp(a->path, b->path) == 0;
}

static bool same_snapshot(const pk_airband_recording_snapshot_t *a,
                          const pk_airband_recording_snapshot_t *b)
{
    if (a->count != b->count || a->invalid_files != b->invalid_files ||
        a->ignored_parts != b->ignored_parts) return false;
    for (size_t i = 0; i < a->count; ++i)
        if (!same_entry(&a->entries[i], &b->entries[i])) return false;
    return true;
}

static void bump_revision(void)
{
    (void)atomic_fetch_add(&s_revision, 1u);
}

static void set_state_locked(pk_airband_recording_catalog_state_t state,
                             bool stale)
{
    const bool changed = atomic_load(&s_catalog_state) != (int)state ||
                         s_stale != stale;
    atomic_store(&s_catalog_state, state);
    s_stale = stale;
    if (changed) bump_revision();
}

static void clear_published(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pk_airband_recording_snapshot_t old = s_current;
    const bool changed = s_current.count != 0 || s_current.entries != NULL ||
                         s_generation != 0 ||
                         atomic_load(&s_catalog_state) !=
                             PK_AIRBAND_RECORDING_CATALOG_SD_UNAVAILABLE ||
                         s_stale;
    s_current = (pk_airband_recording_snapshot_t){0};
    s_generation = 0;
    atomic_store(&s_catalog_state, PK_AIRBAND_RECORDING_CATALOG_SD_UNAVAILABLE);
    s_stale = false;
    if (changed) bump_revision();
    xSemaphoreGive(s_lock);
    pk_airband_recording_snapshot_release(&old);
}

static void poll_once(void)
{
    if (s_lock == NULL) return;
    if (!pk_sdcard_is_mounted()) {
        clear_published();
        return;
    }
    if (!atomic_load(&s_visible)) return;
    const uint32_t generation = pk_sdcard_mount_generation();
    const uint32_t requested = (uint32_t)atomic_load(&s_refresh_requested);
    const int64_t now = esp_timer_get_time();
    if (s_retry_pending && generation == s_attempt_generation &&
        now < s_retry_after_us) return;
    if (!s_retry_pending && generation == s_generation &&
        requested == s_refresh_serviced) return;
    s_attempt_generation = generation;
    s_retry_pending = true;
    s_retry_after_us = now + RECORDING_SCAN_RETRY_US;
    if (!pk_sdcard_io_acquire()) return;
    char directory[PK_AIRBAND_RECORDING_PATH_MAX];
    const int used = snprintf(directory, sizeof(directory), "%s/airband",
                              pk_sdcard_mount_point());
    pk_airband_recording_snapshot_t next = {0};
    pk_airband_recording_scan_result_t result =
        used < 0 || (size_t)used >= sizeof(directory)
            ? PK_AIRBAND_RECORDING_SCAN_INVALID
            : pk_airband_recording_scan_directory(directory,
                                                   &s_psram_allocator, &next);
    pk_sdcard_io_release();

    if (result == PK_AIRBAND_RECORDING_SCAN_NOT_FOUND) {
        /* The recorder creates /airband lazily.  A mounted empty card is a
         * valid empty catalog, not an error banner. */
        next = (pk_airband_recording_snapshot_t){.allocator = s_psram_allocator};
        result = PK_AIRBAND_RECORDING_SCAN_OK;
    }
    if (result != PK_AIRBAND_RECORDING_SCAN_OK ||
        !pk_sdcard_is_mounted() ||
        generation != pk_sdcard_mount_generation()) {
        pk_airband_recording_snapshot_release(&next);
        if (!pk_sdcard_is_mounted()) {
            clear_published();
        } else {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            set_state_locked(result == PK_AIRBAND_RECORDING_SCAN_OOM
                                 ? PK_AIRBAND_RECORDING_CATALOG_OOM
                                 : PK_AIRBAND_RECORDING_CATALOG_SCAN_ERROR,
                             s_generation != 0);
            xSemaphoreGive(s_lock);
        }
        if (result != PK_AIRBAND_RECORDING_SCAN_CHANGED)
            ESP_LOGW(TAG, "catalog scan not published: result=%d", result);
        return;
    }

    s_retry_pending = false;
    s_refresh_serviced = requested;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool unchanged = s_generation == generation &&
                           atomic_load(&s_catalog_state) ==
                               PK_AIRBAND_RECORDING_CATALOG_READY &&
                           !s_stale && same_snapshot(&s_current, &next);
    pk_airband_recording_snapshot_t old = {0};
    if (!unchanged) {
        old = s_current;
        s_current = next;
        next = (pk_airband_recording_snapshot_t){0};
        s_generation = generation;
        atomic_store(&s_catalog_state, PK_AIRBAND_RECORDING_CATALOG_READY);
        s_stale = false;
        bump_revision();
    }
    xSemaphoreGive(s_lock);
    pk_airband_recording_snapshot_release(&old);
    pk_airband_recording_snapshot_release(&next);
}

static void catalog_task(void *arg)
{
    (void)arg;
    for (;;) {
        poll_once();
        vTaskDelay(pdMS_TO_TICKS(RECORDING_SCAN_INTERVAL_MS));
    }
}

bool pk_airband_recording_store_start(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            atomic_store(&s_catalog_state,
                         PK_AIRBAND_RECORDING_CATALOG_START_FAILED);
            return false;
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_started) {
        xSemaphoreGive(s_lock);
        return true;
    }
    set_state_locked(PK_AIRBAND_RECORDING_CATALOG_NOT_READY, false);
    xSemaphoreGive(s_lock);
#ifdef PK_AIRBAND_RECORDING_SERVICE_TEST_HOOKS
    const bool task_failed = s_test_start_failure;
#else
    const bool task_failed = false;
#endif
    if (task_failed ||
        xTaskCreate(catalog_task, "airband_catalog", 5120, NULL, 1, NULL) != pdPASS) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        set_state_locked(PK_AIRBAND_RECORDING_CATALOG_START_FAILED, false);
        xSemaphoreGive(s_lock);
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_started = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "recording catalog worker started");
    return true;
}

void pk_airband_recording_store_request_refresh(void)
{
    (void)atomic_fetch_add(&s_refresh_requested, 1u);
}

void pk_airband_recording_store_set_visible(bool visible)
{
    atomic_store(&s_visible, visible);
    if (visible) pk_airband_recording_store_request_refresh();
}

pk_airband_recording_snapshot_result_t
pk_airband_recording_store_snapshot_acquire_ex(
    pk_airband_recording_snapshot_t *out)
{
    if (out == NULL || out->entries != NULL)
        return PK_AIRBAND_RECORDING_SNAPSHOT_INVALID_ARGUMENT;
    if (s_lock == NULL) {
        return atomic_load(&s_catalog_state) ==
                       PK_AIRBAND_RECORDING_CATALOG_START_FAILED
            ? PK_AIRBAND_RECORDING_SNAPSHOT_START_FAILED
            : PK_AIRBAND_RECORDING_SNAPSHOT_NOT_STARTED;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const pk_airband_recording_catalog_state_t state =
        (pk_airband_recording_catalog_state_t)atomic_load(&s_catalog_state);
    if (state == PK_AIRBAND_RECORDING_CATALOG_NOT_STARTED ||
        state == PK_AIRBAND_RECORDING_CATALOG_START_FAILED) {
        xSemaphoreGive(s_lock);
        return state == PK_AIRBAND_RECORDING_CATALOG_START_FAILED
            ? PK_AIRBAND_RECORDING_SNAPSHOT_START_FAILED
            : PK_AIRBAND_RECORDING_SNAPSHOT_NOT_STARTED;
    }
    const bool mounted = pk_sdcard_is_mounted();
    const bool current_media = s_generation != 0 && mounted &&
                               s_generation == pk_sdcard_mount_generation();
    const size_t count = current_media ? s_current.count : 0;
    pk_airband_recording_entry_t *copy = NULL;
    if (count != 0) {
        copy = psram_alloc(NULL, count * sizeof(*copy));
        if (copy != NULL)
            memcpy(copy, s_current.entries, count * sizeof(*copy));
    }
    if (count != 0 && copy == NULL) {
        xSemaphoreGive(s_lock);
        return PK_AIRBAND_RECORDING_SNAPSHOT_OOM;
    }
    *out = (pk_airband_recording_snapshot_t){
        .entries = copy,
        .count = count,
        .invalid_files = current_media ? s_current.invalid_files : 0,
        .ignored_parts = current_media ? s_current.ignored_parts : 0,
        .allocator = s_psram_allocator,
        .catalog_state = !mounted
            ? PK_AIRBAND_RECORDING_CATALOG_SD_UNAVAILABLE
            : current_media ||
                  state == PK_AIRBAND_RECORDING_CATALOG_SCAN_ERROR ||
                  state == PK_AIRBAND_RECORDING_CATALOG_OOM
                ? state
                : PK_AIRBAND_RECORDING_CATALOG_NOT_READY,
        .stale = current_media && s_stale,
        .revision = (uint32_t)atomic_load(&s_revision),
    };
    xSemaphoreGive(s_lock);
    return PK_AIRBAND_RECORDING_SNAPSHOT_OK;
}

bool pk_airband_recording_store_snapshot_acquire(
    pk_airband_recording_snapshot_t *out)
{
    return pk_airband_recording_store_snapshot_acquire_ex(out) ==
           PK_AIRBAND_RECORDING_SNAPSHOT_OK;
}

uint32_t pk_airband_recording_store_revision(void)
{
    return (uint32_t)atomic_load(&s_revision);
}

pk_airband_recording_store_token_t pk_airband_recording_store_token(void)
{
    const bool mounted = pk_sdcard_is_mounted();
    return (pk_airband_recording_store_token_t){
        .revision = pk_airband_recording_store_revision(),
        .mount_generation = mounted ? pk_sdcard_mount_generation() : 0,
        .mounted = mounted,
    };
}

bool pk_airband_recording_store_find(pk_airband_recording_id_t id,
                                     pk_airband_recording_entry_t *out)
{
    if (out == NULL || s_lock == NULL) return false;
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool current_media = s_generation != 0 && pk_sdcard_is_mounted() &&
                               s_generation == pk_sdcard_mount_generation();
    for (size_t i = 0; current_media && i < s_current.count; ++i) {
        if (same_id(s_current.entries[i].id, id)) {
            *out = s_current.entries[i];
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

#ifdef PK_AIRBAND_RECORDING_SERVICE_TEST_HOOKS
void pk_airband_recording_store_test_poll(void) { poll_once(); }
void pk_airband_recording_store_test_set_oom(bool fail) { s_test_oom = fail; }
void pk_airband_recording_store_test_set_start_failure(bool fail)
{
    s_test_start_failure = fail;
}
#endif
