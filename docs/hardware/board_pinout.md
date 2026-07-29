# Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 pinout

> ⚠️ **部分内容未随 4.3″ Rev1.2 更新（2026-07-29 实测发现）**
>
> 本文虽声称基于 Rev1.2 原理图，但下列条目仍是 2.4″ 载板 + 核心板的旧数据，
> 已导致两次错误结论：
>
> | 本文说法 | 4.3″ Rev1.2 实际（wiki + 实测） |
> |---|---|
> | `P1` = USB HS OTG 排针，RTL-SDR 数据路径 | `P1` 是 **C6 的 `TX RX IO9 GND` UART 下载排针，不是 USB** |
> | RTL-SDR 接 P1 排针 | 接 **H2「USB OTG」Type-C**（USB 2.0 HS） |
> | GPIO20 = BNO085 INT | **BAT_ADC**（电池电压检测，未从 J3 引出） |
> | 无电池检测硬件 | **MX1.25 锂电池座 3.7 V，支持充放电** |
>
> 涉及接口、供电、GPIO 归属时，以
> [`ESP32-P4-WIFI6-Touch-LCD-4.3-wiki.md`](ESP32-P4-WIFI6-Touch-LCD-4.3-wiki.md)
> 和 [`ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf`](ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf)
> 为准，不要引用本文。


Chinese version: [`board_pinout-zh_CN.md`](board_pinout-zh_CN.md)

This document is derived from the **Waveshare Rev1.2 schematic** for the
4.3-inch board:

- [`ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf`](ESP32-P4-WIFI6-Touch-LCD-4.3-schematic.pdf)
  is the board-level authority.
- [`ESP32-P4-WIFI6-Touch-LCD-4.3-wiki.md`](ESP32-P4-WIFI6-Touch-LCD-4.3-wiki.md)
  records the official BSP parameters and local datasheet references.
- Firmware behavior is verified against `firmware/main/` and
  `firmware/sdkconfig.defaults`.

Labels used below:

- **Board** — fixed by the Rev1.2 PCB and cannot be remapped in firmware.
- **Firmware** — behavior implemented by the current Pilot Kit build.
- **Project** — external Pilot Kit wiring; change the wiring and firmware
  together if this allocation changes.

> Do not use `ESP32-P4-WIFI6-datasheet.pdf` as pinout evidence for this board.
> Despite its filename, it is the schematic of a different ESP32-P4-WIFI6
> board. The Rev1.2 PDF above is the only board schematic used here.

## 1. Identify the connectors first

Several old project documents used connector names from the former 2.4-inch
carrier. Those names are not valid on the 4.3-inch board.

| Ref. | Physical connector | Purpose |
|---|---|---|
| **H1** | USB-C, silkscreen `USB TO UART` | CH343P bridge for P4 flashing and serial console |
| **H2** | USB-C, silkscreen `USB` | Native ESP32-P4 USB 2.0 High-Speed OTG; shares its data nets with J3 pins 25/27 |
| **P1** | 1×4, 2.54 mm header, silkscreen `TX RX IO9 GND` | ESP32-C6 UART download header; **not USB** |
| **J3** | 2×20, 2.54 mm header | Power, GPIO and two USB signal pairs; only partially Raspberry Pi HAT-like |
| **P2** | 30-pin, 0.5 mm display/touch FFC | MIPI-DSI LCD, backlight-related panel nets and GT911 touch |
| **J1** | 15-pin camera FFC | 2-lane MIPI-CSI camera |
| **J2** | 2-pin battery connector | 3.7 V Li-ion/LiPo battery |
| **H8** | 2-pin RTC battery connector | Rechargeable RTC battery only |
| **H4** | 2-pin speaker connector | 8 Ω / 2 W speaker output |

The board has three buttons: **Key1 RESET**, **Key2 BOOT** and
**Key3 POWER**. It does not have the former carrier's four application keys.

## 2. J3 40-pin expansion header

### Physical silkscreen view

This section fixes the viewing direction to the user's view while facing the
physical board, **without mirroring**:

- upper-left is `GND`, lower-left is `GPIO48`
- upper-right is `ESP_3V3`, lower-right is `VCC_5V`
- reading left to right, J3 pin numbers descend from 40/39 to 2/1

A mating HAT footprint is mirrored when viewed from its connector side, so
verify the footprint orientation again before routing.

#### Horizontal quick-reference: facing the board, left to right

