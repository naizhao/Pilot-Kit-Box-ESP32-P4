/*
 * sdr_task.c — owns the USB host client + RTL-SDR control flow.
 *
 *   1. Register a USB host client (with our enumeration callback).
 *   2. Pump client events until USB_HOST_CLIENT_EVENT_NEW_DEV arrives,
 *      capturing the dongle's bus address.
 *   3. rtlsdr_open() / set_center_freq(1090 MHz) / set_sample_rate(2 MSPS)
 *      / set_tuner_gain_mode(AGC) / reset_buffer().
 *   4. rtlsdr_read_async(on_iq, ...) — blocks for the rest of the run,
 *      pumping client events itself so that on_iq fires on this task.
 *
 * on_iq is the only execution path from the URB completion path into our
 * code. Per Pilot Kit Box SPEC red line #3, it does *no* DSP work — it
 * pushes the raw bytes into g_iq_ringbuf and returns immediately.
 */

#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "usb/usb_host.h"

#include "rtl-sdr.h"
#include "pilot_kit.h"

static const char *TAG = "sdr";

#define SDR_EVT_NEW_DEV   BIT0
#define SDR_EVT_DEV_GONE  BIT1

typedef struct {
    EventGroupHandle_t evt;
    uint8_t            dev_addr;
} sdr_ctx_t;

static sdr_ctx_t s_ctx;

/* Producer-side drop counter; consumed by dsp_task via pk_iq_dropped_bytes_swap. */
static volatile uint32_t s_iq_dropped_bytes;

uint32_t pk_iq_dropped_bytes_swap(void)
{
    uint32_t v = s_iq_dropped_bytes;
    s_iq_dropped_bytes = 0;
    return v;
}

static void on_client_event(const usb_host_client_event_msg_t *msg, void *arg)
{
    sdr_ctx_t *ctx = (sdr_ctx_t *)arg;

    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ctx->dev_addr = msg->new_dev.address;
        ESP_LOGI(TAG, "USB NEW_DEV at addr %u", ctx->dev_addr);
        xEventGroupSetBits(ctx->evt, SDR_EVT_NEW_DEV);
        break;

    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGW(TAG, "USB DEV_GONE");
        xEventGroupSetBits(ctx->evt, SDR_EVT_DEV_GONE);
        break;

    default:
        break;
    }
}

static void on_iq(unsigned char *buf, uint32_t len, void *cb_ctx)
{
    (void)cb_ctx;

    /* Non-blocking send. The callback is invoked from rtlsdr_read_async()'s
     * usb_host_client_handle_events() loop, which is the same task that
     * pumps the USB client; blocking here would stall every other URB. If
     * the buffer is full, dsp_task hasn't drained fast enough — we account
     * the drop and let the URB get resubmitted upstream. */
    if (xRingbufferSend(g_iq_ringbuf, buf, len, 0) != pdTRUE) {
        s_iq_dropped_bytes += len;
    }
}

