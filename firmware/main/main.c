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
#include "usb/usb_host.h"

#include "pilot_kit.h"
#include "aircraft_state.h"
#include "ble_gatt.h"
#include "display.h"
#include "imu_task.h"
#include "pfd.h"
#include "record_sink.h"

static const char *TAG = "pilot_kit";

RingbufHandle_t g_iq_ringbuf = NULL;

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

    /* Bring up BLE (NimBLE on top of ESP-Hosted/C6). On boards where the
     * C6 has no hosted-slave firmware this fails gracefully — sinks stay
     * configured, BLE notifies just become no-ops. */
    esp_err_t ble_err = ble_gatt_init();
    if (ble_err != ESP_OK) {
        ESP_LOGW(TAG, "BLE init failed (%s) — UART + file sinks only",
                 esp_err_to_name(ble_err));
    } else {
        ESP_LOGI(TAG, "BLE GATT service \"PilotKitBox\" advertising");
    }

    /* Bring the ADS-B record sinks up before the producer starts. The file
     * sink mounts LittleFS and may take ~50 ms on first boot (it formats
     * the partition automatically). The UART sink is always available;
     * the BLE sink piggybacks on ble_gatt_init's lifecycle. */
    const char *file_mount = record_sinks_install_defaults();
    if (file_mount != NULL) {
        ESP_LOGI(TAG, "ADS-B sinks ready (UART + BLE + file at %s)", file_mount);
    } else {
        ESP_LOGW(TAG, "ADS-B file sink unavailable — UART + BLE sinks only");
    }

    ok = xTaskCreatePinnedToCore(sdr_task, "sdr", 8192, NULL, 6, NULL, 1);
    assert(ok == pdTRUE);

    ok = xTaskCreatePinnedToCore(dsp_task, "dsp", 4096, NULL, 4, NULL, 1);
    assert(ok == pdTRUE);

    /* Phase 4a: bring the LCD up and paint a test pattern so we know the
     * SPI / panel / backlight chain is healthy before the PFD lands. */
    esp_err_t lcd_err = pk_display_init();
    if (lcd_err != ESP_OK) {
        ESP_LOGW(TAG, "display init failed (%s) — running headless",
                 esp_err_to_name(lcd_err));
    } else {
        pk_display_test_pattern();
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
}
