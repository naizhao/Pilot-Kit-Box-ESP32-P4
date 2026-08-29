# Hardware reference — Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3

Chinese version: [`README-zh_CN.md`](README-zh_CN.md)

This directory contains the authoritative local hardware references for the
current Pilot Kit Box target.

## Current board

| Item | Value |
|---|---|
| Board | Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3, Rev1.2, SKU 33874 |
| Main SoC | ESP32-P4NRW32, 768 KB SRAM and 32 MB in-package PSRAM |
| External flash | GD25Q256EYIGR, 256 Mbit / 32 MB NOR |
| Wireless | ESP32-C6-MINI-1-N4 over 4-bit ESP-Hosted SDIO |
| Display | ST7701, 4.3-inch 480×800 IPS, 2-lane MIPI-DSI; 800×480 landscape UI |
| Touch | GT911, I²C GPIO7/8, reset GPIO23, interrupt resistor not fitted |
| P4 console | H1 USB-C `USB TO UART`, through CH343P |
| Native USB HS | Same nets on H2 USB-C `USB` and J3-27/25; the RTL-SDR path |
| C6 download | P1 1×4 header `TX RX IO9 GND` |
| Expansion | J3 2×20 header; check the project pinout before wiring |

ESP32-P4 has no integrated radio. The current firmware starts ESP-Hosted on
the C6 before display initialization, then uses the C6 Bluetooth controller.

Official links:

- Documentation: <https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-4.3>
- Resources: <https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-4.3/Resources-And-Documents>
- Examples: <https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4.3>

## Current Pilot Kit connections

| Module | Connection | Firmware status |
|---|---|---|
| RTL-SDR FC0013 | Carrier USB-A plug on J3-27/25 (native USB 2.0 HS), H2 left empty; on a bare board use H2 with a USB-C OTG adapter/hub instead | Integrated |
| ST7701 display | Board-fixed MIPI-DSI, 2 lanes at 500 Mbit/s | Integrated |
| GT911 touch | Board-fixed GPIO7/8, reset GPIO23; polled | Integrated, first contact only |
| BNO085 IMU | GPIO7/8, reset GPIO28, INT GPIO34, address 0x4A | Integrated, polled |
| BMP388 | GPIO7/8, INT wired to GPIO31 (carrier net `BARO_INT`) | Integrated, polled |
| GPS | P4 TX GPIO49, P4 RX GPIO51; optional PPS GPIO50 | UART/RMC integrated; PPS not implemented |
| ESP32-C6 | P4 GPIO14–19 SDIO, GPIO54 EN | Wi-Fi/BLE transport integrated |
| Audio / camera | Board hardware present | Not initialized by Pilot firmware |

## Documents

| File | Purpose |
|---|---|
| [`board_pinout.md`](board_pinout.md) / [`board_pinout-zh_CN.md`](board_pinout-zh_CN.md) | Complete J3, GPIO0–54, connector and Pilot wiring reference |
| [`c6_slave_firmware.md`](c6_slave_firmware.md) / [`c6_slave_firmware-zh_CN.md`](c6_slave_firmware-zh_CN.md) | One-time C6 slave flashing guide using P1 |
| [`ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf`](ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf) | **Board authority:** Rev1.2 schematic and assembly drawing |
| [`ESP32-P4-WIFI6-Touch-LCD-4.3-wiki.md`](ESP32-P4-WIFI6-Touch-LCD-4.3-wiki.md) | Offline official-resource digest and BSP parameters |
| [`ESP32-P4-WIFI6-Touch-LCD-4.3-dimensions.pdf`](ESP32-P4-WIFI6-Touch-LCD-4.3-dimensions.pdf) | Mechanical dimensions: 114.4 × 66.8 mm, 4 × M2.5 |
| [`ST7701-datasheet.pdf`](ST7701-datasheet.pdf) | Display-controller reference |
| [`GT911-datasheet.pdf`](GT911-datasheet.pdf) | Touch electrical and addressing reference |
| [`GT911-programming-guide.pdf`](GT911-programming-guide.pdf) | Touch report and register format |

`ESP32-P4-WIFI6-datasheet.pdf` is **not** the ESP32-P4 chip datasheet and is
not the 4.3-inch Rev1.2 schematic. It describes a different Waveshare
ESP32-P4-WIFI6 board and must not be used for this target's pin assignments.

The previous 2.4-inch carrier and its four-button wiring remain historical
design sources under `docs/jlc/`; they are not the active board.
