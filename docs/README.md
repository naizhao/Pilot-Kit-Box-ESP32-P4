# Pilot Kit Box Documentation Index

This directory uses one language convention:

- The default `*.md` documents are English.
- Simplified Chinese translations use the `-zh_CN.md` suffix.

The root project README remains bilingual because it is the public landing page. Detailed documents under `docs/` are split by language so links are predictable for both readers and tooling. This convention applies to public docs, internal notes, and plan files that live under `docs/`.

## Quick Navigation

| Topic | English | Simplified Chinese | Audience |
|---|---|---|---|
| Build and flash | [`BUILD.md`](BUILD.md) | [`BUILD-zh_CN.md`](BUILD-zh_CN.md) | First-time firmware builders |
| Configuration | [`configuration.md`](configuration.md) | [`configuration-zh_CN.md`](configuration-zh_CN.md) | Firmware customisation |
| Firmware architecture | [`architecture.md`](architecture.md) | [`architecture-zh_CN.md`](architecture-zh_CN.md) | Firmware developers |
| BLE protocol | [`ble_protocol.md`](ble_protocol.md) | [`ble_protocol-zh_CN.md`](ble_protocol-zh_CN.md) | Mobile / EFB client developers |
| Database maintenance | [`database_maintenance.md`](database_maintenance.md) | [`database_maintenance-zh_CN.md`](database_maintenance-zh_CN.md) | Firmware maintainers |
| User guide | [`user_guide.md`](user_guide.md) | [`user_guide-zh_CN.md`](user_guide-zh_CN.md) | Device users |
| Firmware release and web flashing | [`firmware_update.md`](firmware_update.md) | [`firmware_update-zh_CN.md`](firmware_update-zh_CN.md) | Maintainers and end users |
| Hardware reference index | [`hardware/README.md`](hardware/README.md) | [`hardware/README-zh_CN.md`](hardware/README-zh_CN.md) | Hardware builders |
| Board pinout | [`hardware/board_pinout.md`](hardware/board_pinout.md) | [`hardware/board_pinout-zh_CN.md`](hardware/board_pinout-zh_CN.md) | Hardware developers |
| ESP32-C6 slave flashing | [`hardware/c6_slave_firmware.md`](hardware/c6_slave_firmware.md) | [`hardware/c6_slave_firmware-zh_CN.md`](hardware/c6_slave_firmware-zh_CN.md) | BLE bring-up |
| ESP32-C6 bring-up status | [`hardware/c6_bringup_status.md`](hardware/c6_bringup_status.md) | [`hardware/c6_bringup_status-zh_CN.md`](hardware/c6_bringup_status-zh_CN.md) | Maintainer troubleshooting |

## Current Feature Baseline

All public docs should match this baseline:

- Safety boundary: Pilot Kit Box is not certified avionics and must
  not be documented as a primary instrument, backup instrument,
  navigation source, or collision-avoidance system.
- Target board: **Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3**. The P4 handles USB, DSP, UI, and storage; the C6 handles BLE.
- RTL-SDR attaches to the native USB 2.0 HS pair. With the Pilot Kit carrier fitted, the dongle plugs into the carrier's USB-A plug on J3-27/25 and H2 stays empty; on a bare Waveshare board use the H2 Type-C port with an OTG adapter or hub. The two share the same nets, so only one at a time. The firmware defaults to 1090 MHz at 2 MSPS.
- Current recommended SDR dongle tuner: **FC0013**, mainly because the BOM cost is low.
- Identity databases: the ICAO24 aircraft database ships on the microSD card at `/sdcard/aero/pk_actdb.bin` (built into `datafiles/data/pk_actdb.bin`, read by `firmware/main/aircraft_db.c`) and is updated by copying a file, not by reflashing; the airline code table at `firmware/main/airline_codes.c` and the ICAO24 country table at `firmware/main/icao_country.c` are still compiled into the firmware.
- LCD: **4.3-inch ST7701 480x800 MIPI-DSI**, presented as 800x480 landscape through PPA rotation; backlight PWM GPIO26, reset GPIO27, BL_EN GPIO33.
- GT911 touch shares I2C0 on GPIO7/8 and uses GPIO23 reset. The legacy four-button task (`button_task.c`) is excluded from the build.
- IMU: **BNO085 / GY-BN008X**, I2C0 on GPIO7 / GPIO8, RST on GPIO28, address `0x4A`; INT is wired to GPIO34 (J3 pin 28) but the driver polls. GPIO20 is board-fixed BAT_ADC.
- The touch FAB dock directly selects **PFD, TRAFFIC, ADS-B LIST, SETTINGS, ABOUT, or DIAG**.
- Navigation and tare actions are exposed through the 4.3-inch touch UI; references to MODE/TARE tact buttons describe the legacy 2.4-inch carrier only.
- GT-U8 GPS/BeiDou, BMP388 altitude/vertical speed, the traffic radar, and live DIAG page are active runtime paths. GPIO50 PPS is wired but not consumed by firmware.
- Settings controls language, QNH, map orientation, radar range, and log backend; MicroSD supports insertion/removal detection, capacity status, and guarded formatting.
- Settings, About, Diagnostics, and Compass Calibration have English/Chinese firmware UI strings with configuration persisted through NVS.

## Translation Policy

When adding or updating a document under `docs/`:

1. Write or update the English default file first.
2. Add or update the Simplified Chinese counterpart with the same relative path and `-zh_CN.md` suffix.
3. Keep section names and command snippets aligned unless a language-specific note is necessary.
4. Update this index and the root README if the document is meant for users.
5. Keep safety and certification language explicit when documenting cockpit-facing behavior.
