/*
 * cc -std=c11 -Wall -Wextra -Werror -O2 \
 * -I firmware/test/display_stubs -I firmware/test/host_stubs -I firmware/main \
 * -o /tmp/test_display_present firmware/test/test_display_present.c \
 * firmware/test/display_stubs/display_legacy_hook.c && /tmp/test_display_present
 *
 * Runs production display.c against a deterministic DMA/PPA event model. An
 * active DMA framebuffer must never be the output of a subsequent PPA write.
 * The v3 VSYNC ordering is injected, not a claim of reproduction on hardware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "display_sdk.h"
extern bool pk_lcd_dma_frame_selected(esp_lcd_panel_handle_t, const void *);
#include "../main/display.c"

static uint16_t logical_fb[PK_DISPLAY_W * PK_DISPLAY_H];
static uint16_t native_fb[2][PK_LCD_NATIVE_W * PK_LCD_NATIVE_H];
static unsigned active_fb, pending_fb, captured_fb;
static bool have_pending, token;
static unsigned active_writes, ppa_calls, draw_calls, waits;
static esp_err_t ppa_error, draw_error;
static display_event_cb_t refresh_cb;
static void *refresh_ctx;
static int64_t clock_us;
static int64_t ppa_duration_us, draw_duration_us;
static bool drop_wakeup;
enum event_mode { ADOPT_AND_NOTIFY, ADOPT_WITHOUT_NOTIFY, VSYNC_BEFORE_ADOPT,
                  OLD_DMA_THEN_ADOPT, REPEAT_OLD_FRAME, LATE_ADOPT_THEN_STALL };
static enum event_mode events;
static unsigned event_step;
static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, message); ++failures; } \
} while (0)

static unsigned buffer_index(const void *buffer)
{
    if (buffer == native_fb[0]) return 0;
    if (buffer == native_fb[1]) return 1;
    fprintf(stderr, "unexpected framebuffer pointer\n");
    abort();
}

static void notify_refresh(void)
{
    if (refresh_cb) (void)refresh_cb(s_panel, NULL, refresh_ctx);
}

static void notify_selected(unsigned framebuffer)
{
    (void)pk_lcd_dma_frame_selected(s_panel, native_fb[framebuffer]);
}

bool pk_display_test_legacy_selected(esp_lcd_panel_handle_t panel, const void *framebuffer)
{
    (void)panel;
    (void)framebuffer;
    /* v1's existing refresh callback was emitted after the DMA selection. */
    notify_refresh();
    return false;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void) { return &token; }
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t semaphore, BaseType_t *woken)
{
    CHECK(semaphore == &token, "refresh notification uses registered semaphore");
    token = !drop_wakeup;
    *woken = pdFALSE;
    return pdTRUE;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    CHECK(semaphore == &token, "wait uses initialized semaphore");
    if (token) { token = false; return pdTRUE; }
    if (!ticks) return pdFALSE;
    ++waits;
    if (events == ADOPT_WITHOUT_NOTIFY) {
        clock_us += (int64_t)ticks * 1000;
        if (have_pending) { active_fb = pending_fb; have_pending = false; }
        return pdFALSE;
    }
    if (events == LATE_ADOPT_THEN_STALL && event_step++) {
        clock_us += (int64_t)ticks * 1000;
        return pdFALSE;
    }
    if (events == LATE_ADOPT_THEN_STALL) clock_us += 59000;
    clock_us += 1000;
    if (events == REPEAT_OLD_FRAME) {
        notify_selected(active_fb);
    } else if (events == OLD_DMA_THEN_ADOPT && event_step++ == 0) {
        notify_selected(captured_fb);
        notify_refresh();
        /* Selection of the already captured old link list has not adopted the
         * newly requested pointer. It cannot release either pending buffer. */
    } else if (events != VSYNC_BEFORE_ADOPT && have_pending) {
        active_fb = pending_fb;
        have_pending = false;
        notify_selected(active_fb);
    }
    if (events == LATE_ADOPT_THEN_STALL) notify_selected(active_fb);
    notify_refresh();
    if (!token) { clock_us += (int64_t)ticks * 1000; return pdFALSE; }
    token = false;
    return pdTRUE;
}

