# Pilot Kit Box — Firmware Architecture

Chinese version: [`architecture-zh_CN.md`](architecture-zh_CN.md)

Snapshot of the current 4.3-inch touch runtime topology, including ADS-B,
BLE, GPS NMEA/RMC, barometer, dual storage backends, local traffic UI,
diagnostics, IMU and i18n.

## Big picture

```mermaid
flowchart LR
    subgraph HW["Hardware"]
        direction TB
        SDR["RTL-SDR\n(1090 MHz)"]
        USB["H2 native USB 2.0 HS\nUSBD_N/P dedicated nets"]
        C6["ESP32-C6-MINI-1\n(Wi-Fi 6 / BLE 5)"]
        SDIO_C6["SDIO bus\nCLK=18 CMD=19\nD0..3=14..17\nRESET=54"]
        FLASH["32 MB Nor Flash\nfactory app 12 MiB"]
        SD["MicroSD slot\nSDIO 3.0\nCLK=43 CMD=44\nD0..3=39..42"]
        GPS["GT-U8 GPS/BeiDou\nUART1 TX=32 RX=51\nRMC time; PPS not consumed"]
        BARO["BMP388\nI²C0 addr 0x76"]
        BNO["BNO085 IMU\nI²C 7=SDA 8=SCL\npolled; RST=21"]
        SCREEN["ST7701 MIPI-DSI\nnative 480×800\nPPA → 800×480\nRST=27 BL=26"]
        TOUCH["GT911 touch\nI²C0 7/8\nRST=23"]
        BLE_PEER["iPad / iPhone\nPilot Kit app"]
    end

    subgraph P4["ESP32-P4NRW32"]
        direction TB
        subgraph T_USB["usb_host_lib_task — CPU 0, prio 5"]
            USBINST["usb_host_install()\nusb_host_lib_handle_events()"]
        end

        subgraph T_SDR["sdr_task — CPU 1, prio 6"]
            CLIENT["usb_host_client_register()"]
            OPEN["rtlsdr_open / set_freq=1090M\nset_rate=2 MSPS / AGC"]
            READ["rtlsdr_read_async()\n(15 URBs × 6400 B)"]
            CB["on_iq cb\n(non-blocking, push only)"]
        end

        RBUF["g_iq_ringbuf\n512 KiB BYTEBUF\nPSRAM-backed"]

        subgraph T_DSP["dsp_task — CPU 1, prio 4"]
            DRAIN["xRingbufferReceiveUpTo\n8 KiB chunks + 480 B overlap"]
            MAG["mode_s_compute_magnitude_vector"]
            DET["mode_s_detect"]
            ONMSG["on_mode_s_msg\n• CRC filter\n• ICAO/alt/cpr extract\n• CPR global decode"]
            DASH["1 Hz dashboard\n(msgs/s, MB/s, aircraft)"]
        end

        DISPATCH["record_dispatch\n(synchronous fan-out)"]

        subgraph SINKS["sinks (registered at boot)"]
            direction LR
            SINK_UART["uart sink\nprintf line\n→ Type-C USB CDC"]
            SINK_FILE["file sink\nqueue → writer task"]
            SINK_BLE["ble sink\nraw ts-line → queue"]
        end

        subgraph T_FILE["file_writer_task — CPU 0, prio 3"]
            FW["file append\nFlash: 1 MiB rotation, target 12\nMicroSD: 16 MiB × 64"]
        end

        subgraph T_BLE["ble_emit_task — CPU 0, prio 3"]
            STATE["aircraft_state snapshot\n(64 slots, 60 s window)"]
            GDL["GDL90 encode\n• Heartbeat (1 Hz)\n• Traffic Report per aircraft"]
            GATT["NimBLE GATT notify\non Traffic / Heartbeat / Raw chars"]
        end
    end

    SDR -.RF.-> USB
    USB -.USB 2.0 HS\n2 MSPS IQ8.-> CLIENT
    CLIENT --> OPEN --> READ --> CB
    CB -- "xRingbufferSend" --> RBUF
    RBUF --> DRAIN --> MAG --> DET --> ONMSG
    ONMSG --> DISPATCH
    ONMSG -- aircraft_state_ingest --> STATE
    ONMSG --> DASH
    DISPATCH --> SINK_UART
    DISPATCH --> SINK_FILE
    DISPATCH --> SINK_BLE
    SINK_UART -.serial.-> EXT_PY["Pilot Kit\nadsb_to_track.py\n→ GPX/KML"]
    SINK_FILE --> T_FILE
    FW -.write.-> FLASH
    FW -.write.-> SD
    SINK_BLE -.queue.-> T_BLE
    STATE --> GDL
    GDL --> GATT
    GATT -- HCI over SDIO --> SDIO_C6
    SDIO_C6 <-->C6
    C6 -- BLE 5 --> BLE_PEER

    SD --> SINK_FILE
    GPS --> P4
    BARO --> P4
    BNO --> P4
    SCREEN --> P4
    TOUCH --> P4

```

