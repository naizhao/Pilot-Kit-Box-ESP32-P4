/*
 * ble_gatt.h — Pilot Kit Box BLE GATT server.
 *
 * Brings up NimBLE on top of ESP-Hosted's virtual HCI (controller on
 * the on-board ESP32-C6, host on the ESP32-P4) and publishes a custom
 * "Pilot Kit ADS-B" primary service with three notify characteristics:
 *
 *   Service                   1090AD5B-0000-1000-8000-1090AD5B0000
 *   ├─ Traffic Report (NOTIFY) 1090AD5B-0000-1000-8000-1090AD5B0001
 *   │     GDL90 Msg ID 20 frames, one per tracked aircraft @ 1 Hz
 *   ├─ Heartbeat     (NOTIFY)  1090AD5B-0000-1000-8000-1090AD5B0002
 *   │     GDL90 Msg ID 00 frame @ 1 Hz; required by ForeFlight-style
 *   │     EFB apps as a keep-alive
 *   └─ Raw ts-line   (NOTIFY)  1090AD5B-0000-1000-8000-1090AD5B0003
 *         "<ts_ms> *<HEX>;" — the same shape the file/uart sinks emit;
 *         convenient fallback for clients without a GDL90 parser
 *
 * The Pilot Kit mobile app subscribes to whichever subset of these it
 * needs. Subscriptions are tracked per characteristic so we don't burn
 * radio time encoding+notifying frames nobody is listening to.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Initialise ESP-Hosted (Bluetooth controller on C6), bring up NimBLE
 * host, register the Pilot Kit GATT service, and spawn the GDL90
 * emitter task. Safe to call only once.
 *
 * Returns ESP_OK if both hosted and NimBLE came up. On failure the
 * function logs the cause and the rest of the firmware keeps running
 * without BLE (UART + file sinks unaffected).
 */
esp_err_t ble_gatt_init(void);

/*
 * Queue a raw ts-format line (no trailing newline, NUL-terminated) for
 * notification on the Raw ts-line characteristic. Drops silently if
 * the queue is full or no peer is currently subscribed. Safe to call
 * from any task.
 *
 * The string is copied into the queue, so the caller may reuse its
 * buffer immediately after returning.
 */
void ble_gatt_notify_raw_line(const char *line);

/*
 * True if at least one BLE peer is currently connected. Cheap; safe to
 * poll from dashboard tasks.
 */
bool ble_gatt_is_connected(void);

/*
 * True while the device is actively BLE advertising (i.e. after a
 * successful ble_gap_adv_start() and before the first peer connects or
 * the stack resets). Returns false once a peer has connected (NimBLE
 * stops advertising automatically on connect) or if advertising has
 * not started yet. Use ble_gatt_is_connected() to check the connected
 * state; the two flags are mutually exclusive in normal operation.
 */
bool ble_gatt_is_advertising(void);