int64_t esp_timer_get_time(void) { return clock_us++; }
TickType_t xTaskGetTickCount(void) { return (TickType_t)(clock_us / 1000); }
esp_err_t ppa_do_scale_rotate_mirror(ppa_client_handle_t client, const ppa_srm_oper_config_t *operation)
{
    CHECK(client == s_ppa, "PPA client is initialized");
    ++ppa_calls;
    clock_us += ppa_duration_us;
    if (ppa_error != ESP_OK) return ppa_error;
    const unsigned output = buffer_index(operation->out.buffer);
    if (output == active_fb) ++active_writes;
    CHECK(operation->in.buffer == logical_fb, "PPA reads production logical framebuffer");
    CHECK(operation->out.buffer_size == sizeof(native_fb[0]), "PPA writes full native framebuffer");
    native_fb[output][0] = logical_fb[0];
    return ESP_OK;
}
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x0, int y0, int x1, int y1, const void *buffer)
{
    CHECK(panel == s_panel, "submit uses initialized panel");
    CHECK(x0 == 0 && y0 == 0 && x1 == PK_LCD_NATIVE_W && y1 == PK_LCD_NATIVE_H,
          "submit covers the native panel");
    ++draw_calls;
    clock_us += draw_duration_us;
    if (draw_error != ESP_OK) return draw_error;
    captured_fb = active_fb;
    pending_fb = buffer_index(buffer);
    have_pending = true;
    return ESP_OK;
}

/* Bring-up stubs supply two real backing arrays and the registered callback. */
esp_err_t ledc_timer_config(const ledc_timer_config_t *config) { (void)config; return ESP_OK; }
esp_err_t ledc_channel_config(const ledc_channel_config_t *config) { (void)config; return ESP_OK; }
esp_err_t ledc_set_duty(int mode, int channel, uint32_t duty) { (void)mode; (void)channel; (void)duty; return ESP_OK; }
esp_err_t ledc_update_duty(int mode, int channel) { (void)mode; (void)channel; return ESP_OK; }
esp_err_t esp_ldo_acquire_channel(const esp_ldo_channel_config_t *config, esp_ldo_channel_handle_t *out)
{ (void)config; *out = &clock_us; return ESP_OK; }
esp_err_t esp_lcd_new_dsi_bus(const esp_lcd_dsi_bus_config_t *config, esp_lcd_dsi_bus_handle_t *out)
{ (void)config; *out = &clock_us; return ESP_OK; }
esp_err_t esp_lcd_new_panel_io_dbi(esp_lcd_dsi_bus_handle_t bus, const esp_lcd_dbi_io_config_t *config, esp_lcd_panel_io_handle_t *out)
{ (void)bus; (void)config; *out = &clock_us; return ESP_OK; }
esp_err_t esp_lcd_new_panel_st7701(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *config, esp_lcd_panel_handle_t *out)
{ (void)io; (void)config; *out = &clock_us; return ESP_OK; }
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t panel) { (void)panel; return ESP_OK; }
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t panel)
{
    /* Startup ISR may run while panel buffers/semaphore are not yet ready. */
    CHECK(!pk_lcd_dma_frame_selected(panel, native_fb[1]), "startup DMA notification cannot wake an uninitialized waiter");
    CHECK(!token, "startup callback does not touch an uninitialized semaphore");
    return ESP_OK;
}
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t panel, bool on) { (void)panel; (void)on; return ESP_OK; }
esp_err_t esp_lcd_dpi_panel_get_frame_buffer(esp_lcd_panel_handle_t panel, unsigned count, void **first, void **second)
{ (void)panel; CHECK(count == 2, "production asks for two framebuffers"); *first = native_fb[0]; *second = native_fb[1]; return ESP_OK; }
esp_err_t esp_lcd_dpi_panel_register_event_callbacks(esp_lcd_panel_handle_t panel, const esp_lcd_dpi_panel_event_callbacks_t *callbacks, void *ctx)
{ (void)panel; refresh_cb = callbacks->on_refresh_done; refresh_ctx = ctx; return ESP_OK; }
esp_err_t ppa_register_client(const ppa_client_config_t *config, ppa_client_handle_t *out)
{ (void)config; *out = &clock_us; return ESP_OK; }
void *heap_caps_aligned_alloc(size_t alignment, size_t bytes, unsigned caps)
{ (void)alignment; (void)caps; CHECK(bytes == sizeof(logical_fb), "logical framebuffer allocation size"); return logical_fb; }

