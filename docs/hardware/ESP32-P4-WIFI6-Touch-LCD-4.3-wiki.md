# Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 — Hardware Documentation Archive

Chinese version: [`ESP32-P4-WIFI6-Touch-LCD-4.3-wiki-zh_CN.md`](ESP32-P4-WIFI6-Touch-LCD-4.3-wiki-zh_CN.md)

This document is an offline archive of the official Waveshare wiki content + pin assignments read directly from the schematic PDF.

- Official documentation page: <https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-4.3>
- Resources download page: <https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-4.3/Resources-And-Documents>
- Product page: <https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-4.3.htm> (SKU 33874 = standard version, 33875 = version with OV5647 camera)
- Official example repository: <https://github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-4.3>
- Note: `https://www.waveshare.com/wiki/...` and `https://www.waveshare.net/wiki/...` are both dead (403 / 404); content has migrated to `docs.waveshare.com`.

## Companion Files in This Directory

| File | Description | Source |
|---|---|---|
| `ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf` | Official schematic (Rev1.2, 2 pages: page 1 circuit diagram / page 2 assembly silkscreen), SHA-256 `3697baa3ded0089446baf09705f437d13cf0324874031ccc57fd9b72cd9dfe53` | <https://www.waveshare.net/w/upload/b/b8/ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf> |
| `ESP32-P4-WIFI6-Touch-LCD-4.3-dimensions.pdf` | Dimensions drawing (20260411) | Extracted from the official `ESP32-P4-WIFI6-Touch-LCD-4.3.zip` |
| `ESP32-P4-WIFI6-Touch-LCD-4.3.zip` | Official mechanical package (dimensions PDF + STEP + DXF), SHA-256 `8209b6aa405d4d3d8a2009e7eb545a4844e456b9cf95d8b8e53529414b03ecaf` | <https://www.waveshare.net/w/upload/3/36/ESP32-P4-WIFI6-Touch-LCD-4.3.zip> |
| `ST7701-datasheet.pdf` | Display driver IC datasheet (Sitronix ST7701 SPEC V1.2, 303 pages, includes MIPI-DSI chapters) | Crystalfontz mirror <https://www.crystalfontz.com/controllers/uploaded/ST7701.pdf> |
| `GT911-datasheet.pdf` | Goodix GT911 touch IC datasheet Rev.09 | <https://files.waveshare.com/wiki/common/GT911_EN_Datasheet.pdf> |
| `GT911-programming-guide.pdf` | GT911 programming guide Rev.10 (register table / coordinate report format) | <https://www.lcd-module.de/fileadmin/eng/pdf/zubehoer/GT911_Programming_Guide_Rev.10.pdf> |

## Onboard Resources (abridged translation of the official Hardware Description)

1. **ESP32-P4-Core** — ESP32-P4NRW32 + 32MB Nor Flash (32MB PSRAM stacked in-package)
2. **ESP32-C6-MINI-1 module** — SDIO interface, provides Wi-Fi 6 / Bluetooth 5 (LE)
3. **ES7210** echo cancellation chip
4. **ES8311** low-power audio codec chip
5. **MIPI CSI interface** — 15PIN / 0.5mm pitch, supports MIPI 2-lane cameras
6. **2.54mm 4PIN pads** — for flashing firmware to the ESP32-C6
7. **RTC battery holder** — rechargeable RTC batteries only
8. **POWER button** — hold for 2s to power off, short press to power on
9. **BOOT button** — hold during power-up/reset to enter download mode
10. **RESET button**
11. **TF card slot** — SDIO 3.0
12. **MIPI DSI LCD interface** — connects MIPI 2-lane displays
13. **Speaker connector** — GH 1.25 2PIN (locking), 8Ω 2W recommended
14. **Type-C (USB TO UART)** — power / flashing / debugging
15. **Type-C (USB OTG)** — USB OTG 2.0 High Speed
16. **Onboard dual-microphone array**
17. **PWR LED power indicator**
18. **MX1.25 lithium battery connector** — 3.7V, supports charging and discharging
19. **40PIN pin header expansion** — 2.54mm, compatible with some Raspberry Pi HATs

Display: 4.3 inch capacitive touch IPS, **480 × 800**.

Dimensions (from dimensions.pdf): cover plate 114.4 × 66.8 mm, active area 94.4 × 56.96 mm, overall thickness 11.15 mm, 4×M2.5 mounting holes, hole spacing 92 × 50 mm.

