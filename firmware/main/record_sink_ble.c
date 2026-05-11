/*
 * record_sink_ble.c — BLE raw-ts-line sink.
 *
 * The GDL90 Heartbeat + Traffic notifications are emitted by
 * ble_gatt.c's 1 Hz timer task, which reads aircraft_state directly.
 * This sink is responsible only for the third characteristic — the
 * "Raw ts-line" stream — which mirrors what the UART and file sinks
 * write. Clients that don't speak GDL90 can subscribe to the raw
 * characteristic and parse `<ts_ms> *<HEX>;` lines themselves.
 *
 * Write path is single-step: format → call ble_gatt_notify_raw_line.
 * The notify helper internally checks subscription status and drops
 * silently if no one is listening, so this sink never back-pressures
 * the dsp_task.
 */

#include "record_sink.h"
#include "ble_gatt.h"

#include <inttypes.h>
#include <stdio.h>

static bool ble_write(record_sink_t *self, const record_t *rec)
{
    (void)self;
    char line[80];
    int n = snprintf(line, sizeof(line), "%" PRId64 " *%s;",
                     rec->ts_ms, rec->hex);
    if (n < 0 || (size_t)n >= sizeof(line)) return false;
    ble_gatt_notify_raw_line(line);
    return true;
}

static record_sink_t s_ble_sink = {
    .name  = "ble_raw",
    .write = ble_write,
    .priv  = NULL,
};

record_sink_t *record_sink_ble_create(void)
{
    return &s_ble_sink;
}