static void reset_fixture(void)
{
    active_fb = 0;
    pending_fb = 0;
    captured_fb = 0;
    event_step = 0;
    ppa_duration_us = draw_duration_us = 0;
    drop_wakeup = false;
    have_pending = token = false;
    active_writes = ppa_calls = draw_calls = waits = 0;
    clock_us = 0;
    pk_display_flush_split(NULL, NULL, NULL);
    ppa_error = draw_error = ESP_OK;
    events = ADOPT_AND_NOTIFY;
    CHECK(pk_display_init() == ESP_OK, "display initializes using real production path");
    logical_fb[0] = 0x1234;
}

static void test_normal_alternation(void)
{
    reset_fixture();
    for (unsigned frame = 0; frame < 4; ++frame) {
        CHECK(pk_display_flush_full() == ESP_OK, "normal frame completes");
        CHECK(active_fb == ((frame + 1) & 1), "DMA alternates buffers after completion");
        CHECK(native_fb[active_fb][0] == 0x1234, "submitted pixels reach adopted buffer");
    }
    CHECK(active_writes == 0, "normal PPA never overwrites active DMA framebuffer");
    CHECK(draw_calls == 4 && ppa_calls == 4, "every normal frame is rotated and submitted once");
}

static void test_stale_notification(void)
{
    reset_fixture();
    notify_selected(active_fb);
    CHECK(pk_display_flush_full() == ESP_OK, "stale refresh token does not cause timeout");
    CHECK(waits == 1 && active_fb == 1, "old token cannot complete a newly submitted frame");
    CHECK(active_writes == 0, "stale token path preserves DMA buffer ownership");
}

static void test_failure_before_submission(void)
{
    reset_fixture();
    ppa_error = ESP_FAIL;
    CHECK(pk_display_flush_full() == ESP_FAIL, "PPA failure propagates to caller");
    CHECK(draw_calls == 0 && !have_pending, "failed PPA is never submitted");
    ppa_error = ESP_OK;
    draw_error = ESP_FAIL;
    CHECK(pk_display_flush_full() == ESP_FAIL, "draw submission failure propagates");
    CHECK(active_fb == 0 && !have_pending, "failed draw leaves active DMA buffer unchanged");
    draw_error = ESP_OK;
    CHECK(pk_display_flush_full() == ESP_OK, "retry after rejected submission is usable");
    CHECK(active_writes == 0, "retry after rejected submission never overwrites active framebuffer");
}

static void test_timeout_after_adoption(void)
{
    reset_fixture();
    events = ADOPT_WITHOUT_NOTIFY;
    CHECK(pk_display_flush_full() == ESP_ERR_TIMEOUT, "missing completion notification reports timeout");
    CHECK(active_fb == 1 && !have_pending, "DMA can adopt submitted buffer despite lost notification");
    const unsigned calls_before_retry = ppa_calls;
    (void)pk_display_flush_full();
    CHECK(ppa_calls == calls_before_retry, "unconfirmed submission blocks subsequent PPA writes");
    CHECK(active_writes == 0, "after timeout next PPA must not overwrite already adopted buffer");
    notify_selected(active_fb);
    events = ADOPT_AND_NOTIFY;
    CHECK(pk_display_flush_full() == ESP_OK, "late DMA adoption notification permits recovery");
    CHECK(active_writes == 0, "recovery rotates only into the released framebuffer");
}

static void test_v3_vsync_before_dma_adoption(void)
{
    reset_fixture();
    events = VSYNC_BEFORE_ADOPT;
    (void)pk_display_flush_full();
    CHECK(active_fb == 0 && have_pending, "injected v3 VSYNC can precede DMA framebuffer adoption");
    (void)pk_display_flush_full();
    CHECK(active_writes == 0, "VSYNC alone must not release a framebuffer still owned by DMA");
}

