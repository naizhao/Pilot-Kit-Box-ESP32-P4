# Pilot Kit Box — BLE Integration Specification

| Item | Value |
|------|-------|
| **Document version** | 1.1 |
| **Status** | Draft — implemented in firmware, ready for client integration |
| **Audience** | Mobile / desktop client developers integrating with Pilot Kit Box |
| **Firmware reference** | `firmware/main/ble_gatt.c` |
| **License** | This document and its reference implementation are released under the same MIT license as the rest of [Pilot-Kit-Box-ESP32-P4](https://github.com/naizhao/Pilot-Kit-Box-ESP32-P4) |
| **Companion documents** | [`docs/architecture.md`](architecture.md) (system architecture), [`docs/hardware/c6_slave_firmware.md`](hardware/c6_slave_firmware.md) (radio bring-up) |

Chinese version: [`ble_protocol-zh_CN.md`](ble_protocol-zh_CN.md)

## 1. Overview

A Pilot Kit Box is an ESP32-P4-based ADS-B receiver that demodulates
1090 MHz traffic from an attached RTL-SDR dongle and re-broadcasts the
decoded surveillance to nearby BLE clients. This document specifies
the BLE wire-level contract clients must follow to consume that
broadcast.

The protocol is intentionally minimal:

- **One primary GATT service** with **four characteristics**
- **No bonding, no pairing, no encryption in protocol v1.1** — treat
  the link as open local telemetry. ADS-B traffic is publicly
  broadcast, but clients must not use this BLE link for confidential
  data, aircraft control, or safety-critical command paths.
- **No vendor extensions** beyond the published GDL90 standard for
  traffic frames
- **Auto-time-sync via SIG-standard Current Time Service** on iOS;
  optional custom write characteristic for clients that prefer to
  push time explicitly

The Pilot Kit mobile app is the **reference client**, but the same
protocol is intended for use by any third-party EFB or analysis tool
that wants to integrate.

## 2. Advertising

A Pilot Kit Box advertises continuously while no peer is connected
and re-starts advertising on every disconnect.

**Advertisement (31 bytes):**

| Field | Value |
|-------|-------|
| **Flags** | LE General Discoverable + BR/EDR Not Supported |
| **Complete Local Name** | `Pilot Kit Box-AABBCC` (factory default) or `<USER>` (renamed — no suffix) |

**Scan response (31 bytes):**

| Field | Value |
|-------|-------|
| **Complete List of 128-bit Service UUIDs** | `1090AD5B-0000-1000-8000-1090AD5B0000` |

**Common:**

| Field | Value |
|-------|-------|
| **Advertising interval** | NimBLE default (≈ 100 ms) |
| **Address type** | Random Static (default NimBLE auto-inferred) |

### About the name suffix

`AABBCC` is the **upper-case hex of the last 3 bytes of the device's
BLE MAC** — stable across reboots (it's burned into the C6's efuse),
distinct across boards. It is appended to the **factory default name
only** (see the next section). So in a hangar with multiple untouched
Pilot Kit Boxes, each shows up as its own discoverable name:

```
Pilot Kit Box-0B5A8A
Pilot Kit Box-3F1224
…
```

### User-settable device name

Since firmware v0.9.4 the owner can rename the box from **Settings →
DEVICE NAME** (an on-screen A–Z / 0–9 / `-` / `_` editor, max **26**
characters). Once a name is set, that string **is** the advertised
name — the firmware adds nothing to it:

```
factory default   Pilot Kit Box-0B5A8A          20 bytes
renamed "N123AB"  N123AB                         6 bytes
worst case        PILOT-KIT-BOX-HANGAR-01-AB    26 bytes
```

**Why the default keeps the MAC suffix and a custom name does not:**
the suffix solves "several boxes in one hangar all look alike in the
scan list", which is only true while every device carries the *same*
name — exactly the factory default's situation. Once the owner picks a
name, whether it collides is his own call. BLE connects by device
address, not by name, and every scan result already carries the MAC, so
dropping the suffix costs neither connectability nor distinguishability
(clients must not identify devices by name anyway — see below).

The 26-character cap is the advertisement budget: 31 bytes total − 3
(Flags AD) − 2 (Name AD header) = **26 bytes** available. With no
suffix to reserve room for, the cap sits exactly on that budget —
26 ≤ 26, filled to the brim — and a compile-time assertion in the
firmware fails the build on the 27th character.

Clearing the name restores the factory default byte-for-byte (MAC
suffix included). Renaming takes effect immediately — the firmware
stops and restarts advertising, no reboot needed.

### Filtering (BREAKING for clients that matched the name prefix)

Clients **MUST** filter scan results by the **128-bit Service UUID**
`1090AD5B-0000-1000-8000-1090AD5B0000` (advertised in the scan
response).

**Do not** filter on the name. The `Pilot Kit Box-` prefix used to be a
valid alternative, but a renamed box does not carry it — a
prefix-filtering client will simply stop seeing that device. The name
is for **display only**.

## 3. GATT Service

```
Service:                                  1090AD5B-0000-1000-8000-1090AD5B0000
  ├─ Traffic Report   (0x14 / NOTIFY)     1090AD5B-0000-1000-8000-1090AD5B0001
  ├─ Heartbeat        (0x00 / NOTIFY)     1090AD5B-0000-1000-8000-1090AD5B0002
  ├─ Raw ts-line      (ASCII / NOTIFY)    1090AD5B-0000-1000-8000-1090AD5B0003
  └─ Time Sync        (R/W binary)        1090AD5B-0000-1000-8000-1090AD5B0004
```

The Pilot Kit ADS-B service uses a custom 128-bit base UUID. The
lower 32 bits identify the resource:

| Offset | Resource |
|--------|----------|
| `0000` | Primary service |
| `0001` | Traffic Report notify characteristic |
| `0002` | Heartbeat notify characteristic |
| `0003` | Raw ts-line notify characteristic |
| `0004` | Time Sync read/write characteristic |

A subsequent revision of this protocol **MAY** allocate further
characteristics in the same low-32-bit range (`0005`+). Clients
**MUST** ignore any characteristic with an unrecognised UUID under
the same service.

## 4. Connection lifecycle

```
                       Pilot Kit Box                       Mobile client
       ─────────────────────────────                  ─────────────────────────
                                                      Scan, filter by name/UUID
                                                      Connect()
       GAP CONNECT  ───────────────────────────────►  (encrypted=false, no pairing)
       GATT discover all services  ────────────────►  (returns Pilot Kit service +
                                                       Current Time Service if iOS)
       § 5 — Time sync flow runs here
       Subscribe (CCCD on whichever chars wanted)
            ◄────────────────────────────────────────  Write 0x0001 to CCCD
                                                                of …0001 / …0002 / …0003
       Notify Traffic Report (GDL90) ──────────────►
       Notify Heartbeat (GDL90)      ──────────────►   1 / sec
       Notify Raw ts-line (ASCII)    ──────────────►   per CRC-valid Mode-S frame

       GAP DISCONNECT  ◄───────────────────────────   Disconnect() or out of range
       (re-enters advertising)
```

### 4.1. ATT MTU

The firmware requests **ATT_MTU = 256** at link-up so a single
GDL90 Traffic Report (≤ 62 bytes after escaping) fits in one PDU
without fragmentation. Clients **SHOULD** negotiate up to at least
**ATT_MTU = 64**. This protocol revision does not define
cross-notification frame reassembly, so clients should treat smaller
MTUs as unsupported for GDL90 traffic.

### 4.2. Multiple clients

The firmware supports one peer at a time. Subsequent connection
requests while another peer is connected **MAY** be rejected by the
NimBLE controller; clients **MUST** handle a connection failure
gracefully by retrying after a short backoff.

## 5. Time synchronization

The firmware ships with **no real-time clock** and starts at Unix
epoch ms = 0 on every boot. Surveillance frames carry a timestamp,
so the firmware needs to learn wall-clock time from the connected
peer. Two mechanisms are supported in parallel; whichever
delivers a valid value first wins for the rest of the connection.

### 5.1. Bluetooth SIG Current Time Service (CTS)

iOS exposes the SIG-standard **Current Time Service** (UUID
`0x1805`) as a GATT server on every BLE connection automatically.
The firmware acts as a GATT client toward this service immediately
after `GAP CONNECT`:

```
GATT  discover service 0x1805
GATT  discover characteristic 0x2A2B inside it
GATT  read characteristic value
parse 10-byte payload as Bluetooth Date Time (UTC) per the SIG spec
syscall settimeofday()
```

Android phones do **not** expose CTS by default. In that case the
firmware silently moves on and waits for §5.2.

> **iOS app developers:** there is nothing to do — CTS is provided
> by the platform and you can rely on the firmware clock being
> synced within ~2 s of any iOS connection.

### 5.2. Custom Time Sync characteristic (recommended for Android / cross-platform)

Characteristic UUID `…0004` accepts a **WRITE** containing an 8-byte
little-endian unsigned integer carrying **Unix epoch milliseconds in
UTC**. The firmware applies the value to its clock immediately and
logs the delta against the prior value.

| Field | Encoding | Notes |
|-------|----------|-------|
| `epoch_ms` | `uint64_le` | Milliseconds since 1970-01-01T00:00:00Z |

Reads return the same encoding with the firmware's current
best-known epoch_ms, so a client can verify a write took effect by
reading the characteristic back.

Values older than `1704067200000` (2024-01-01T00:00:00Z) are
**rejected** with ATT error `0x13` (`Value Not Allowed`) — the
firmware refuses to roll its clock backwards, mostly to guard
against an accidental zero-initialised buffer.

Cross-platform pseudocode (Dart shown here; port to any BLE library):

```dart
final bytes = ByteData(8);
bytes.setUint64(0, DateTime.now().toUtc().millisecondsSinceEpoch, Endian.little);
await timeSyncCharacteristic.write(bytes.buffer.asUint8List(), withoutResponse: false);
```

### 5.3. Frames produced before sync

CRC-valid Mode-S frames detected before the clock is synced still
hit all the notify pipes; their `ts_ms` will be small (seconds
since boot, often <1000). Clients **MAY** discard pre-sync frames,
or pass them through with a "clock unsynced" flag. The reference
mobile app discards them.

## 6. Characteristic payloads

### 6.1. Traffic Report (`…0001`, NOTIFY)

Each notification payload is one complete GDL90 **Ownship Report**
(msg ID `0x0A`) or **Traffic Report** (msg ID `0x14`) as defined by
[FAA spec 560-1058-00 Rev A](https://www.faa.gov/sites/faa.gov/files/air_traffic/technology/adsb/archival/GDL90_Public_ICD_RevA.PDF):

```
0x7E | <0x0A or 0x14> | <27-byte payload> | <CRC-LSB> | <CRC-MSB> | 0x7E
       └──── ID ────┘                     └─── FCS ────┘
```

Byte stuffing (replace `0x7D`→`0x7D 0x5D`, `0x7E`→`0x7D 0x5E`)
applies inside the frame but never to the bracketing `0x7E`
flag bytes. CRC is CCITT-16 (poly `0x1021`, init `0x0000`,
no reflect, no xorout), LSB transmitted first, computed over
`msg ID + payload` before stuffing.

#### Cadence

When a valid own-ship source exists, the firmware first emits one
Ownship Report per second. Manual ADS-B binding wins; GT-U8 GPS is the
fallback. It then emits one Traffic Report per tracked aircraft per
second. A tracked aircraft is any 24-bit ICAO seen in a CRC-valid frame
within the trailing 60-second window.

#### Decoding hints

| Bytes (raw payload) | Meaning |
|---------------------|---------|
| 0       | Alert Status (4 bits) ⎮ Address Type (4 bits) — always `0x00` in this revision |
| 1..3    | 24-bit ICAO participant address (big-endian) |
| 4..6    | Latitude, signed 24-bit, `LSB = 180 / 2^23` degrees |
| 7..9    | Longitude, signed 24-bit, same LSB |
| 10..11  | Altitude (12 bits, 25 ft/LSB, −1000 ft offset; `0xFFF` = no data) ⎮ Misc indicators (4 bits) |
| 12      | NIC (4 bits) ⎮ NACp (4 bits) — currently `0x99` |
| 13..15  | Horizontal velocity (12 bits, 1 kt/LSB, `0xFFF` = N/A) ⎮ Vertical velocity (12 bits, signed, 64 fpm/LSB, `0x800` = N/A) |
| 16      | Track / Heading (`8-bit`, 360/256 deg/LSB) |
| 17      | Emitter Category — currently fixed to `0x01` (light aircraft) |
| 18..25  | 8-char ASCII callsign (space-padded) |
| 26      | Emergency/Priority code (4 bits) ⎮ Spare (4 bits) — always `0x00` here |

A high-quality reference parser is included in [SoftRF's
`gdl90.c`](https://github.com/lyusupov/SoftRF/blob/master/software/firmware/source/libraries/rotobox/gdl90.c)
(MIT-compatible).

### 6.2. Heartbeat (`…0002`, NOTIFY)

GDL90 Heartbeat (msg ID `0x00`), 6-byte payload, same framing as
above. Emitted **once per second** while connected. EFB apps that
expect GDL90 input rely on this to consider the receiver "alive";
clients **SHOULD** treat absence for > 5 s as a stale link.

| Bytes (raw payload) | Meaning |
|---------------------|---------|
| 0 | Status Byte 1 (bit 7 = GPS valid, bit 0 = UAT initialised) |
| 1 | Status Byte 2 (bit 7 = TS MSB, bit 0 = UTC OK) |
| 2..3 | UAT Time Stamp lower 16 bits (LSB first), seconds since 0000Z UTC |
| 4..5 | Message counts (uplink + basic/long, see spec §3.1) |

`gps_valid` follows the current GT-U8 RMC fix state and
`uat_initialised` is set to 1. In the current `v0.8.0` implementation,
the Heartbeat `utc_ok` bit remains 0 even when GPS/BLE has disciplined
the system clock; clients should use the timestamp value and treat the
flag as not yet implemented.

### 6.3. Raw ts-line (`…0003`, NOTIFY)

A plain ASCII alternative to GDL90, intended for clients that want
to reuse the same parser they already run against the Pilot Kit
Flash or MicroSD dumps (`pilot_kit_ts_*.txt`). Each notification is **one
line, no trailing newline, no null terminator**:

```
1715432198765 *8D4CA1BD9908F19A5841E08E4DFC;
^^^^^^^^^^^^^ ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
   ts_ms       AVR-format raw Mode-S hex (DF11/17/18/20/21)
```

Same `<ts_ms> *<HEX>;` shape used by dump1090-compatible
post-processing scripts. Cadence: one per CRC-valid Mode-S frame
the decoder accepts, typically 5–50 / sec depending on traffic
density.

### 6.4. Time Sync (`…0004`, READ / WRITE)

See §5.2.

| Operation | Payload |
|-----------|---------|
| READ      | `uint64_le` epoch_ms, firmware's current best-known wall-clock |
| WRITE     | `uint64_le` epoch_ms, must be ≥ 2024-01-01 UTC |

ATT errors returned on WRITE:

| Code | Meaning |
|------|---------|
| `0x0D` `Invalid Attribute Value Length` | The write was not exactly 8 bytes |
| `0x13` `Value Not Allowed` | The epoch is older than the minimum |

## 7. Sample integration

The snippet below uses [`flutter_blue_plus`](https://pub.dev/packages/flutter_blue_plus)
as a Dart illustration. Substitute your preferred BLE library —
the protocol is library-agnostic.

```dart
import 'dart:typed_data';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';

class PilotKitBoxClient {
  static final Guid svcUuid     = Guid('1090AD5B-0000-1000-8000-1090AD5B0000');
  static final Guid trafficUuid = Guid('1090AD5B-0000-1000-8000-1090AD5B0001');
  static final Guid heartbeatUuid = Guid('1090AD5B-0000-1000-8000-1090AD5B0002');
  static final Guid rawLineUuid = Guid('1090AD5B-0000-1000-8000-1090AD5B0003');
  static final Guid timeSyncUuid = Guid('1090AD5B-0000-1000-8000-1090AD5B0004');

  Future<void> connectAndStream(BluetoothDevice device) async {
    await device.connect();
    await device.requestMtu(247);  // negotiate up

    final services = await device.discoverServices();
    final svc = services.firstWhere((s) => s.uuid == svcUuid);

    // Push our own time (works on both iOS and Android).
    final timeChr = svc.characteristics.firstWhere((c) => c.uuid == timeSyncUuid);
    final bytes = ByteData(8)..setUint64(
        0, DateTime.now().toUtc().millisecondsSinceEpoch, Endian.little);
    await timeChr.write(bytes.buffer.asUint8List(), withoutResponse: false);

    // Subscribe to whichever notify chars we want.
    final traffic = svc.characteristics.firstWhere((c) => c.uuid == trafficUuid);
    await traffic.setNotifyValue(true);
    traffic.lastValueStream.listen((bytes) {
      // bytes is one framed GDL90 message; feed into a GDL90 parser.
      onGdl90TrafficFrame(bytes);
    });

    final heartbeat = svc.characteristics.firstWhere((c) => c.uuid == heartbeatUuid);
    await heartbeat.setNotifyValue(true);
    heartbeat.lastValueStream.listen((bytes) {
      // Reset a "receiver alive" timer; warn user if no beat in >5 s.
      onGdl90HeartbeatFrame(bytes);
    });

    // Optional: also subscribe to raw ts-lines for diagnostics.
    // final raw = svc.characteristics.firstWhere((c) => c.uuid == rawLineUuid);
    // await raw.setNotifyValue(true);
    // raw.lastValueStream.listen((bytes) => print(utf8.decode(bytes)));
  }
}
```

### 7.1. Recommended GDL90 parser dependencies

The Dart ecosystem doesn't yet have a maintained GDL90 package on
[pub.dev](https://pub.dev) (as of May 2026). A 1-page port of
[SoftRF's `gdl90.c`](https://github.com/lyusupov/SoftRF/blob/master/software/firmware/source/libraries/rotobox/gdl90.c)
covers all three message types we emit.

## 8. Protocol versioning & compatibility

This document tracks the protocol revision implemented by the
matching firmware build. Bumping any of the following requires a
new revision and a new section here:

- A characteristic UUID change
- An incompatible payload format change (new field added in the
  middle of the payload, semantics of an existing field changed)
- A required new characteristic (clients that don't know about it
  cannot function)

Adding a brand-new optional characteristic does **not** require a
revision bump — existing clients ignore unknown UUIDs (§3).

The firmware does not currently expose its build version over BLE.
A future firmware revision should add a standard Bluetooth Device
Information Service (`0x180A`) with the Firmware Revision String
characteristic so clients can detect protocol incompatibilities.

## 9. Open questions / future work

The following items are **not** part of this protocol revision but
are on the firmware roadmap; they will get their own sections once
implemented:

- **Device Info Service (0x180A)** for firmware version
- **Configuration write characteristic** — set RTL-SDR sample
  rate / gain / centre frequency from the app
- **Bonding + LESC encryption** before adding any non-public,
  user-identifying, control, or cockpit-sensitive data path
- **Multiple concurrent peers** — currently one at a time

## 10. References

1. FAA, [GDL 90 Data Interface Specification 560-1058-00 Rev A](https://www.faa.gov/sites/faa.gov/files/air_traffic/technology/adsb/archival/GDL90_Public_ICD_RevA.PDF), 2007.
2. Bluetooth SIG, [Current Time Service 1.1.0](https://www.bluetooth.com/specifications/specs/current-time-service-1-1/), Adopted 2015.
3. RTCA, [DO-260B *Minimum Operational Performance Standards for 1090 MHz Extended Squitter ADS-B*](https://my.rtca.org/productdetails?id=a1B36000001IcjzEAC).
4. SoftRF `gdl90.c`, [lyusupov/SoftRF](https://github.com/lyusupov/SoftRF/blob/master/software/firmware/source/libraries/rotobox/gdl90.c), MIT.
5. Stratux `gen_gdl90.go`, [cyoung/stratux](https://github.com/cyoung/stratux/blob/master/main/gen_gdl90.go), BSD-3-Clause.
