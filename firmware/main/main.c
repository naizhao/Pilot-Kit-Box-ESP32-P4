/*
 * main.c — Pilot Kit Box (ESP32-P4) Phase 1 boot strap.
 *
 *   1. Allocate the shared IQ ring buffer.
 *   2. Spawn usb_host_lib_task on CPU 0 (USB stack lifecycle pump).
 *   3. Wait until the USB host library is installed.
 *   4. Spawn sdr_task on CPU 1 (RTL-SDR control + async IQ producer).
 *   5. Spawn dsp_task on CPU 1 (consumer + 1 Hz throughput meter).
 *
 * app_main returns; the three tasks own the rest of the runtime.
 */

#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "usb/usb_host.h"

#include "pilot_kit.h"
#include "aircraft_state.h"
#include "ble_gatt.h"
#include "boot_splash.h"
#include "button_task.h"
#include "display.h"
#include "imu_task.h"
#include "pfd.h"
#include "power.h"
#include "record_sink.h"
#include "ui_state.h"

static const char *TAG = "pilot_kit";

RingbufHandle_t g_iq_ringbuf = NULL;

/* --- Button → action routing ----------------------------------------- *
 *
 * Single source of truth for what each button does in each UI mode.
 * Stays in main.c so the button task doesn't have to depend on
 * imu_task + ui_state directly — keeps modules loosely coupled.
 *
 * Press semantics defined in button_task.h:
 *   - PK_BTN_EVT_SHORT_PRESS      fires on release (< 3 s held)
 *   - PK_BTN_EVT_LONG_PRESS       fires at 3 s while still held
 *                                 (TARE / MODE only — UP/DOWN suppress
 *                                  long-press in favour of the combo)
 *   - PK_BTN_EVT_VERY_LONG_PRESS  fires at 10 s while still held
 *                                 (TARE only — reserves MODE long for
 *                                  power on/off)
 *   - PK_BTN_EVT_COMBO_BLE_PAIR   fires on PK_BTN_UP at 5 s while both
 *                                 UP and DOWN are still held
 *
 * TARE actions form a graduated cage: short = "zero the live attitude
 * for this session", long = "also persist to NVS so it survives a
 * reboot", very-long = "wipe everything (NVS + BNO state) and start
 * over". The application accepts both LONG and VERY_LONG arriving on
 * the same sustained hold; factory_reset() running after tare_persist()
 * cleanly undoes it.
 *
 * Exception: in the ADS-B aircraft-list view, TARE short-press is
 * repurposed to "bind the highlighted aircraft as own-ship" — that's
 * the runtime override for the PFD's ALT/VS/GS data source. Long
 * and very-long TARE still do their IMU actions in any mode.
 */
static void on_button_event(pk_button_id_t id, pk_button_event_t evt)
{
    switch (id) {
    case PK_BTN_TARE:
        if (evt == PK_BTN_EVT_SHORT_PRESS) {
            if (pk_ui_get_mode() == PK_UI_MODE_ADSB_LIST) {
                /* Bind the currently-highlighted aircraft as own-ship.
                 * ui_state tracks the highlight by ICAO, so we don't
                 * need to re-snapshot the aircraft table here — just
                 * read whichever ICAO the list renderer last committed
                 * as the selection. Returns 0 when the user hasn't
                 * scrolled yet OR the previously-highlighted aircraft
                 * has dropped out of the 60s window without being
                 * replaced (snapshot was empty on the last frame). */
                uint32_t sel_icao = pk_ui_list_get_selected_icao();
                if (sel_icao != 0) {
                    pk_ui_set_own_icao(sel_icao);
                } else {
                    ESP_LOGW(TAG, "TARE in ADSB list: no aircraft "
                                  "highlighted yet — binding skipped");
                }
            } else {
                (void)pk_imu_tare_now();
            }
        } else if (evt == PK_BTN_EVT_LONG_PRESS) {
            (void)pk_imu_tare_persist();
        } else if (evt == PK_BTN_EVT_VERY_LONG_PRESS) {
            (void)pk_imu_factory_reset();
        }
        break;

    case PK_BTN_MODE:
        if (evt == PK_BTN_EVT_SHORT_PRESS) {
            pk_ui_toggle_mode();
        } else if (evt == PK_BTN_EVT_LONG_PRESS) {
            /* Soft power-off: drop backlight, configure GPIO5 as ext1
             * wake source, enter deep sleep. Does not return — next
             * press of MODE is a cold boot. */
            pk_power_enter_sleep();
        }
        break;

    case PK_BTN_UP:
        if (evt == PK_BTN_EVT_SHORT_PRESS) {
            pk_ui_list_scroll(-1);
        } else if (evt == PK_BTN_EVT_COMBO_BLE_PAIR) {
            /* TODO(BLE pairing): trigger BLE pairing flow once the
             * Flutter side adds a pairing-window UI. For now, just
             * log it so the gesture is observably detected. */
            ESP_LOGW(TAG, "UP+DOWN combo: BLE pairing requested "
                          "(TODO — Flutter side not implemented yet)");
        }
        break;

    case PK_BTN_DOWN:
        if (evt == PK_BTN_EVT_SHORT_PRESS) {
            pk_ui_list_scroll(+1);
        }
        break;

    default:
        break;
    }
}