void sdr_task(void *arg)
{
    (void)arg;

    s_ctx.evt = xEventGroupCreate();
    assert(s_ctx.evt != NULL);

    const usb_host_client_config_t client_cfg = {
        .is_synchronous     = false,
        .max_num_event_msg  = 5,
        .async = {
            .client_event_callback = on_client_event,
            .callback_arg          = &s_ctx,
        },
    };
    usb_host_client_handle_t client_hdl = NULL;
    ESP_ERROR_CHECK(usb_host_client_register(&client_cfg, &client_hdl));
    ESP_LOGI(TAG, "USB client registered, waiting for RTL-SDR enumeration");

    /* If we got here after a P4-side soft reset (idf.py flash, watchdog
     * reboot, etc.) and the dongle was already plugged in, the USB bus
     * never sees a fresh attach event — the dongle's PHY stayed powered
     * and configured the whole time. USB_HOST_CLIENT_EVENT_NEW_DEV
     * would never fire and sdr_task would hang forever waiting.
     *
     * Cover that path by asking usb_host_lib for already-enumerated
     * devices right after we register the client. If one's already on
     * the bus, fake the NEW_DEV event so the rest of the open path
     * proceeds normally. */
    uint8_t addr_list[8];
    int     n_dev = 0;
    if (usb_host_device_addr_list_fill(sizeof(addr_list), addr_list, &n_dev) == ESP_OK
        && n_dev > 0) {
        s_ctx.dev_addr = addr_list[0];
        ESP_LOGI(TAG, "USB device already on bus at addr %u (persisted from previous boot)",
                 s_ctx.dev_addr);
        xEventGroupSetBits(s_ctx.evt, SDR_EVT_NEW_DEV);
    }

    /* Pump client events until NEW_DEV. The callback fires synchronously
     * from inside handle_events, so by the time the bit is set, dev_addr
     * is already populated. */
    while ((xEventGroupGetBits(s_ctx.evt) & SDR_EVT_NEW_DEV) == 0) {
        usb_host_client_handle_events(client_hdl, pdMS_TO_TICKS(100));
    }

    rtlsdr_dev_t *dev = NULL;
    int r = rtlsdr_open(&dev, s_ctx.dev_addr, client_hdl);
    if (r < 0 || dev == NULL) {
        ESP_LOGE(TAG, "rtlsdr_open failed (%d) — suspending sdr_task", r);
        vTaskSuspend(NULL);
    }
    ESP_LOGI(TAG, "rtlsdr_open OK (addr %u)", s_ctx.dev_addr);

    if ((r = rtlsdr_set_center_freq(dev, PK_RTLSDR_FREQ_HZ)) < 0) {
        ESP_LOGE(TAG, "set_center_freq failed (%d)", r);
    } else {
        ESP_LOGI(TAG, "Tuned to %lu Hz", (unsigned long)PK_RTLSDR_FREQ_HZ);
    }

    if ((r = rtlsdr_set_sample_rate(dev, PK_RTLSDR_SAMPLERATE_HZ)) < 0) {
        ESP_LOGE(TAG, "set_sample_rate failed (%d)", r);
    } else {
        ESP_LOGI(TAG, "Sampling at %lu S/s",
                 (unsigned long)PK_RTLSDR_SAMPLERATE_HZ);
    }

    /* `manual = 0` enables the tuner's automatic gain control. */
    if ((r = rtlsdr_set_tuner_gain_mode(dev, 0)) != 0) {
        ESP_LOGW(TAG, "set_tuner_gain_mode(AGC) failed (%d)", r);
    } else {
        ESP_LOGI(TAG, "Tuner gain mode: AGC");
    }

    if ((r = rtlsdr_reset_buffer(dev)) < 0) {
        ESP_LOGW(TAG, "reset_buffer failed (%d)", r);
    }

    ESP_LOGI(TAG, "Starting async IQ stream (defaults: %d URBs)",
             /* doc value, kept in sync with DEFAULT_BUF_NUMBER in librtlsdr.c */
             15);

    /* Blocks until rtlsdr_cancel_async() — only happens on unrecoverable
     * USB error in Phase 1 (callback path will trip xfer_errors threshold). */
    r = rtlsdr_read_async(dev, on_iq, NULL, /*buf_num=*/0, /*buf_len=*/0);
    ESP_LOGW(TAG, "rtlsdr_read_async returned %d (stream stopped)", r);

    rtlsdr_close(dev);
    /* Don't ESP_ERROR_CHECK — if the stream aborted with dangling URBs the
     * client may refuse to deregister; we'd rather log and suspend than
     * reboot the whole firmware over the SDR alone. */
    esp_err_t dereg = usb_host_client_deregister(client_hdl);
    if (dereg != ESP_OK) {
        ESP_LOGW(TAG, "usb_host_client_deregister: %s — leaking the client",
                 esp_err_to_name(dereg));
    }
    vTaskSuspend(NULL);
}
