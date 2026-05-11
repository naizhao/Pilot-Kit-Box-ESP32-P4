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
 * 2 MB/s producer × 128 KiB buffer ≈ 64 ms of head-room. That covers
 * the worst observed dsp_task scheduling jitter on ESP32-P4 with WiFi /
 * BLE coexistence still off; revisit once Phase 3 brings BLE online.
 */
#define PK_IQ_RINGBUF_SIZE_BYTES (128 * 1024)

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
