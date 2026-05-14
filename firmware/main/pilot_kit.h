/*
 * pilot_kit.h — Phase 1 shared declarations for the Pilot Kit Box firmware.
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