| Facing the board | left 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 right |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **Upper silkscreen** | GND | 52 | 51 | 50 | 49 | 35 | 34 | GND | 31 | 30 | 29 | 3V3 | 28 | 4 | 3 | GND | 2 | SCL | SDA | 3V3 |
| **Actual upper net** | GND | GPIO52 | GPIO51 | GPIO50 | GPIO49 | GPIO35 | GPIO34 | GND | GPIO31 | GPIO30 | GPIO29 | ESP_3V3 | GPIO28 | GPIO4 | GPIO3 | GND | GPIO2 | GPIO8 | GPIO7 | ESP_3V3 |
| **Upper J3 pin** | 40 | 38 | 36 | 34 | 32 | 30 | 28 | 26 | 24 | 22 | 20 | 18 | 16 | 14 | 12 | 10 | 8 | 6 | 4 | 2 |
| **Lower silkscreen** | 48 | 47 | 46 | GND | 32 | GND | DP | DM | 25 | 24 | GND | 22 | 21 | GND | 5 | 38 | 37 | GND | 5V | 5V |
| **Actual lower net** | GPIO48 | GPIO47 | GPIO46 | GND | GPIO32 | GND | `USBD_P` | `USBD_N` | GPIO25 | GPIO24 | GND | GPIO22 | GPIO21 | GND | GPIO5 | GPIO38 | GPIO37 | GND | VCC_5V | VCC_5V |
| **Lower J3 pin** | 39 | 37 | 35 | 33 | 31 | 29 | 27 | 25 | 23 | 21 | 19 | 17 | 15 | 13 | 11 | 9 | 7 | 5 | 3 | 1 |

#### Per-column detail: same physical direction

| Physical position (left→right) | Upper J3 pin | Actual upper net | Lower J3 pin | Actual lower net |
|---:|---:|---|---:|---|
| 1 | 40 | GND | 39 | GPIO48 |
| 2 | 38 | GPIO52 | 37 | GPIO47 |
| 3 | 36 | GPIO51 | 35 | GPIO46 |
| 4 | 34 | GPIO50 | 33 | GND |
| 5 | 32 | GPIO49 | 31 | GPIO32 |
| 6 | 30 | GPIO35 / BOOT and auto-download circuit | 29 | GND |
| 7 | 28 | GPIO34 | 27 | `USBD_P`, native USB HS D+, silkscreen `DP` |
| 8 | 26 | GND | 25 | `USBD_N`, native USB HS D−, silkscreen `DM` |
| 9 | 24 | GPIO31 | 23 | GPIO25 / `USB1P1_P` |
| 10 | 22 | GPIO30 | 21 | GPIO24 / `USB1P1_N` |
| 11 | 20 | GPIO29 | 19 | GND |
| 12 | 18 | ESP_3V3 | 17 | GPIO22 |
| 13 | 16 | GPIO28 | 15 | GPIO21 |
| 14 | 14 | GPIO4 | 13 | GND |
| 15 | 12 | GPIO3 | 11 | GPIO5 |
| 16 | 10 | GND | 9 | GPIO38 / P4 UART RX from CH343P |
| 17 | 8 | GPIO2 / optional TP_INT through unpopulated R35 | 7 | GPIO37 / P4 UART TX to CH343P |
| 18 | 6 | GPIO8 / shared I²C SCL, silkscreen `SCL` | 5 | GND |
| 19 | 4 | GPIO7 / shared I²C SDA, silkscreen `SDA` | 3 | VCC_5V |
| 20 | 2 | ESP_3V3 | 1 | VCC_5V |

```text
Silkscreen readable, J3 below the text:

left   upper J3-40 ───────────────────────────── J3-2   right
       lower J3-39 ───────────────────────────── J3-1
```

The native USB HS connection for the new HAT carrier is therefore:

- J3-27: `USBD_P` / `DP`
- J3-25: `USBD_N` / `DM`
- J3-29 or J3-26: adjacent GND
- J3-1/J3-3: `VCC_5V`; pass it through a USB Host current-limited switch
  before connecting it to dongle VBUS

The silkscreen labels `25`/`24` mean GPIO25/GPIO24 and carry the separate
Full-Speed `USB1P1_P/N` pair. They are not the native USB HS `DP`/`DM` pair.

### Schematic pin-number order

The following table starts at schematic pin 1 and therefore reads **right to
left relative to the physical board table above**. Prefer the physical-view
table above when wiring or drawing the mating HAT footprint.

