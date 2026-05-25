# Hardware Reference — Waveshare ESP32-P4-WIFI6

Chinese version: [`README-zh_CN.md`](README-zh_CN.md)

This directory stores vendor material, GPIO assignments, ESP32-C6 co-processor bring-up notes, and wiring references for the current Pilot Kit Box hardware target.

## Board Identification

| Item | Value |
|---|---|
| Vendor | Waveshare |
| Product code | **ESP32-P4-WIFI6**, 32 MB Nor Flash variant |
| Main MCU | **ESP32-P4NRW32**, RISC-V dual-core, 360 MHz, 768 KB SRAM, 32 MB stacked PSRAM |
| Radio co-processor | **ESP32-C6-MINI-1**, connected to the P4 over SDIO, provides Wi-Fi 6 / BLE 5 |
| Wiki | <https://www.waveshare.com/wiki/ESP32-P4-WIFI6> |
| Product page | <https://www.waveshare.com/esp32-p4-wifi6.htm> |

ESP32-P4 has no native wireless radio; the current firmware uses the on-board C6 BLE controller through ESP-Hosted SDIO.

## Current External Hardware

| Module | Wiring | Status |
|---|---|---|
| RTL-SDR FC0013 USB dongle | Board P1 USB 2.0 HS OTG port, dedicated USB HS PHY | Integrated |
| TK024F3036 / ST7789 320x240 SPI display | CS GPIO28, MOSI GPIO29, SCK GPIO30, DC GPIO31, BL GPIO50 | Verified |
| GY-BN008X / BNO085 IMU | SDA GPIO7, SCL GPIO8, INT GPIO20, RST GPIO21 | Verified |
| TARE / MODE / UP / DOWN buttons | GPIO26 / GPIO5 / GPIO22 / GPIO23, active-low to GND | Integrated |
| ESP32-C6 hosted slave firmware | H4 UART header, flashed once with an external USB-UART adapter | BLE resolved |

## Files In This Directory

| File | Purpose |
|---|---|
| [`ESP32-P4-WIFI6-datasheet.pdf`](ESP32-P4-WIFI6-datasheet.pdf) | Waveshare schematic and one-page-wide vendor datasheet, stored locally for offline pin audits |
| [`board_pinout.md`](board_pinout.md) | GPIO assignments, board peripherals, LCD / IMU / button wiring |
| [`board_pinout-zh_CN.md`](board_pinout-zh_CN.md) | Simplified Chinese version of the pinout document |
| [`c6_slave_firmware.md`](c6_slave_firmware.md) | First-time ESP32-C6 hosted slave flashing guide |
| [`c6_slave_firmware-zh_CN.md`](c6_slave_firmware-zh_CN.md) | Simplified Chinese version of the C6 flashing guide |
| [`c6_bringup_status.md`](c6_bringup_status.md) | P4 <-> C6 SDIO / VHCI / NimBLE bring-up troubleshooting record |
| [`c6_bringup_status-zh_CN.md`](c6_bringup_status-zh_CN.md) | Simplified Chinese version of the bring-up record |
| `ESP32-P4-WIFI6-details-inter.jpg` | Waveshare board interface and silkscreen photo |
| `ESP32-P4-WIFI6-details-size.jpg` | Waveshare mechanical size reference |

## Quick Links

- ESP32-P4 official datasheet: <https://www.espressif.com/sites/default/files/documentation/esp32-p4_datasheet_en.pdf>
- ESP32-P4 technical reference manual: <https://www.espressif.com/sites/default/files/documentation/esp32-p4_technical_reference_manual_en.pdf>
- ESP-Hosted MCU docs: <https://github.com/espressif/esp-hosted>
- `espressif/esp_wifi_remote` component: <https://components.espressif.com/components/espressif/esp_wifi_remote>

## Why Keep Vendor Docs Locally

The Waveshare wiki occasionally rate-limits or returns 403 outside mainland China. Keeping the schematic, board photos, and distilled pinout notes in-tree lets future firmware and hardware work audit pin assignments without live internet access.
