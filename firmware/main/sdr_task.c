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
#include <stdlib.h>     /* malloc/free for gain-table query */
#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/event_groups.h"
#include "esp_heap_caps.h"  /* 启流前后报内部堆余量——URB 只能落内部 DMA RAM */
#include "esp_log.h"
#include "esp_system.h"     /* esp_restart() — final-fallback only */
#include "esp_timer.h"      /* esp_timer_get_time() for stream-healthy timer */
#include "usb/usb_host.h"

#include "rtl-sdr.h"
#include "pilot_kit.h"

static const char *TAG = "sdr";

#define SDR_EVT_NEW_DEV   BIT0
#define SDR_EVT_DEV_GONE  BIT1

/* Re-init policy:
 *
 * If a re-init attempt brings the IQ stream back and it stays healthy
 * for SDR_REINIT_RESET_AFTER_US, the consecutive-failure counter is
 * cleared. If we burn through SDR_REINIT_MAX_ATTEMPTS without ever
 * seeing the stream recover that long, fall back to esp_restart() —
 * something deeper is wrong (USB host stuck, dongle hardware fault,
 * peripheral_map error) and a clean boot is the safer move. */
#define SDR_REINIT_MAX_ATTEMPTS     5
#define SDR_REINIT_RESET_AFTER_US   (5 * 1000000LL)

typedef struct {
    EventGroupHandle_t evt;
    uint8_t            dev_addr;
    /* Pointer to the currently-open rtlsdr_dev_t, or NULL while we are
     * not streaming. Written by sdr_task around rtlsdr_open/rtlsdr_close;
     * read by on_client_event when DEV_GONE fires so it can poke
     * rtlsdr_cancel_async() to break read_async out of its URB loop.
     * Single-task access pattern (both writer and reader run on
     * sdr_task), so no synchronisation needed. */
    rtlsdr_dev_t      *dev;
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

/* --- Re-init request path (dsp_task → sdr_task) --------------------- *
 *
 * dsp_task sets `s_reinit_pending` then we cancel the current async
 * read so sdr_task's main loop unblocks; the next iteration takes the
 * reinit branch (skips NEW_DEV wait, re-opens the same address). */
static atomic_bool s_reinit_pending;
static int64_t     s_last_iq_us;            /* updated in on_iq, read by sdr_task */
static uint32_t    s_reinit_attempts;       /* consecutive failed re-inits */

/*
 * Dongle 状态查询（诊断页用）。
 *
 * 为什么必须有这个：在它之前，诊断页把 SDR 一律显示成"在线"——注释里明写着
 * "dongle 未插无法区分"。2026-07-29 实测时把 dongle 插到了 USB-C 口（那是
 * GPIO24/25 的 FS PHY，即 USB Serial/JTAG；host 栈跑在 HS 控制器上，两者是
 * 两套 PHY），屏幕上什么异常都看不出来，只能靠抓串口日志才发现从头到尾没有
 * 枚举事件。一台带诊断页的设备不该让人干这种事。
 *
 * 三态取自 sdr_task 自己的两个事实，不额外维护状态机：
 *   dev_addr == 0        USB 上没有枚举到设备 → 根本没插，或插错了口
 *   dev == NULL          枚举到了但还没打开/已关闭 → 正在初始化或出错
 *   有 IQ 回调           在流
 *
 * 「在流」用最后一次 on_iq 的时间戳判定而不是 dev != NULL：dongle 插着但
 * 停止出数（供电不足、过热、USB 掉链）时 dev 仍然非空，那种情况正是最需要
 * 被诊断页喊出来的。
 */
pk_sdr_state_t pk_sdr_state_get(uint32_t *dropped_kb)
{
    if (dropped_kb) *dropped_kb = s_iq_dropped_bytes / 1024;

    if (s_ctx.dev_addr == 0) return PK_SDR_NO_DEVICE;
    if (s_ctx.dev == NULL)   return PK_SDR_ATTACHED;

    const int64_t now = esp_timer_get_time();
    /* 1 s 没有 IQ 就算停流。真实速率是 2 MSPS，回调以毫秒计，1 s 已经是
     * 三个数量级的宽容。 */
    if (s_last_iq_us == 0 || now - s_last_iq_us > 1000000LL)
        return PK_SDR_STALLED;
    return PK_SDR_STREAMING;
}

void pk_sdr_request_reinit(const char *reason)
{
    ESP_LOGW(TAG, "re-init requested: %s", reason ? reason : "(no reason)");
    atomic_store(&s_reinit_pending, true);
    /* Same trick as the DEV_GONE callback uses: nudge read_async out
     * of its URB pump. Safe to call from any task; rtlsdr_cancel_async
     * just sets a flag inside librtlsdr. */
    if (s_ctx.dev != NULL) {
        rtlsdr_cancel_async(s_ctx.dev);
    }
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
        ESP_LOGW(TAG, "USB DEV_GONE — canceling async stream");
        xEventGroupSetBits(ctx->evt, SDR_EVT_DEV_GONE);
        /* Critical: nudge rtlsdr_read_async() out of its URB pump loop.
         * On physical unplug the URB completion callback path doesn't
         * reliably deliver USB_TRANSFER_STATUS_NO_DEVICE (verified on
         * Waveshare P4-WIFI6), so the librtlsdr-internal error-threshold
         * cancel never trips and read_async would block forever.
         * rtlsdr_cancel_async just sets a flag — safe from the host
         * client callback context. */
        if (ctx->dev != NULL) {
            rtlsdr_cancel_async(ctx->dev);
        }
        break;

    default:
        break;
    }
}

