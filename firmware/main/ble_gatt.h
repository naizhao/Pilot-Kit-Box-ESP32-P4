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

/*
 * 当前**完整**的广播名，形如 "Pilot Kit Box-AABBCC" 或 "<用户串>-AABBCC"。
 *
 * 设置页显示的就是这一串——用户关心的是「手机上会扫到什么」，而不是自己在
 * NVS 里存了半截什么（那半截在 config_devname.h）。控制器还没同步出 MAC 时
 * 返回的是不带后缀的前缀，不会是空串。
 *
 * 返回的是内部静态缓冲。它只在 on_sync() 与 pk_ble_device_name_apply() 里被
 * 改写，两者都在 NimBLE host 任务上下文，渲染任务读到的最坏情况是「上一版
 * 的名字」，不会读到半截——名字整串由一次 snprintf 写成。
 */
const char *pk_ble_device_name(void);

/*
 * 按当前 NVS 设置重拼广播名，并把广播重开一遍让改动立刻生效。
 *
 * 设置页改完名字调它。不必重启整机：断连路径本来就会复用 start_advertising()
 * （见 ble_gatt.c 的 gap_event_cb），运行时重开广播是这个模块的既有能力。
 *
 * BLE 关闭或控制器尚未同步时是安全的空操作。
 */
void pk_ble_device_name_apply(void);
