# Waveshare ESP32-P4-WIFI6 — pin & GPIO map

All assignments below are extracted directly from
`ESP32-P4-WIFI6-datasheet.pdf` (Waveshare schematic). When the
firmware references a GPIO number, this file is the source of truth.

## On-board peripherals

### MicroSD card slot (TF1, SDIO 3.0)

The TF card connector is wired for **SDMMC 4-bit (slot 1)** with
51 kΩ pull-ups to 3V3 on every data line and CMD.

| TF pin | Function | Pull-up | ESP32-P4 GPIO |
|---|---|---|---|
| 5 | CLK  | —    | **GPIO43** |
| 3 | CMD  | R10  | **GPIO44** |
| 7 | D0   | R11  | **GPIO39** |
| 8 | D1   | R12  | **GPIO40** |
| 1 | D2   | R8   | **GPIO41** |
| 2 | D3 / CD | R9   | **GPIO42** |

> Use the ESP-IDF `sdmmc_host` driver with `slot = SDMMC_HOST_SLOT_1`
> and the GPIOs above. The slot has no card-detect pin wired to a
> P4 GPIO — implement detection by mount-retry, or assume always
> inserted.

### ESP32-C6 co-processor (SDIO host, for ESP-Hosted / Wi-Fi remote / BLE)

The C6-MINI-1 is connected to the P4 as an **ESP-Hosted slave over
SDIO**. Wi-Fi and BLE traffic from the application is proxied across
this bus. The C6 itself must be flashed with the matching
`esp_hosted` slave firmware.

| C6 pad | Function | ESP32-P4 GPIO |
|---|---|---|
| IO19 | CLK  | **GPIO18** |
| IO18 | CMD  | **GPIO19** |
| IO12 | D0   | **GPIO14** |
| IO13 | D1   | **GPIO15** |
| IO14 | D2   | **GPIO16** |
| IO15 | D3   | **GPIO17** |
| EN   | RESET (active low) | **GPIO54** |
| IO2  | (strap)  | GPIO6 |

These match the Espressif P4 eval board's reference layout, so
upstream `espressif/esp_hosted` examples work without GPIO overrides
(only the `BOARD_*_PIN` defines have to be set).

### C6 debug header (H4, 4-pin)

| H4 pin | Net | Purpose |
|---|---|---|
| 1 | C6_IO9 | Hold low at reset → C6 enters bootloader |
| 2 | GND | |
| 3 | C6_U0RXD | C6 UART0 RX (programming) |
| 4 | C6_U0TXD | C6 UART0 TX (programming) |

The C6 is flashed via this header using an external USB-UART, with
the C6 in bootloader mode. Not connected to any P4 GPIO — it's a
direct standalone path.

### USB-OTG HS (the RTL-SDR port)

ESP32-P4 chip pins 49 (USB_DM) / 50 (USB_DP) are dedicated USB 2.0
HS PHY signals — **not GPIOs**. They route directly to the on-board
MX1.25 4-pin connector P1 (`V D- D+ G`).

The ESP-IDF USB host stack uses these pins automatically once
`peripheral_map = BIT0` is set in `usb_host_config_t` (already done
in `firmware/main/main.c`).

The board's 2x20 pin headers do also expose **GPIO24 / GPIO25**
labelled "DM/GPIO24" / "DP/GPIO25" on the silkscreen — those are
the P4's USB-OTG **FS** alternate-PHY pins muxed onto GPIOs 24/25,
which is a *separate* USB controller that we do not use for the
RTL-SDR data path.

### USB-to-UART (Type-C, CH343P, debug / flashing)

The Type-C connector H2 attaches to a CH343P bridge (U4). The
P4-side bridge UART pins are:

| Signal | ESP32-P4 GPIO |
|---|---|
| RXD (PC → P4) | **GPIO37** |
| TXD (P4 → PC) | **GPIO38** |

This is the default `idf.py monitor` console. Firmware should not
reassign these.

GPIO35 is wired through R35 (4.7 kΩ) to the auto-reset / auto-bootloader
transistor pair (U5, MMDT3906DW). **Do not use GPIO35** as an
application GPIO — it would fight the auto-reset circuit.

### I²C0 (codec + future BNO085 IMU)

| Signal | ESP32-P4 GPIO |
|---|---|
| SDA | **GPIO7** |
| SCL | **GPIO8** |

Existing slaves:
- ES8311 audio codec at 7-bit address `0x18`

Planned slaves (Phase 4):
- BNO085 IMU at 7-bit address `0x4A` (or `0x4B` with SA0 strap)

No address conflict; both can co-exist on the bus.

### I²S0 (ES8311 codec, audio)

| Signal | ESP32-P4 GPIO |
|---|---|
| MCLK    | GPIO13 |
| SCLK    | GPIO12 |
| LRCK    | GPIO10 |
| DSDIN   | GPIO9  (P4 → codec, playback) |
| ASDOUT  | GPIO11 (codec → P4, capture) |