| Odd pin | Net / board use | Even pin | Net / board use |
|---:|---|---:|---|
| 1 | VCC_5V | 2 | ESP_3V3 |
| 3 | VCC_5V | 4 | GPIO7 / shared I²C SDA |
| 5 | GND | 6 | GPIO8 / shared I²C SCL |
| 7 | GPIO37 / P4 UART TX to CH343P | 8 | GPIO2 / optional TP_INT through unpopulated R35 |
| 9 | GPIO38 / P4 UART RX from CH343P | 10 | GND |
| 11 | GPIO5 | 12 | GPIO3 |
| 13 | GND | 14 | GPIO4 |
| 15 | GPIO21 | 16 | GPIO28 |
| 17 | GPIO22 | 18 | ESP_3V3 |
| 19 | GND | 20 | GPIO29 |
| 21 | GPIO24 / USB1P1_N | 22 | GPIO30 |
| 23 | GPIO25 / USB1P1_P | 24 | GPIO31 |
| 25 | USBD_N, native USB HS D− | 26 | GND |
| 27 | USBD_P, native USB HS D+ | 28 | GPIO34 |
| 29 | GND | 30 | GPIO35 / BOOT and auto-download circuit |
| 31 | GPIO32 | 32 | GPIO49 |
| 33 | GND | 34 | GPIO50 |
| 35 | GPIO46 | 36 | GPIO51 |
| 37 | GPIO47 | 38 | GPIO52 |
| 39 | GPIO48 | 40 | GND |

J3 exposes **26 numbered P4 GPIOs**:
`2, 3, 4, 5, 7, 8, 21, 22, 24, 25, 28, 29, 30, 31, 32, 34, 35, 37,
38, 46, 47, 48, 49, 50, 51, 52`.

It does **not** expose GPIO20, GPIO23, GPIO26, GPIO27 or GPIO33. Older
left/right header drawings that show those pins belong to the previous board.

Important cautions:

- J3 is not electrically pin-compatible with a complete Raspberry Pi 40-pin
  header. Check every pin before attaching a HAT.
- J3 pins 25/27 share the native USB HS data nets with H2. Use either H2 or
  the HAT USB-A port, never both.
- GPIO24/25 are a separate Full-Speed USB Serial/JTAG pair. They are not the
  H2 native HS pair.
- GPIO35 is BOOT and is also driven by the CH343P auto-download circuit.
- GPIO37/38 are shared with the on-board CH343P. External UART use may contend
  with H1.
- GPIO2 reaches the touch interrupt net only if optional 0 Ω resistor R35 is
  populated. It is open by default.

### Recommended J3 allocation for Pilot Kit

| Function | J3 connection | Status |
|---|---|---|
| Shared I²C SDA / SCL | GPIO7 / GPIO8 | **Project + firmware** |
| BNO085 reset | GPIO21 | **Project + firmware** |
| BNO085 interrupt | Not connected | Firmware polls; GPIO20 is not on J3 and is BAT_ADC |
| BMP388 interrupt | GPIO31 optional | Reserved by project; current driver polls |
| GPS UART1 TX / RX | GPIO32 / GPIO51 | **Project + firmware**, 9600 8N1 |
| GPS PPS | GPIO46 optional | Wiring reservation only; current firmware does not consume PPS |
| RTL-SDR USB | J3-27 `DP` / J3-25 `DM` | **New HAT carrier**; leave H2 empty and feed VBUS from J3 `VCC_5V` through a current-limited switch |

Reasonable general-purpose candidates, when the optional project functions
above are unused, are GPIO5, GPIO22, GPIO28, GPIO29, GPIO30, GPIO34,
GPIO47, GPIO48, GPIO49, GPIO50 and GPIO52.

Use these only after considering their caveats:

- GPIO2/3/4 overlap default JTAG functions.
- GPIO24/25 overlap USB Serial/JTAG.
- GPIO31 is reserved for a possible BMP388 interrupt.
- GPIO32/51 are the current GPS UART.
- GPIO46 is reserved for a possible GPS PPS input.
- GPIO35 and GPIO37/38 should normally be left to boot/console circuitry.

## 3. Complete board-level GPIO ownership

This table prevents internal board signals from being mistaken for free pins.