## Pin Definitions (read directly from the "4.3inch Display" / "4.3 INCH" blocks on page 1 of schematic.pdf)

In the schematic, all display and touch signals are jumpered to the P4 GPIOs via 0R resistors:

| Net | Jumper Resistor | ESP32-P4 GPIO | Notes |
|---|---|---|---|
| `ESP_I2C_SCL` / `TP_SCL` | direct connection | **GPIO8** | shares the same I²C bus with the onboard codec |
| `ESP_I2C_SDA` / `TP_SDA` | direct connection | **GPIO7** | same as above |
| `RESET` (LCD reset) | R60 = 0R | **GPIO27** | this net also has R102 = 10K pull-down to GND |
| `BL_EN` (backlight enable) | R32 = 0R | **GPIO33** | EN pin of the AP3032, already has R57 = 100K pull-up to Core_5V, enabled by default |
| `TP_RST` (touch reset) | R37 = 0R | **GPIO23** | |
| `TP_INT` (touch interrupt) | R35 = **NC/0R, not populated by default** | GPIO2 (connected only if populated) | also routed to test point TP2 |
| `LCD_BL_PWM` (backlight dimming) | R43 = 0R | **GPIO26** | injected into the AP3032 FB node via R42 = 10K |

> **Backlight dimming is the "inject into FB" style, not direct LED switching.** The official BSP configures the LEDC channel with `.flags = {.output_invert = 1}`, i.e. higher duty → lower actual pin output → brighter. If you write your own driver you must include this inversion, otherwise the brightness curve is reversed.
>
> **`TP_INT` is disconnected by default**, which is why the official BSP sets `BSP_LCD_TOUCH_INT = GPIO_NUM_NC` and touch can only be polled. To use an interrupt you must populate R35 yourself.

Display FPC connector P2 (0.5mm pitch 30PIN, rear flip type, 2.0H):

| Pin | Net | Pin | Net |
|---|---|---|---|
| 1 | VLED- | 21 | VCC_1.8V / IOVCC |
| 2 | VLED+ | 22 | TE |
| 4 | ESP_3V3 | 23 | RESET |
| 6 / 7 | DSI_D0_P / DSI_D0_N | 25 | TP_RST |
| 9 / 10 | DSI_D1_P / DSI_D1_N | 26 | TP_SDA |
| 12 / 13 | DSI_CLK_P / DSI_CLK_N | 27 | TP_SCL |
| | | 28 | TP_INT |
| | | 29 | ESP_3V3 / TP_VDD |

Remaining onboard resource pins (from the official BSP header `bsp/esp32_p4_wifi6_touch_lcd_4_3.h`):

| Function | GPIO |
|---|---|
| I2S SCLK / MCLK / LCLK / DOUT / DSIN | 12 / 13 / 10 / 9 / 11 |
| Power amp enable `BSP_POWER_AMP_IO` | 53 |
| SD D0–D3 / CMD / CLK | 39 / 40 / 41 / 42 / 44 / 43 |

## Panel Parameters (from the official BSP, can be copied as-is)

The driver IC is **ST7701** (the BSP depends on `espressif/esp_lcd_st7701`), connected via MIPI-DSI:

| Parameter | Value |
|---|---|
| Resolution | 480 (H) × 800 (V) |
| Number of MIPI-DSI data lanes | 2 |
| Lane bit rate | 500 Mbps |
| DPI pixel clock | 30 MHz |
| Pixel format | RGB565 (16bpp), RGB888 optional |
| hsync back / pulse / front porch | 42 / 12 / 42 |
| vsync back / pulse / front porch | 2 / 8 / 60 |
| DSI PHY power supply | LDO_VO3 channel 3, 2500 mV |
| DBI command bit width | cmd 8 bit / param 8 bit |

The vendor-specific initialization sequence (39 commands, `0xFF` page switching + `0xB0/0xB1` gamma + `0xC0~0xCC` etc.) is in the official repository
`examples/esp-idf/08_lvgl_demo_v9/components/esp32_p4_wifi6_touch_lcd_4_3/esp32_p4_wifi6_touch_lcd_4_3.c`
at lines 24–73, `vendor_specific_init_default[]`; read it side by side with the register chapters of `ST7701-datasheet.pdf`.

## Touch (GT911)

