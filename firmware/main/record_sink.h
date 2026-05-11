/*
 * record_sink.h — multi-sink fan-out for CRC-valid Mode-S records.
 *
 * The DSP task (dsp_task.c) calls record_dispatch() once per Mode-S
 * frame that passes the upstream decoder's CRC check. Each registered
 * sink then sees the same record and decides what to do with it:
 *
 *   - record_sink_uart  prints "<ts_ms> *<HEX>;" to the console
 *   - record_sink_file  appends the same line to a rotated LittleFS
 *                       log file (later: SD card variant)
 *   - record_sink_ble   (Phase 3b) encodes GDL90 + queues to GATT notify
 *
 * Sinks live in a small static registry populated at boot, so there is
 * no allocation on the hot path. The dispatcher walks the registry and
 * calls each sink's write() callback synchronously. Sinks whose backing
 * I/O is slow (flash, BLE) must internally enqueue + return promptly —
 * blocking inside write() back-pressures the DSP task, which would
 * eventually drop USB IQ samples.
 *
 * Time semantics: rec->ts_ms is wall-clock milliseconds via
 * gettimeofday(); if the system clock has not been set (no SNTP yet,
 * no RTC battery), this is effectively monotonic milliseconds since
 * boot. Downstream tools — including Pilot-Kit/scripts/adsb_to_track.py
 * which expects an epoch-ms first column — treat it as authoritative,
 * so any time-sync work needs to happen before record_dispatch starts.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define RECORD_HEX_MAX_LEN  (14 * 2)  /* longest Mode-S frame (DF17 = 112 bits) */

typedef struct {
    int64_t  ts_ms;                       /* wall-clock ms (gettimeofday) */
    uint32_t icao24;                      /* 24-bit ICAO addr; 0 for DF0/DF5 etc */
    uint8_t  df;                          /* downlink format (msgtype) */
    uint8_t  hex_len;                     /* strlen(hex) — bytes_in_msg * 2 */
    char     hex[RECORD_HEX_MAX_LEN + 1]; /* null-terminated, uppercase */
} record_t;

typedef struct record_sink record_sink_t;
struct record_sink {
    const char *name;
    bool (*write)(record_sink_t *self, const record_t *rec);  /* non-blocking */
    void *priv;
};

/*
 * Register a sink. Must be called before record_dispatch() is invoked
 * (i.e. before sdr_task spawns). Returns ESP_ERR_NO_MEM if the static
 * registry is full (see RECORD_SINK_MAX in record_sink.c).
 */
esp_err_t record_sink_register(record_sink_t *sink);

/*
 * Dispatch a record to every registered sink. Safe to call from any
 * FreeRTOS task; not safe from ISR context.
 */
void record_dispatch(const record_t *rec);

/*
 * Helper invoked once during app_main, after task spawning prerequisites
 * (LittleFS mount, UART) have been satisfied. Constructs the default set
 * of sinks for Phase 3a and registers them.
 *
 * Returns the LittleFS mount point used by the file sink (or NULL if
 * the file sink failed to initialise), purely for diagnostic logging.
 */
const char *record_sinks_install_defaults(void);

/* --- Sink factories ----------------------------------------------------
 *
 * Each factory builds and registers one sink (and returns a pointer to
 * the registered record_sink_t* for callers who want to inspect or
 * tear it down). Factories own the underlying state via static storage
 * — never call any factory more than once.
 */

record_sink_t *record_sink_uart_create(void);

/*
 * file_sink writes ts-format lines to LittleFS, mounted at
 * "/storage" on the `storage` partition. Files follow the
 * pilot_kit_ts_<boot-seq>.txt naming so Pilot-Kit/scripts/adsb_to_track.py
 * can glob them. Rotation happens at FILE_SINK_ROTATE_BYTES; the oldest
 * files are pruned to keep at most FILE_SINK_KEEP_FILES.
 */
record_sink_t *record_sink_file_create(void);

/*
 * ble_sink forwards each ts-line to ble_gatt's Raw notify characteristic.
 * Requires ble_gatt_init() to have been called successfully; otherwise
 * the sink is silently inert (its writes return false but never block).
 */
record_sink_t *record_sink_ble_create(void);