The codec also has an on-chip MIC preamp routed to the on-board
electret mic; Phase 4+ may use this for voice notes.

### Speaker amplifier (NS4150B, U11)

| Signal | ESP32-P4 GPIO |
|---|---|
| PA_CTRL (enable) | **GPIO53** |

Drives an 8 Ω 2 W speaker via the MX1.25 SPK header.

### On-board Flash (octal-SPI, GD25Q256EYIGR, 32 MB)

The Nor Flash is on the P4's dedicated MSPI bus (chip pins 27-33).
Not muxed to any GPIO. The default partition table fits comfortably
within 32 MB.

### Other on-board features

- **MIC** (built-in electret): wired through ES8311's MIC1P/N
- **Power LED (D1)**: 3V3 indicator, no GPIO control
- **Buttons**: BOOT (GPIO0) and RST (CHIP_PU); used at boot time only
- **MIPI-DSI display connector J2** (22-pin): for 5/7/8/10.1" DSI panels
- **MIPI-CSI camera connector J1** (22-pin): for OV5647 etc.
- **2×20 pin headers (J1/J2 backside)**: Raspberry Pi Pico-compatible 40-pin footprint

## Phase 4 add-on peripherals

Pins below are assigned by the firmware (not wired on-board); user
adds them via the 2x20 GPIO header.

### ST7789 LCD via SPI2 (TK024F3036 240×320 module, 4-wire SPI)

The TK024F3036 module is parallel-default; the user's driver-board
adapter pre-straps it to 4-wire SPI mode (IM1-2=1, IM0=0) and exposes
{CS, RST, DC, SCK, MOSI, BL} on a simple header. Voltage is 2.8 V on
both VCI and IOVCC; the driver board contains the LDO and (if needed)
a level shifter to interface with the P4's 3.3 V GPIOs.

| Signal | ESP32-P4 GPIO | IO_MUX direct? | Notes |
|---|---|---|---|
| SCLK | **GPIO47** | ✅ FSPICLK | Allows full-speed (≤80 MHz) SPI |
| MOSI | **GPIO45** | ✅ FSPID | Single-line write only (no MISO) |
| CS   | **GPIO46** | (software) | esp_lcd toggles it per transaction |
| DC   | **GPIO48** | — | Data/Command select (a.k.a. RS) |
| RST  | **GPIO49** | — | Hard reset, driven by panel driver |
| BL   | **GPIO50** | LEDC PWM | 20 kHz, 8-bit duty, brightness control |

Bus configured at 40 MHz in Phase 4a; can be pushed to 80 MHz if the
driver board / FPC layout proves clean enough.

### BNO085 IMU via I²C0 (SparkFun / Adafruit module, 9-axis sensor fusion)

Shares the existing I²C0 bus with the on-board ES8311 codec (different
addresses; no conflict). Phase 4b talks SH-2 / SHTP to the sensor and
enables the Rotation Vector report at 100 Hz.

| Signal | ESP32-P4 GPIO | Notes |
|---|---|---|
| SDA  | GPIO7 | (already on-board) |
| SCL  | GPIO8 | (already on-board) |
| HOST_INT | **GPIO20** | Active-low; goes low when data ready |
| RST  | **GPIO21** | Active-low hard reset |

I²C 7-bit address: `0x4A` (default) or `0x4B` (if SA0 pulled high).

### Reserved for Phase 5 (config UI tact buttons)

| Signal | ESP32-P4 GPIO | Notes |
|---|---|---|
| BTN1 | GPIO26 | Reserved, not yet driven by firmware |
| BTN2 | GPIO27 | Reserved |

## Programmable GPIOs available to the application

After accounting for everything above, the following P4 GPIOs remain
free for future expansion (e.g. additional sensors, GPS NMEA UART,
status LEDs):

```
GPIO0  GPIO1  GPIO2  GPIO3  GPIO4  GPIO5  GPIO6
GPIO22 GPIO23
GPIO28 GPIO29 GPIO30 GPIO31 GPIO32 GPIO33
GPIO36 GPIO51 GPIO52
```

(GPIO0 / GPIO6 are strapping pins — use with care.)

That's ~16 freely-assignable GPIOs after Phase 4, suitable for a
NEO-M9N GPS UART, an external SPI flash, status LEDs, or whatever
Phase 5+ wants to add.

## Pin-assignment policy

When introducing new peripherals in later phases:

1. Pick from the "free GPIOs" list above.
2. Add a row to the relevant table in this file *before* writing the
   driver — drivers should `#include` numbers from a shared header
   that mirrors this file, not hard-code raw numbers.
3. Sanity-check against `firmware/sdkconfig.defaults` (some IDF
   defaults reserve GPIOs for JTAG / SPI flash / etc.).
