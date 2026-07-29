/*
 * pilot_kit.h — shared declarations for the Pilot Kit Box firmware.
 *
 * Three tasks cooperate to deliver a stable IQ data pipeline:
 *
 *   usb_host_lib_task  pumps usb_host_lib_handle_events() forever
 *   sdr_task           registers a USB host client, opens the RTL-SDR,
 *                      configures 1090 MHz / 2 MSPS / AGC, runs
 *                      rtlsdr_read_async() (whose callback fires on
 *                      this same task and pushes bytes into g_iq_ringbuf)
 *   dsp_task           drains g_iq_ringbuf, accumulates byte counts,
 *                      logs throughput at 1 Hz
 *
 * Per Pilot Kit Box SPEC red line #3 the IQ callback never performs DSP;
 * computation is strictly downstream of the ring buffer.
 */
#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

/* --- RF / hardware constants ------------------------------------------- */

#define PK_RTLSDR_FREQ_HZ        1090000000UL  /* ADS-B Mode-S carrier */
#define PK_RTLSDR_SAMPLERATE_HZ  2000000UL     /* 2 MSPS (= 2 MB/s of IQ8) */
#define PK_USB_PERIPHERAL_MAP    BIT0          /* ESP32-P4 has one USB-OTG */

/* --- Buffering --------------------------------------------------------- *
 * Producer is 2 MSPS × 2 B (IQ8) = 4 MB/s real-time. 128 KiB only gave
 * us ~32 ms of head-room and we observed occasional overflows
 * ("DROPPED ~37 KB") once BLE + SDIO + LCD landed on the CPU 1
 * scheduler. 512 KiB ≈ 128 ms — comfortable margin against any
 * realistic scheduling burst.
 *
 * The buffer is BYTEBUF (no per-item header) and lives in PSRAM via
 * the CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384 threshold — internal
 * RAM is way too precious to spend on bulk IQ.
 */
#define PK_IQ_RINGBUF_SIZE_BYTES (512 * 1024)

/* Boot splash minimum on-screen time. Init work (IMU/UI/buttons/BLE/
 * SDR) overlaps with this hold, so we only sleep for the remainder
 * if init was faster than the target. 3 s is long enough to read the
 * version line and watch the panel stabilise; bump to 5000 if you
 * want to admire the logo. */
#define PK_BOOT_SPLASH_MIN_MS    3000

extern RingbufHandle_t g_iq_ringbuf;

/* --- Task entry points ------------------------------------------------- */

void usb_host_lib_task(void *arg);
void sdr_task(void *arg);
void dsp_task(void *arg);

/* --- Drop counter (sdr_task producer, dsp_task consumer) --------------- *
 * Returns the number of IQ bytes that were dropped on the floor since the
 * previous call (because xRingbufferSend() failed) and atomically resets
 * the counter. 32-bit reads/writes are atomic on RV32 so no extra
 * synchronisation is needed for this single-producer/single-consumer use.
 */
uint32_t pk_iq_dropped_bytes_swap(void);

/* --- SDR re-init signal (dsp_task → sdr_task) --------------------------- *
 *
 * Callable from any task. Asks sdr_task to tear down the current
 * librtlsdr session (close + re-open the same USB device address — no
 * USB unplug/replug needed) and rebuild the streaming pipeline. Used
 * by dsp_task's IQ-stall watchdog instead of esp_restart() so a hung
 * dongle no longer drags IMU / PFD / BLE down with it.
 *
 * `reason` is logged as the cause. After N consecutive re-inits that
 * fail to restore a live IQ stream, sdr_task escalates to esp_restart()
 * as a last-resort recovery.
 */
void pk_sdr_request_reinit(const char *reason);

/* --- Dongle 状态（诊断页）--------------------------------------------- *
 *
 * NO_DEVICE 与其余三态的区别是**接线问题 vs 运行问题**，诊断页要分开说：
 * 前者多半是插到了 USB-C（GPIO24/25 的 FS PHY = USB Serial/JTAG），而 host
 * 栈跑在 HS 控制器上，dongle 必须接 P1 排针（MX1.25 四针，芯片 pin 49/50）。
 */
typedef enum {
    PK_SDR_NO_DEVICE = 0,   /* USB 上没枚举到任何设备 —— 没插或插错口 */
    PK_SDR_ATTACHED,        /* 枚举到了，尚未打开（初始化中或打开失败） */
    PK_SDR_STALLED,         /* 已打开但超过 1 s 没有 IQ —— 掉链/供电/过热 */
    PK_SDR_STREAMING,       /* 正常出数 */
} pk_sdr_state_t;

pk_sdr_state_t pk_sdr_state_get(uint32_t *dropped_kb);