| GPIO | Rev1.2 board connection | J3? |
|---:|---|:---:|
| 0, 1 | 32.768 kHz crystal | No |
| 2 | Optional GT911 TP_INT through R35 (not fitted) | 8 |
| 3 | Expansion; default JTAG MTDI | 12 |
| 4 | Expansion; default JTAG MTMS | 14 |
| 5 | Expansion | 11 |
| 6 | ESP32-C6 IO2 through R33, 0 Ω | No |
| 7 | Shared I²C SDA: touch, audio, camera and J3 | 4 |
| 8 | Shared I²C SCL: touch, audio, camera and J3 | 6 |
| 9 | I²S DSDIN, P4 to ES8311 | No |
| 10 | I²S LRCK | No |
| 11 | I²S ASDOUT, ES7210 to P4 | No |
| 12 | I²S SCLK | No |
| 13 | I²S MCLK | No |
| 14–17 | P4↔C6 SDIO D0–D3 | No |
| 18, 19 | P4↔C6 SDIO CLK, CMD | No |
| 20 | BAT_ADC, battery voltage divided by 3 | No |
| 21, 22 | Expansion | 15, 17 |
| 23 | GT911 TP_RST | No |
| 24, 25 | USB1P1_N/P Full-Speed USB Serial/JTAG | 21, 23 |
| 26 | LCD_BL_PWM | No |
| 27 | LCD RESET | No |
| 28–32 | Expansion | 16, 20, 22, 24, 31 |
| 33 | BL_EN, pulled up by 100 kΩ | No |
| 34 | Expansion | 28 |
| 35 | BOOT plus CH343P auto-download | 30 |
| 36 | 10 kΩ pull-up only; not exposed | No |
| 37 | P4 UART TX to CH343P RXD | 7 |
| 38 | P4 UART RX from CH343P TXD | 9 |
| 39–42 | microSD D0–D3 | No |
| 43, 44 | microSD CLK, CMD | No |
| 45 | microSD power-switch control | No |
| 46–52 | Expansion | 35, 37, 39, 32, 34, 36, 38 |
| 53 | NS4150B PA_CTRL | No |
| 54 | ESP32-C6 CHIP_PU/EN through R34, 0 Ω | No |

Dedicated non-GPIO nets carry MIPI-DSI, MIPI-CSI, native USB HS and external
flash. They must not be assigned GPIO numbers.

## 4. Display, touch and backlight

### ST7701 MIPI-DSI display

The panel is natively 480×800 portrait. The current firmware presents an
800×480 landscape UI.

| Item | Board / current firmware |
|---|---|
| DSI | 2 lanes at 500 Mbit/s per lane |
| DPI framebuffer | 480×800, RGB565, 30 MHz pixel clock |
| Horizontal porch | back 42 / pulse 12 / front 42 |
| Vertical porch | back 2 / pulse 8 / front 60 |
| DSI PHY supply | ESP LDO_VO3 channel 3 at 2.5 V |
| LCD reset | GPIO27, active low; 10 kΩ pull-down |
| Backlight PWM | GPIO26 into AP3032 feedback network |
| Backlight enable | GPIO33; 100 kΩ pull-up, default enabled |
| Firmware transform | PPA 270° counter-clockwise, equivalent to 90° clockwise, with byte swap |
| Framebuffers | Two native 480×800 DPI buffers, switched at VSYNC |

The LEDC PWM on GPIO26 is output-inverted because PWM is injected into the
AP3032 feedback node. Current firmware uses 5 kHz, 10-bit duty control,
a 0–255 application brightness scale and starts at 180. It does not actively
drive BL_EN.

### P2 display/touch FFC

| Pin | Net | Pin | Net |
|---:|---|---:|---|
| 1 | VLED− | 16 | NC |
| 2 | VLED+ | 17 | NC |
| 3 | NC | 18 | NC |
| 4 | ESP_3V3 | 19 | NC |
| 5 | NC | 20 | NC |
| 6 | DSI_D0_P | 21 | VCC_1.8V / IOVCC |
| 7 | DSI_D0_N | 22 | TE, not routed onward to P4 |
| 8 | NC | 23 | LCD RESET |
| 9 | DSI_D1_P | 24 | NC |
| 10 | DSI_D1_N | 25 | TP_RST |
| 11 | NC | 26 | TP_SDA |
| 12 | DSI_CLK_P | 27 | TP_SCL |
| 13 | DSI_CLK_N | 28 | TP_INT |
| 14 | NC | 29 | ESP_3V3 / TP_VDD |
| 15 | NC | 30 | GND |

### GT911 touch