- The I²C bus is shared with the codec / other I²C slaves: SCL = GPIO8, SDA = GPIO7
- Address: the official BSP probes **0x5D** first (`ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS`), falling back to **0x14** (`..._ADDRESS_BACKUP`) — either address is possible on this board, so probing is mandatory
- Reset: GPIO23; interrupt: NC by default (see above)
- For the coordinate report format, the status register `0x814E`, and the config-area `0x8047–0x80FE` checksum refresh flow, see `GT911-programming-guide.pdf`

## Migration Results for This Project (see `board_pinout.md`)

The "old use" column in the table below only describes the pre-migration 2.4-inch carrier board; the current Rev1.2 4.3-inch firmware has already
resolved these conflicts.

| GPIO | Old 2.4-inch Use | Rev1.2 Onboard Use | Current Handling |
|---|---|---|---|
| 7 / 8 | I²C0 (BNO085) | GT911 + audio + camera shared I²C | BNO085/BMP388 continue to share, no address conflict |
| 20 | BNO085 INT | BAT_ADC, and not broken out on J3 | BNO085 INT moved to GPIO34 (J3-28, carrier board net `IMU_INT`); firmware still polls |
| 23 | DOWN button | TP_RST | legacy four-button task disabled |
| 26 | TARE button | LCD_BL_PWM | leveling migrated to the touch UI |
| 27 | BMP388 INT | LCD RESET | BMP388 INT moved to GPIO31 (J3-24, carrier board net `BARO_INT`); firmware still polls |
| 33 | GPS RX | BL_EN | GPS P4-side RX moved to GPIO51 (GPIO33 not occupied); GPIO50 reserved for PPS |

The following connectors must also be distinguished:

- H1: the `USB TO UART` Type-C used for P4 flashing / serial logging.
- H2: the P4 native USB 2.0 HS OTG Type-C, on the same nets as J3-25/27. When the Pilot Kit
  carrier board is installed, the RTL-SDR plugs into the carrier board's USB-A (routed via J3-27/25), and H2 must be left empty; only for bare-board
  bench debugging should the dongle be plugged into H2.
- P1: the C6's `TX RX IO9 GND` UART download pin header, not USB.
- H4: the speaker connector, not a C6 debug header.

## J3 Pin Reassignment (2026-07-31, due to 60mm PCB routing)

In the original pin assignment, IMU_RST (GPIO21/Pin15), GPS_RX (GPIO32/Pin31) and GPS_PPS (GPIO46/Pin35) were all on
the bottom row of J3 (odd pins). Since the bottom row of the narrow 60mm board sits right against the PCB edge with no routing space, these three function signals were swapped with the adjacent top-row
pins:

| Function Signal | Old GPIO / Pin | New GPIO / Pin |
|---|---|---|
| IMU_RST | GPIO21 / Pin15 (bottom row) | GPIO28 / Pin16 (top row) |
| GPS_RX | GPIO32 / Pin31 (bottom row) | GPIO49 / Pin32 (top row) |
| GPS_PPS | GPIO46 / Pin35 (bottom row) | GPIO50 / Pin34 (top row) |

GPS_TX remains at Pin36 (GPIO51, top row), unchanged. Pin definitions in the firmware have been updated accordingly.

> The `GPS_RX` / `GPS_TX` in this section are **carrier board PCB net names**, using the module's perspective:
> `GPS_RX` (GPIO49) is the RX of the GPS module, i.e. the **P4's TX**; `GPS_TX`
> (GPIO51) is the TX of the GPS module, i.e. the **P4's RX**. `board_pinout.md`
> and the firmware constants consistently use the P4 perspective; the two directions are opposite, do not mix them.

## FAQ (abridged translation from the official text)

- **Recommended ESP-IDF version?** v5.5.1 ~ v5.5.4.
- **Flashing reports `bootloader.bin requires chip revision in range [v3.1 - v3.99] (this chip is revision v1.3)` — what to do?** Change the chip revision in menuconfig.
- **What is the maximum camera resolution supported by the ESP32-P4?** 2 megapixels. The P4 has a built-in ISP and H.264 encoder, with encoding performance capped at 1080p@30fps.
- **How to flash firmware to the ESP32-C6?** Pull `C6_IO9` low at power-up to put the C6 into download mode, put the P4 into download mode as well, then flash via `C6_U0RXD` / `C6_U0TXD`. The C6 ships with firmware preloaded.
- **Can PlatformIO / MicroPython be used?** Not recommended for now; the official recommendation is to use ESP-IDF at this stage.