static void on_iq(unsigned char *buf, uint32_t len, void *cb_ctx)
{
    (void)cb_ctx;

    /* Track IQ liveness for the re-init attempt counter. Same task as
     * sdr_task's main loop (URB callback fires from read_async's pump),
     * so a plain int64_t store is fine. */
    s_last_iq_us = esp_timer_get_time();

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

    /* Outer reconnect loop. Each iteration opens the dongle, runs the
     * blocking async stream until it errors out, tears the dongle down,
     * and falls back into either:
     *   - the event pump waiting for the next NEW_DEV (normal unplug
     *     path — physical USB removal), or
     *   - immediately re-opening the same address (re-init path —
     *     dsp_task called pk_sdr_request_reinit() because the IQ
     *     stream stalled while USB was still up).
     * The USB client stays registered across iterations so the next
     * attach still gets delivered to us. */
    while (1) {
        bool reinit_branch = atomic_exchange(&s_reinit_pending, false);

        if (reinit_branch) {
            s_reinit_attempts++;
            if (s_reinit_attempts > SDR_REINIT_MAX_ATTEMPTS) {
                ESP_LOGE(TAG, "re-init attempts exhausted (%lu/%d) — "
                              "falling back to esp_restart()",
                         (unsigned long)s_reinit_attempts,
                         SDR_REINIT_MAX_ATTEMPTS);
                vTaskDelay(pdMS_TO_TICKS(100));   /* let UART drain */
                esp_restart();
            }
            ESP_LOGW(TAG, "re-init attempt %lu/%d — re-opening dongle at addr %u "
                          "without waiting for fresh USB enumeration",
                     (unsigned long)s_reinit_attempts,
                     SDR_REINIT_MAX_ATTEMPTS,
                     s_ctx.dev_addr);
            /* Skip the NEW_DEV wait — the USB device is still on the
             * bus, just our librtlsdr session is gone. */
        } else {
            /* Wait for NEW_DEV. First iteration: either addr_list_fill
             * pre-set the bit (persisted dongle) or this blocks until
             * cold plug. Subsequent iterations: blocks until next
             * replug. */
            /*
             * 干等时每 10 s 报一次总线状态。
             *
             * 之前这里是彻底静默的：没插 dongle 和「插了但主机没看见」在串口
             * 上长得一模一样——都只有开头那句 waiting，然后永远没有下文。
             * 2026-08-03 排查载板 USB-A 不枚举时就卡在这一点上：VBUS 4.88 V、
             * dongle 发烫，可日志里没有任何东西能证明主机侧到底在做什么。
             *
             * num_devices 是**已枚举**设备数，所以它把问题一分为二：
             *   >0 → 设备枚举成功了，是我们这一侧漏了 NEW_DEV 事件（固件 bug）；
             *   =0 → 主机压根没看到设备，往下查根端口/走线/极性（硬件侧）。
             */
            int64_t last_report_us = 0;
            while ((xEventGroupGetBits(s_ctx.evt) & SDR_EVT_NEW_DEV) == 0) {
                usb_host_client_handle_events(client_hdl, pdMS_TO_TICKS(100));

                int64_t now = esp_timer_get_time();
                if (now - last_report_us >= 10 * 1000 * 1000) {
                    last_report_us = now;
                    usb_host_lib_info_t info = { 0 };
                    if (usb_host_lib_info(&info) == ESP_OK) {
                        ESP_LOGW(TAG, "still waiting for RTL-SDR — USB bus has "
                                      "%d enumerated device(s), %d client(s), "
                                      "root port %s",
                                 info.num_devices, info.num_clients,
                                 info.root_port_suspended ? "SUSPENDED" : "active");
                    } else {
                        ESP_LOGE(TAG, "still waiting for RTL-SDR — "
                                      "usb_host_lib_info() failed");
                    }
                }
            }
            /* Real attach: any prior re-init failures are irrelevant
             * since this is a fresh hardware event. */
            s_reinit_attempts = 0;
        }
        /* Consume both bits so the next disconnect/reconnect cycle is
         * detected cleanly — DEV_GONE may have been left set by the
         * previous iteration's teardown path. */
        xEventGroupClearBits(s_ctx.evt, SDR_EVT_NEW_DEV | SDR_EVT_DEV_GONE);

        rtlsdr_dev_t *dev = NULL;
        int r = rtlsdr_open(&dev, s_ctx.dev_addr, client_hdl);
        if (r < 0 || dev == NULL) {
            /* Most likely: the device disappeared between NEW_DEV and
             * here, or addr_list_fill returned a stale address. Don't
             * suspend — back off and wait for the next attach. */
            ESP_LOGE(TAG, "rtlsdr_open failed (%d) — waiting for next attach", r);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        ESP_LOGI(TAG, "rtlsdr_open OK (addr %u)", s_ctx.dev_addr);
        /* Publish for on_client_event: DEV_GONE will use it to break
         * read_async out of its URB loop on physical unplug. */
        s_ctx.dev = dev;

        /* Re-open kick: on reconnect the tuner's PLL inherits stale
         * state from the previous session. Detune to 800 MHz first so
         * the next set_center_freq(PK_RTLSDR_FREQ_HZ) is guaranteed
         * to reprogram the PLL (rules out tuner driver paths that
         * no-op on "same frequency"). Cheap on fresh boot. */
        (void)rtlsdr_set_center_freq(dev, 800000000UL);

        if ((r = rtlsdr_set_sample_rate(dev, PK_RTLSDR_SAMPLERATE_HZ)) < 0) {
            ESP_LOGE(TAG, "set_sample_rate failed (%d)", r);
        } else {
            ESP_LOGI(TAG, "Sampling at %lu S/s",
                     (unsigned long)PK_RTLSDR_SAMPLERATE_HZ);
        }

        if ((r = rtlsdr_set_center_freq(dev, PK_RTLSDR_FREQ_HZ)) < 0) {
            ESP_LOGE(TAG, "set_center_freq failed (%d)", r);
        } else {
            ESP_LOGI(TAG, "Tuned to %lu Hz", (unsigned long)PK_RTLSDR_FREQ_HZ);
        }

        /* Fixed manual gain instead of tuner AGC. AGC was observed to
         * either fail to converge (>55 s before first decode after a
         * hot replug) or drift into a saturated state after ~60 s of
         * streaming (4 MB/s IQ keeps flowing but msgs/s drops to 0
         * permanently). dump1090 and the rest of the ADS-B ecosystem
         * use fixed max gain for the same reason — ADS-B is a strong
         * short-pulse signal where max LNA + IF gain works well across
         * dynamic range. Query the tuner's gain table at runtime so
         * this isn't pinned to FC0013 (max = 19.7 dB, value 197). */
        int num_gains = rtlsdr_get_tuner_gains(dev, NULL);
        if (num_gains > 0) {
            int *gains = malloc(num_gains * sizeof(int));
            if (gains) {
                rtlsdr_get_tuner_gains(dev, gains);
                int max_gain = gains[num_gains - 1];
                free(gains);
                if ((r = rtlsdr_set_tuner_gain_mode(dev, 1)) != 0) {
                    ESP_LOGW(TAG, "set_tuner_gain_mode(manual) failed (%d)", r);
                }
                if ((r = rtlsdr_set_tuner_gain(dev, max_gain)) != 0) {
                    ESP_LOGW(TAG, "set_tuner_gain(%d) failed (%d)", max_gain, r);
                } else {
                    ESP_LOGI(TAG, "Tuner gain: %d.%d dB (manual, max of %d steps)",
                             max_gain / 10, abs(max_gain % 10), num_gains);
                }
            } else {
                ESP_LOGW(TAG, "OOM querying gain table — falling back to AGC");
                rtlsdr_set_tuner_gain_mode(dev, 0);
            }
        } else {
            ESP_LOGW(TAG, "tuner reports no gain table — falling back to AGC");
            rtlsdr_set_tuner_gain_mode(dev, 0);
        }

        if ((r = rtlsdr_reset_buffer(dev)) < 0) {
            ESP_LOGW(TAG, "reset_buffer failed (%d)", r);
        }

        ESP_LOGI(TAG, "Starting async IQ stream (%d URBs x %d B, free internal "
                      "heap: %u B)",
                 PK_SDR_URB_COUNT, PK_SDR_URB_LEN,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

        /* Mark "stream just started — we haven't seen any IQ yet" so the
         * health check below knows the difference between "stream ran
         * for hours then died" and "open succeeded but nothing ever
         * came through". */
        s_last_iq_us = 0;
        int64_t stream_start_us = esp_timer_get_time();

        /* Blocks until rtlsdr_cancel_async() (called by either DEV_GONE
         * handler on physical unplug, or pk_sdr_request_reinit() on
         * IQ stall) or the URB error threshold trips. */
        r = rtlsdr_read_async(dev, on_iq, NULL,
                              PK_SDR_URB_COUNT, PK_SDR_URB_LEN);

        /* Was this an actually healthy run? If we saw IQ for at least
         * SDR_REINIT_RESET_AFTER_US, clear the attempts counter so the
         * next stall doesn't immediately fall into the esp_restart()
         * escalation path. */
        int64_t now_us = esp_timer_get_time();
        int64_t healthy_us = (s_last_iq_us > 0)
            ? (s_last_iq_us - stream_start_us) : 0;
        if (healthy_us >= SDR_REINIT_RESET_AFTER_US) {
            if (s_reinit_attempts > 0) {
                ESP_LOGI(TAG, "stream was healthy for %.1fs before exit — "
                              "clearing re-init attempt counter (was %lu)",
                         (double)healthy_us / 1e6,
                         (unsigned long)s_reinit_attempts);
            }
            s_reinit_attempts = 0;
        }

        ESP_LOGW(TAG, "rtlsdr_read_async returned %d after %.1fs of IQ "
                      "(healthy_us=%lld, now_us=%lld) — closing dongle",
                 r, (double)healthy_us / 1e6,
                 (long long)healthy_us, (long long)now_us);

        /* Hide the dev pointer before close so any racing event callback
         * doesn't poke a stale rtlsdr_dev_t (rtlsdr_close frees it). */
        s_ctx.dev = NULL;
        rtlsdr_close(dev);
        /* Deliberately do NOT deregister the client here: keep the
         * USB_HOST_CLIENT_EVENT_NEW_DEV subscription alive so a replug
         * is picked up on the next loop iteration without rebooting. */
    }
}