- **Board:** SDA GPIO7, SCL GPIO8, reset GPIO23.
- **Board:** TP_INT can reach GPIO2 through R35, but R35 is not fitted.
- **Firmware:** 400 kHz I²C; probe address `0x5D`, then `0x14`; polling mode.
- **Firmware:** maps native 480×800 coordinates to the logical 800×480 UI.
- **Hardware capability:** GT911 supports up to five contacts.
- **Current firmware:** consumes only the first contact.

## 5. ESP32-C6 wireless co-processor

The ESP32-P4 has no built-in Wi-Fi or Bluetooth. The on-board
ESP32-C6-MINI-1-N4 supplies Wi-Fi 6 and Bluetooth 5 LE through
ESP-Hosted.

| Function | P4 GPIO | C6 pad |
|---|---:|---|
| SDIO D0 | 14 | IO20 |
| SDIO D1 | 15 | IO21 |
| SDIO D2 | 16 | IO22 |
| SDIO D3 | 17 | IO23 |
| SDIO CLK | 18 | IO19 |
| SDIO CMD | 19 | IO18 |
| C6 IO2 | 6 | IO2 |
| C6 CHIP_PU / EN | 54 | EN |

All six SDIO lines have 51 kΩ pull-ups on the C6 side. GPIO54 reaches C6 EN
directly through R34, 0 Ω; Rev1.2 has **no reset inverter**. The current,
hardware-verified project configuration remains:

```text
CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=54
CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y
```

Do not explain that software option with a nonexistent board inverter.

The C6 UART download header is **P1**:

| P1 | Signal | USB-UART wiring |
|---:|---|---|
| 1 | C6_U0TXD | Adapter RX |
| 2 | C6_U0RXD | Adapter TX |
| 3 | C6_IO9 | Pull low while powering/resetting C6 to enter download |
| 4 | GND | Adapter GND |

H4 is the speaker connector, not the C6 header.

## 6. USB paths and debug console

| Path | Electrical connection | Use |
|---|---|---|
| H1 `USB TO UART` | USB-C → CH343P → P4 GPIO38 RX / GPIO37 TX | Flashing and `idf.py monitor` |
| H2 `USB` | USB-C → dedicated USBD_N/P | Native USB 2.0 HS OTG; shares the J3 HS data nets |
| J3 pins 21/23 | GPIO24/25, USB1P1_N/P | Separate Full-Speed USB Serial/JTAG |
| J3 pins 25/27 | Dedicated USBD_N/P | RTL-SDR USB HS data path on the new HAT carrier |

The new HAT carrier should route J3-27 `DP` and J3-25 `DM` to a USB-A
receptacle while leaving H2 empty. Feed USB-A VBUS from J3 `VCC_5V` through a
500 mA USB Host current-limited switch. Do not follow obsolete instructions
for a four-pin P1/MX1.25 USB cable.

## 7. microSD

The on-board socket uses SDMMC Slot 0 in 4-bit mode.

| Signal | GPIO |
|---|---:|
| D0 | 39 |
| D1 | 40 |
| D2 | 41 |
| D3 | 42 |
| CLK | 43 |
| CMD | 44 |
| Power-switch control | 45 |

CMD and all data lines have 51 kΩ pull-ups to ESP_LDO_VO4. There is no
independent card-detect GPIO; D3 shares the socket's CD contact. The card
supply is controlled by a P-channel MOSFET whose gate is GPIO45, with a 10 kΩ
default pull-down.

**Firmware:** ESP-Hosted already initializes the shared SDMMC controller for
C6 Slot 1. The microSD driver therefore uses Slot 0 with dummy host
`init`/`deinit` callbacks to avoid reinitializing the controller.

## 8. Camera, audio and storage

### J1 MIPI-CSI camera

| Pin | Net | Pin | Net |
|---:|---|---:|---|
| 1 | GND | 9 | CSI_CLK_P |
| 2 | CSI_D0_N | 10 | GND |
| 3 | CSI_D0_P | 11 | CSI_IO0 |
| 4 | GND | 12 | CSI_IO1 |
| 5 | CSI_D1_N | 13 | ESP_I2C_SCL |
| 6 | CSI_D1_P | 14 | ESP_I2C_SDA |
| 7 | GND | 15 | ESP_3V3 |
| 8 | CSI_CLK_N |  |  |

CSI_IO0 has a 10 kΩ pull-up; the optional CSI_IO1 pull resistor is not fitted.
The schematic does not assign GPIO numbers to CSI_IO0/1. Camera capture is not
implemented in the current Pilot Kit firmware.

### Audio