## ASCII view (when the SVG render is unavailable)

```
                       ┌────────────── ESP32-P4-WIFI6 ──────────────┐
                       │                                            │
   RTL-SDR ──USB-HS──▶ │ usb_host_lib_task (CPU0)                   │
   (1090 MHz)          │     │                                      │
                       │     ▼                                      │
                       │ sdr_task (CPU1) ──rtlsdr_read_async──┐     │
                       │     │                                │     │
                       │     │  ┌── on_iq cb (zero compute) ──┘     │
                       │     │  │                                   │
                       │     │  ▼                                   │
                       │   g_iq_ringbuf  512 KiB BYTEBUF            │
                       │     │                                      │
                       │     ▼                                      │
                       │ dsp_task (CPU1)                            │
                       │   magnitude → detect → on_mode_s_msg       │
                       │     │           │                          │
                       │     │           ├─ ESP_LOGI dashboard      │
                       │     │           ▼                          │
                       │     │     record_dispatch                  │
                       │     │      │   │   │                       │
                       │     │      ▼   ▼   ▼                       │
                       │     │   uart  file  ble (3b)               │
                       │     │    │     │     │                     │
                       │     │    │     ▼     ▼                     │
                       │     │    │  writer  ble_gatt (3b)          │
                       │     │    │   task   ▼                      │
                       │     │    │   │   GDL90 enc → SDIO → C6 ──▶ │  iPad / iPhone
                       │     │    │   ▼                             │   (BLE 5)
                       │     │    │  LittleFS (or SD)               │
                       │     │    ▼                                 │
                       │     │  Type-C UART ─────────────────────── │  PC / Pilot-Kit
                       │     │                                      │     adsb_to_track.py
                       │     ▼                                      │
                       │  1 Hz console dashboard                    │
                       └────────────────────────────────────────────┘
```

## Task table