static void test_old_dma_notification_after_submit(void)
{
    /* Defensive contract test: main/PFD/ISR are currently pinned to core 0,
     * so this injected capture/submit ordering is not evidence of a current
     * on-device race. A completion must identify its actual framebuffer even
     * if task/interrupt placement changes in another supported configuration. */
    reset_fixture();
    events = OLD_DMA_THEN_ADOPT;
    CHECK(pk_display_flush_full() == ESP_OK, "new frame eventually completes after old DMA callback");
    CHECK(active_fb == 1 && !have_pending && waits == 2,
          "callback for old captured DMA frame does not acknowledge new submitted pointer");
    events = ADOPT_AND_NOTIFY;
    CHECK(pk_display_flush_full() == ESP_OK, "next frame completes after real adoption");
    CHECK(active_writes == 0, "old captured DMA callback never releases active framebuffer early");
}

static void test_bounded_wait_and_slow_rotation(void)
{
    reset_fixture();
    events = REPEAT_OLD_FRAME;
    const int64_t begin = clock_us;
    CHECK(pk_display_flush_full() == ESP_ERR_TIMEOUT, "repeated old DMA events cannot complete pending frame");
    CHECK(clock_us - begin >= 100000 && clock_us - begin < 102000,
          "duplicate wakeups do not extend the 100 ms total wait budget");
    const unsigned before = ppa_calls;
    CHECK(pk_display_flush_full() == ESP_ERR_TIMEOUT, "unconfirmed frame remains pending on retry");
    CHECK(ppa_calls == before && active_writes == 0, "timeout retry performs no PPA writes");

    reset_fixture();
    ppa_duration_us = 200000;
    draw_duration_us = 150000;
    CHECK(pk_display_flush_full() == ESP_OK, "slow rotation and submission are outside adoption wait budget");
    CHECK(active_fb == 1 && active_writes == 0, "slow rotation retains correct DMA ownership");

    reset_fixture();
    events = ADOPT_WITHOUT_NOTIFY;
    CHECK(pk_display_flush_full() == ESP_ERR_TIMEOUT, "prepare earlier unconfirmed submission");
    events = LATE_ADOPT_THEN_STALL;
    event_step = 0;
    ppa_duration_us = 200000;
    draw_duration_us = 150000;
    const int64_t resume = clock_us;
    CHECK(pk_display_flush_full() == ESP_ERR_TIMEOUT, "second wait times out if new submission is never adopted");
    const int64_t waiting_us = clock_us - resume - ppa_duration_us - draw_duration_us;
    CHECK(waiting_us >= 100000 && waiting_us < 102000,
          "old pending wait and new submitted wait share one 100 ms budget");
    CHECK(active_writes == 0, "delayed earlier adoption releases only its former buffer");
}

static void test_lost_wakeup_and_foreign_events(void)
{
    reset_fixture();
    drop_wakeup = true;
    CHECK(pk_display_flush_full() == ESP_OK, "adopted atomic state remains valid if semaphore wakeup is lost");
    CHECK(active_fb == 1 && active_writes == 0, "lost wakeup does not lose DMA ownership state");

    reset_fixture();
    CHECK(!pk_lcd_dma_frame_selected((void *)native_fb, native_fb[1]), "foreign panel DMA event is rejected");
    CHECK(!pk_lcd_dma_frame_selected(s_panel, logical_fb), "unknown framebuffer DMA event is rejected");
    CHECK(!token, "foreign events do not wake the display waiter");
    CHECK(pk_display_flush_full() == ESP_OK && active_writes == 0,
          "foreign notifications cannot contaminate subsequent framebuffer ownership");
}

int main(void)
{
    test_normal_alternation();
    test_stale_notification();
    test_failure_before_submission();
    test_timeout_after_adoption();
    test_v3_vsync_before_dma_adoption();
    test_old_dma_notification_after_submit();
    test_bounded_wait_and_slow_rotation();
    test_lost_wakeup_and_foreign_events();
    if (failures) { fprintf(stderr, "display present: %d failure(s)\n", failures); return 1; }
    puts("display present: PASS");
    return 0;
}