void usb_host_lib_task(void *arg)
{
    ESP_LOGI(TAG, "Installing USB host stack on peripheral_map=0x%x",
             (unsigned)PK_USB_PERIPHERAL_MAP);

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags     = ESP_INTR_FLAG_LEVEL1,
        .peripheral_map = PK_USB_PERIPHERAL_MAP,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    ESP_LOGI(TAG, "USB host stack installed");

    /* Wake app_main so it can spawn the SDR task that depends on the stack
     * being up. */
    xTaskNotifyGive((TaskHandle_t)arg);

    while (1) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGW(TAG, "USB host: no clients registered");
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            ESP_LOGW(TAG, "USB host: all devices freed");
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Pilot Kit Box (ESP32-P4) — Phase 1 boot");

    /* Log wakeup cause + ext1 status. After a deep-sleep wake, cause
     * should be ESP_SLEEP_WAKEUP_EXT1 (=4) and ext1_status should have
     * bit 5 set (= 0x20) — that's GPIO5 reading low. Anything else
     * tells us self-wake came from a different source. */
    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    uint64_t ext1_status = esp_sleep_get_ext1_wakeup_status();
    ESP_LOGI(TAG, "boot wakeup_cause=%d  ext1_status=0x%llx",
             (int)wake_cause, (unsigned long long)ext1_status);

    ESP_LOGI(TAG, "Free internal heap at boot: %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    g_iq_ringbuf = xRingbufferCreate(PK_IQ_RINGBUF_SIZE_BYTES,
                                     RINGBUF_TYPE_BYTEBUF);
    assert(g_iq_ringbuf != NULL && "IQ ring buffer alloc failed");
    ESP_LOGI(TAG, "IQ ring buffer ready: %u B (BYTEBUF)",
             (unsigned)PK_IQ_RINGBUF_SIZE_BYTES);

    TaskHandle_t lib_task_hdl = NULL;
    BaseType_t ok = xTaskCreatePinnedToCore(
        usb_host_lib_task, "usb_lib", 4096,
        xTaskGetCurrentTaskHandle(), 5, &lib_task_hdl, 0);
    assert(ok == pdTRUE);

    /* Block until USB host stack is installed (usb_host_lib_task notifies). */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "USB host stack online — spawning SDR + DSP tasks");

    /* Initialise the per-aircraft fusion table before any sink can write
     * into it. */
    aircraft_state_init();

    /* Bring the ADS-B record sinks up before the producer starts. The
     * file sink mounts LittleFS and may take ~50 ms on first boot (it
     * formats the partition automatically). The UART sink is always
     * available; the BLE sink is a thin wrapper that null-guards
     * everything until ble_gatt_init() succeeds later — safe to
     * register even if BLE never comes up. */
    const char *file_mount = record_sinks_install_defaults();
    if (file_mount != NULL) {
        ESP_LOGI(TAG, "ADS-B sinks ready (UART + file at %s)", file_mount);
    } else {
        ESP_LOGW(TAG, "ADS-B file sink unavailable — UART sink only");
    }

    ok = xTaskCreatePinnedToCore(sdr_task, "sdr", 8192, NULL, 6, NULL, 1);
    assert(ok == pdTRUE);

    ok = xTaskCreatePinnedToCore(dsp_task, "dsp", 4096, NULL, 4, NULL, 1);
    assert(ok == pdTRUE);

    /* Phase 4a: bring up the LCD and paint the boot splash (logo +
     * "Booting ..." text). The splash stays on screen until the PFD
     * render task starts ~1 s later — gives the user something to
     * look at instead of staring at the test pattern (or worse,
     * the transflective panel's bare backlight) through the
     * USB/SDIO/IMU init storm. */
    esp_err_t lcd_err = pk_display_init();
    if (lcd_err != ESP_OK) {
        ESP_LOGW(TAG, "display init failed (%s) — running headless",
                 esp_err_to_name(lcd_err));
    } else {
        pk_boot_splash_render(pk_display_framebuffer());
        (void)pk_display_flush_full();
        pk_display_set_brightness(180);
    }

    /* Phase 4b: BNO085 IMU. Failure is non-fatal — the rest of the
     * firmware (RTL-SDR, BLE, storage) keeps working without attitude. */
    esp_err_t imu_err = pk_imu_init();
    if (imu_err != ESP_OK) {
        ESP_LOGW(TAG, "IMU init failed (%s) — PFD will run without attitude",
                 esp_err_to_name(imu_err));
    } else {
        ESP_LOGI(TAG, "BNO085 IMU online");
    }

    /* UI state lives in its own module so the button callback can flip
     * the mode without touching the render task directly. Default mode
     * is PFD; survives an IMU-init failure (you can still scroll the
     * ADS-B list with no attitude). */
    esp_err_t ui_err = pk_ui_init();
    if (ui_err != ESP_OK) {
        ESP_LOGW(TAG, "ui_state init failed (%s)", esp_err_to_name(ui_err));
    }

    /* Tact buttons: TARE/MODE/UP/DOWN on GPIO 26/27/22/23. Spawned
     * even when IMU init failed — only the TARE button does anything
     * without an IMU (no-ops out, harmless), and MODE/UP/DOWN still
     * drive the UI. */
    esp_err_t btn_err = pk_button_init(on_button_event);
    if (btn_err != ESP_OK) {
        ESP_LOGW(TAG, "button init failed (%s)", esp_err_to_name(btn_err));
    }

    /* Phase 4c: PFD render task. Starts after the display + IMU init
     * so it can read both straight away. Survives either failing. */
    if (lcd_err == ESP_OK) {
        esp_err_t pfd_err = pk_pfd_start();
        if (pfd_err != ESP_OK) {
            ESP_LOGW(TAG, "PFD start failed (%s)", esp_err_to_name(pfd_err));
        } else {
            ESP_LOGI(TAG, "PFD render task running");
        }
    }

    /* Phase 3b: BLE init. Requires the on-board ESP32-C6 to have been
     * pre-flashed with the matching esp_hosted slave firmware (one-time
     * board setup, see docs/BUILD.md §3). The hosted vhci_drv.c uses
     * ESP_ERROR_CHECK() internally so if C6 doesn't respond, the whole
     * P4 firmware aborts — there's no graceful path. Default is on
     * (CONFIG_PK_BLE_ENABLED=y); turn off via menuconfig if you haven't
     * flashed C6 yet or are running CI without it. */
#if CONFIG_PK_BLE_ENABLED
    esp_err_t ble_err = ble_gatt_init();
    if (ble_err != ESP_OK) {
        ESP_LOGW(TAG, "BLE init failed (%s) — UART + file sinks only",
                 esp_err_to_name(ble_err));
    } else {
        ESP_LOGI(TAG, "BLE GATT service up — advertised name landed in"
                      " on_sync (see 'ble_gatt: advertising as ...')");
    }
#else
    ESP_LOGI(TAG, "BLE disabled at build time (CONFIG_PK_BLE_ENABLED=n) — "
                  "UART + file sinks only. Flash C6 esp_hosted slave + "
                  "re-enable in menuconfig once you're ready.");
#endif
}
