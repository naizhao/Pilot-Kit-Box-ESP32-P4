# Hardware reference — Waveshare ESP32-P4-WIFI6

This directory holds vendor documentation and our distilled pin/GPIO
notes for the development board the Pilot Kit Box firmware targets.

## Board identification

- Vendor: Waveshare (微雪)
- Product code: **ESP32-P4-WIFI6** (32 MB Nor Flash variant)
- Wiki: <https://www.waveshare.com/wiki/ESP32-P4-WIFI6>
- Product page: <https://www.waveshare.com/esp32-p4-wifi6.htm>

The board is a dual-MCU module: an **ESP32-P4NRW32** (RISC-V dual-core,
360 MHz, 768 KB SRAM, 32 MB stacked PSRAM, 32 MB external Nor Flash)
acting as the main MCU, plus an **ESP32-C6-MINI-1** acting purely as a
radio co-processor connected to the P4 over SDIO. The C6 provides
Wi-Fi 6 (2.4 GHz) and Bluetooth 5 / BLE — the P4 by itself has no
wireless capability.

## Files in this directory

| File | Source | What it is |
|---|---|---|
| `ESP32-P4-WIFI6-datasheet.pdf` | `files.waveshare.com/wiki/...` | Vendor schematic + datasheet (single-page wide format) |
| `board_pinout.md` | distilled from schematic | All GPIO assignments the firmware needs to know about |

## Quick links

- ESP32-P4 official datasheet: <https://www.espressif.com/sites/default/files/documentation/esp32-p4_datasheet_en.pdf>
- ESP32-P4 technical reference manual: <https://www.espressif.com/sites/default/files/documentation/esp32-p4_technical_reference_manual_en.pdf>
- ESP-Hosted MCU docs (for P4 + C6 BLE/Wi-Fi): <https://github.com/espressif/esp-hosted>
- `espressif/esp_wifi_remote` component: <https://components.espressif.com/components/espressif/esp_wifi_remote>

## Why we save vendor docs locally

The Waveshare wiki occasionally rate-limits or 403s outside the
Chinese mainland. Keeping a copy of the schematic alongside the source
tree means future firmware work (and future engineers) can audit pin
assignments without needing live internet access.