| Device / net | Connection |
|---|---|
| ES8311 codec | I²C address `0x18` in the official BSP |
| ES7210 four-channel ADC | I²C address `0x40`; two on-board microphones and playback-reference AEC path |
| I²C | SDA GPIO7 / SCL GPIO8 |
| I²S MCLK / SCLK / LRCK | GPIO13 / GPIO12 / GPIO10 |
| I²S DSDIN / ASDOUT | GPIO9 / GPIO11 |
| NS4150B amplifier enable | PA_CTRL GPIO53 |
| Speaker | H4, 8 Ω / 2 W recommended |

The current Pilot Kit firmware does not initialize the audio subsystem.

### Memory

- ESP32-P4NRW32: 32 MB in-package PSRAM.
- GD25Q256EYIGR: 256 Mbit / 32 MB external NOR flash on dedicated flash pins.
- ESP32-C6-MINI-1-N4: wireless co-processor module; its flash size is implied
  by the N4 module variant, not separately stated on the schematic.

## 9. Power and batteries

- J2 accepts a 3.7 V Li-ion/LiPo battery. ETA6098 handles charging and SCT12
  boosts the battery rail into the board's 5 V system.
- BAT_ADC is connected to GPIO20 through a 200 kΩ / 100 kΩ divider, so the ADC
  sees approximately one third of the battery voltage.
- Key3 POWER: short press powers on; hold for about two seconds to power off
  (official board behavior).
- Key1 RESET pulls ESP_EN low. Key2 BOOT pulls GPIO35 low.
- H8 is for a **rechargeable RTC battery only**. Do not install a
  non-rechargeable coin cell because the board supplies a charging path.
- The PWR LED is tied to Core_5V and is not software-controlled.

## 10. Pilot external modules

### BNO085 IMU

| BNO085 pin | Connection |
|---|---|
| VCC / GND | J3 ESP_3V3 / GND |
| SDA / SCL | GPIO7 / GPIO8 |
| RST | GPIO21 |
| INT | Not connected; current driver polls |
| AD0 / PS1 / PS0 | GND / GND / GND |
| CS | ESP_3V3 |

The current driver supports address `0x4A`; wire AD0 low. GPIO20 must not be
used for BNO085 INT: it is not on J3 and is hardwired to BAT_ADC.

The current firmware mounting transform assumes the IMU board is vertical,
with the chip face toward the pilot, its header on the pilot's left and VCC
at the top. Under that installation, chip +X maps to aircraft up, chip +Y to
aircraft left and chip +Z to aircraft back. The firmware applies
`q_body_fix = (0, 0.7071068, 0, -0.7071068)`. If the sensor is mounted in the
canonical aircraft axes instead, update the transform and revalidate attitude
before flight.

### BMP388 barometer

| BMP388 pin | Connection |
|---|---|
| VCC / GND | ESP_3V3 / GND |
| SDA / SCL | GPIO7 / GPIO8 |
| SDO / CSB | GND / ESP_3V3 for address `0x76` |
| INT | Optional GPIO31; current driver polls |

### GPS

| GPS pin | Connection |
|---|---|
| TXD | GPIO51, GPS → P4 UART1 RX |
| RXD | GPIO32, P4 UART1 TX → GPS |
| PPS | Optional GPIO46 wiring only |
| VCC / GND | ESP_3V3 / GND |

Current firmware uses UART1 at 9600 8N1 and derives time from NMEA RMC.
It does not implement a GPIO interrupt or timing discipline from PPS.

## 11. Bring-up checklist

1. Use H1 to flash and monitor the P4; use P1 only to flash the C6.
2. Confirm `ST7701 DSI ready: logical 800x480 -> PPA 90 CW`.
3. Confirm touch tracks the full screen; TP_INT should remain open unless R35
   was intentionally populated.
4. Confirm BNO085 at `0x4A`, BMP388 at `0x76`, and GPS RX on GPIO51.
5. Confirm C6 ESP-Hosted starts before DSI initialization and no SDIO CMD5
   error appears.
6. Confirm microSD mounts through Slot 0 without reinitializing the shared
   SDMMC host.
7. Attach RTL-SDR to the new HAT USB-A port, confirm native USB HS enumeration
   through J3, and leave H2 empty.
8. Perform repeated cold boots before treating any wiring change as stable.

## 12. Legacy boundary

The old 2.4-inch SPI display and four-button carrier are historical material
under `docs/jlc/`. Their GPIO28/29/30/31/50 display wiring, GPIO26/5/22/23
application keys and old connector names must not be copied into the
Rev1.2 4.3-inch build.
