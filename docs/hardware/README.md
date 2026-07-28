# Hardware Reference — Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3

Chinese version: [`README-zh_CN.md`](README-zh_CN.md)

This directory stores vendor material, GPIO assignments, ESP32-C6 co-processor bring-up notes, and wiring references for the current Pilot Kit Box hardware target.

## Board Identification

| Item | Value |
|---|---|
| Vendor | Waveshare |
| Product code | **ESP32-P4-WIFI6-Touch-LCD-4.3** (SKU 33874) |
| Main MCU | **ESP32-P4NRW32**, RISC-V dual-core, 360 MHz, 768 KB SRAM, 32 MB stacked PSRAM |
| Radio co-processor | **ESP32-C6-MINI-1**, connected to the P4 over SDIO, provides Wi-Fi 6 / BLE 5 |
| Display | 4.3-inch 480×800 IPS, ST7701, 2-lane MIPI-DSI, used as 800×480 landscape |
| Touch | GT911, 5-point capacitive touch, I²C GPIO7/8 |
| Wiki | <https://docs.waveshare.net/ESP32-P4-WIFI6-Touch-LCD-4.3/> |
| Product resources | <https://docs.waveshare.net/ESP32-P4-WIFI6-Touch-LCD-4.3/Resources-And-Documents/> |
| Official examples | <https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4.3> |

ESP32-P4 has no native wireless radio; the current firmware uses the on-board C6 BLE controller through ESP-Hosted SDIO.

## Current External Hardware

| Module | Wiring | Status |
|---|---|---|
| RTL-SDR FC0013 USB dongle | Board P1 USB 2.0 HS OTG port, dedicated USB HS PHY | Integrated |
| ST7701 480×800 MIPI-DSI display | 2 lanes at 500 Mbps; DPI 30 MHz; PPA rotates to 800×480 | Bring-up in progress |
| GT911 capacitive touch | SDA GPIO7, SCL GPIO8, RST GPIO23, INT not fitted | Driver planned |
| GY-BN008X / BNO085 IMU | SDA GPIO7, SCL GPIO8, INT GPIO20, RST GPIO21 | Verified |
| GT-U8 GPS | P4 TX GPIO32, GPS TX → P4 RX GPIO51, PPS GPIO46 | Remapped for BL_EN conflict |
| BMP388 barometer | SDA GPIO7, SCL GPIO8, optional INT GPIO31 | Remapped for LCD reset conflict |
| ESP32-C6 hosted slave firmware | H4 UART header, flashed once with an external USB-UART adapter | BLE resolved |

## Files In This Directory

| File | Purpose |
|---|---|
| [`board_pinout.md`](board_pinout.md) | GPIO assignments, board peripherals, LCD / IMU / button wiring |
| [`board_pinout-zh_CN.md`](board_pinout-zh_CN.md) | Simplified Chinese version of the pinout document |
| [`c6_slave_firmware.md`](c6_slave_firmware.md) | First-time ESP32-C6 hosted slave flashing guide |
| [`c6_slave_firmware-zh_CN.md`](c6_slave_firmware-zh_CN.md) | Simplified Chinese version of the C6 flashing guide |
| [`c6_bringup_status.md`](c6_bringup_status.md) | P4 <-> C6 SDIO / VHCI / NimBLE bring-up troubleshooting record |
| [`c6_bringup_status-zh_CN.md`](c6_bringup_status-zh_CN.md) | Simplified Chinese version of the bring-up record |
| [`ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf`](ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf) | **4.3″ touch board schematic** — the authoritative source for LCD / touch / backlight pin assignments |
| [`ESP32-P4-WIFI6-Touch-LCD-4.3-dimensions.pdf`](ESP32-P4-WIFI6-Touch-LCD-4.3-dimensions.pdf) | 4.3″ board outline: 114.4 × 66.8 mm, VA 94.4 × 56.96 mm, 4 × M2.5 on 92 × 50 mm |
| [`ESP32-P4-WIFI6-Touch-LCD-4.3.zip`](ESP32-P4-WIFI6-Touch-LCD-4.3.zip) | Official mechanical resource bundle (dimension PDF + STEP + DXF), SHA-256 `8209b6aa405d4d3d8a2009e7eb545a4844e456b9cf95d8b8e53529414b03ecaf` |
| [`ESP32-P4-WIFI6-Touch-LCD-4.3-wiki.md`](ESP32-P4-WIFI6-Touch-LCD-4.3-wiki.md) | Vendor wiki text + panel timings read out of the official BSP |
| [`ST7701-datasheet.pdf`](ST7701-datasheet.pdf) | LCD driver IC — MIPI-DSI chapter and MIPISET1-4 (D0h–D3h) registers |
| [`GT911-datasheet.pdf`](GT911-datasheet.pdf) | Touch controller electricals and I²C addressing |
| [`GT911-programming-guide.pdf`](GT911-programming-guide.pdf) | Touch **coordinate report format** — not in the datasheet, only here |

## Quick Links

- ESP32-P4 official datasheet: <https://www.espressif.com/sites/default/files/documentation/esp32-p4_datasheet_en.pdf>
- ESP32-P4 technical reference manual: <https://www.espressif.com/sites/default/files/documentation/esp32-p4_technical_reference_manual_en.pdf>
- Waveshare ESP-IDF development and flashing guide: <https://docs.waveshare.net/ESP32-P4-WIFI6-Touch-LCD-4.3/Development-Environment-Setup-IDF/>
- ESP-Hosted MCU docs: <https://github.com/espressif/esp-hosted>
- `espressif/esp_wifi_remote` component: <https://components.espressif.com/components/espressif/esp_wifi_remote>

## Why Keep Vendor Docs Locally

The Waveshare wiki occasionally rate-limits or returns 403 outside mainland China. Keeping the schematic, board photos, and distilled pinout notes in-tree lets future firmware and hardware work audit pin assignments without live internet access.