| Task              | CPU | Prio | Stack | Role |
|-------------------|-----|------|-------|------|
| `usb_host_lib`    | 0   | 5    | 4 KiB | Pumps `usb_host_lib_handle_events()`; required by USB stack lifecycle. |
| `sdr`             | 1   | 6    | 8 KiB | Owns the USB client, opens the RTL-SDR, drives `rtlsdr_read_async()`. The async URB callback runs *on this same task* (`rtlsdr_read_async`'s wait loop pumps client events itself), so the IQ producer is a single-task design with no cross-CPU contention. |
| `dsp`             | 1   | 4    | 4 KiB | Drains the ring buffer, runs dump1090's magnitude + Manchester decode, dispatches CRC-valid frames into the sink fan-out + the per-aircraft fusion table, and emits the 1 Hz dashboard. |
| `rec_file`        | 0   | 3    | 4 KiB | File writer selected at boot from NVS: LittleFS or MicroSD, with LittleFS fallback when the requested card is absent. Keeps the DSP task off storage writes. |
| `gps`             | 0   | 4    | 4 KiB | Parses GT-U8 UART1 NMEA (RMC/GGA/GSV/TXT), maintains GPS/BeiDou fix, satellite/SNR and antenna state, and sets time from RMC. GPIO50 PPS is not consumed. |
| `imu`             | 0   | 5    | 4 KiB | Polls BNO085 rotation-vector reports at 100 Hz, applies software tare, and feeds the PFD / calibration wizard. |
| `baro`            | 0   | 4    | 4 KiB | Lightweight task: polls BMP388 over I²C0 at ~10 Hz, runs temperature-compensated pressure-to-altitude conversion, computes vertical speed, and writes results into `g_baro_state` (QNH-adjustable). |
| `sd_detect`       | 0   | 2    | 4 KiB | Probes an absent MicroSD every 3 seconds; checks mounted-card health and refreshes cached capacity every 2 seconds. |
| `buttons`         | —   | —    | — | Legacy source retained but not started on the 4.3-inch touch board. |
| `pfd`             | 0   | 4    | 6 KiB | Renders PFD and UI views into the 800×480 logical framebuffer at ~30 FPS. |
| `nimble_host`     | 0   | 4    | 4 KiB | NimBLE host event loop, hosts the GATT server; events arrive from the C6 controller over the SDIO/VHCI transport. |
| `ble_emit`        | 0   | 3    | 6 KiB | 1 Hz timer task: snapshots `aircraft_state` and emits GDL90 Heartbeat + one Traffic Report per fresh aircraft on the BLE notify pipes; also drains the raw-ts-line queue produced by the BLE sink. |

## Memory budget

| Region | Size | Owner |
|--------|------|-------|
| IQ ring buffer | 512 KiB | `g_iq_ringbuf` (bulk IQ buffering, PSRAM-backed under current malloc threshold) |
| URB pool       | ~96 KiB | 15 × 6400 B in-flight USB transfers |
| DSP working set| ~12 KiB | 8 KiB IQ buf + 4 KiB magnitude buf |
| CPR table      | ~5 KiB  | 64 aircraft slots in `cpr_decode.c` |
| aircraft_state | ~7 KiB  | 64 slots in `aircraft_state.c` (callsign + alt + position + velocity) |
| Application framebuffer | 750 KiB | 800×480×16 bpp RGB565-swapped in PSRAM |
| DPI framebuffers | 1.5 MiB | Two 480×800×16 bpp scanout buffers in PSRAM |
| file_sink queue| ~10 KiB | 256 × 40 B `file_record_t` items |
| ble_raw queue  | ~5 KiB  | 64 × 80 B ts-line strings for the BLE Raw characteristic |
| NimBLE host    | ~30 KiB | event loop, GATT DB, peer connection state (typical IDF v6 footprint) |
| aircraft DB blob | ~8.16 MiB flash | `aircraft_db.bin`, embedded by `EMBED_FILES` for ICAO24 -> type/model/registration lookup |
| airline/country tables | ~230 KiB + small table in flash | `airline_codes.c` and `icao_country.c`, generated lookup data for callsign and country display |

Large bulk buffers now live in PSRAM where practical, preserving the
P4's 768 KiB internal SRAM for DMA-capable allocations, FreeRTOS stacks,
ESP-Hosted queues, and USB host descriptors.

## Failure isolation

```
URB / IQ stall → librtlsdr.c::_libusb_callback bumps xfer_errors;
                 repeated errors trip rtlsdr_cancel_async(). dsp_task can
                 also call pk_sdr_request_reinit() when IQ stalls. sdr_task
                 closes and re-opens the same dongle address; after repeated
                 failed attempts it escalates to esp_restart().

Ring overflow  → xRingbufferSend in on_iq fails → counter incremented
                 (pk_iq_dropped_bytes_swap); dashboard logs a WARN line
                 so the operator sees DSP back-pressure.

File queue full→ file sink xQueueSend returns pdFALSE → s_dropped++;
                 every 256 drops the writer task logs a WARN. The DSP
                 path never stalls.

Storage write → LittleFS/MicroSD fwrite or rotation failures are logged;
  error          UART and BLE sinks continue. Card removal is detected
                 and unmounted, but the file backend does not switch
                 dynamically during the same boot.

BLE peer drops → NimBLE handles GAP/connection state; ble_gatt_task
                 keeps draining its queue and discards notifies when no
                 peer is subscribed. Reconnect is transparent.
```

## Time synchronisation

The firmware boots with system time at Unix epoch 0 (no RTC or
battery-backed clock). Three paths can seed wall-clock time, with
source-quality protection preventing a lower-quality source from
overwriting a better one:

1. **GT-U8 RMC** — RMC supplies UTC date/time. This is the preferred
   source. GPIO50 is only a proposed PPS wire; current firmware has no PPS
   GPIO input or discipline loop.
2. **iOS Current Time Service** (BLE SIG std., UUID 0x1805) —
   immediately after every `GAP CONNECT` the firmware acts as a
   GATT client toward the peer's CTS, reads the 10-byte UTC date-time
   payload, and calls `settimeofday()`. iOS exposes CTS by default,
   so this is zero-config on Apple platforms.
3. **Custom Time Sync characteristic** (UUID `…0004`, R/W) — any
   client can `write` an 8-byte little-endian Unix-ms value; the
   firmware applies it via `settimeofday()`. Reads return the
   current firmware-perceived epoch_ms. Used by mobile clients on
   Android (no system CTS) and any other cross-platform clients.

See [`docs/ble_protocol.md`](ble_protocol.md) for the BLE wire
format; the diagram below focuses on the two BLE-assisted paths.

```
Mobile peer ── GAP_CONNECT ──► sdr_task              dsp_task
                                  │                   │
                                  ▼                   ▼
                          ble_gatt::gap_event_cb     emits ts_ms from
                                  │                  gettimeofday()
                                  ▼                  on every Mode-S
                          time_sync_kickoff           frame, so the
                                  │                   stamp magically
              ╭───────────────────┴───────────────────╮ "catches up" the
              ▼                                       ▼  moment either
       ble_gattc_disc_svc_by_uuid(0x1805)      Time Sync char write
       (silent no-op on Android)               (epoch_ms LE u64)
              │                                       │
              ▼                                       ▼
       ble_gattc_read(0x2A2B)              chr_time_access_cb
              │ parse 10 B CTS UTC                    │
              ▼                                       ▼
              ╰─────────► apply_epoch_ms() ◄──────────╯
                              │
                              ▼
                       settimeofday(),
                       all subsequent
                       ts_ms are real UTC
```

Frames produced before the first sync still go down all sinks; the
ts_ms reads small (seconds since boot). Clients can detect and
discard them — the reference Pilot Kit app does.

### Carrier-board sensors (GPS / baro / microSD)

Pilot Kit connects a GT-U8 GPS over UART, a BMP388 barometer (I²C0,
`0x76`), and uses the Rev1.2 board's microSD slot. GPIO50 may be reserved
for future PPS work, but that path is not implemented. Design notes:
[`superpowers/specs/2026-05-31-gps-baro-timing-storage.md`](superpowers/specs/2026-05-31-gps-baro-timing-storage.md).
How each slots into the architecture:

- **GPS time sync** (`gps_task.c`) — GPS UTC from RMC seeds `settimeofday()`. GPS is
  **preferred (most accurate)**, BLE is the backup; overwrite protection
  keeps a lower-quality source from clobbering a good GPS fix. The clock
  self-disciplines without a phone, and DIAG shows system time, fix,
  constellation, and SNR state. There is no PPS-edge timestamp correction.
- **GPS own-ship** (`gps_task.c`) — used when no compile-time ADS-B own-ship
  ICAO is active. Feeds the same
  `aircraft_state` own-ship path + GDL90 ownship report.
- **BMP388 baro** (`baro_task.c`) — altitude / vertical-speed shown as a **reference
  only** (unreliable in a pressurised cabin), never the authoritative
  altitude. QNH is user-adjustable from SETTINGS.
- **microSD record sink** — Settings selects Flash or MicroSD and stores
  the choice in NVS for the next boot. A missing requested card falls
  back to LittleFS. Flash rotates at 1 MiB with a 12-file count target
  constrained by the 10 MiB partition; MicroSD rotates
  16 MiB × 64 files (about 1 GiB) and supports guarded FAT32 formatting.

## What goes on the wire / on disk

The exact line shape every sink produces for one Mode-S frame:

```
1715432198765 *8D4CA1BD58C386435840BA1AD7CA;
^^^^^^^^^^^^^ ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 │             │
 │             └── AVR-format hex payload of the Mode-S frame
 │                 (14 bytes for DF17/18, 7 bytes for DF11)
 └── gettimeofday() in milliseconds; pre-SNTP this is boot-relative
```

This format is exactly what `Pilot-Kit/scripts/adsb_to_track.py`
ingests (`parse_ts_line()` in that file), so the firmware → Python
pipeline is a single `cat | adsb_to_track.py` away from producing
GPX / KML tracks.

## Why this shape

A few non-obvious choices that this diagram makes load-bearing:

1. **All RF / USB work runs on CPU 1.** The audio codec interrupts,
   future BLE host events, and any SDIO traffic stay on CPU 0,
   removing one large source of jitter from the 2 MSPS data path.

2. **One task owns the USB client.** Two tasks calling
   `usb_host_client_handle_events()` on the same client is undefined
   behaviour. `sdr_task` is the sole pump (it pumps from the
   `rtlsdr_read_async` wait loop).

3. **DSP never blocks on I/O.** The sinks are responsible for their
   own backpressure. The DSP loop guarantees forward progress on the
   IQ stream regardless of what flash, BLE peers, or operators do.

4. **The raw on-wire format matches the on-disk format.** Serial,
   LittleFS, and the BLE Raw characteristic all use the same
   `<ts_ms> *<HEX>;` line shape. The GDL90 encoder is the structured
   BLE traffic path, while raw ts-lines remain the debug and
   compatibility path.
