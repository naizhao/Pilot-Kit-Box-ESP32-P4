# Waveshare ESP32-P4-WIFI6 — pin & GPIO map

Chinese version: [`board_pinout-zh_CN.md`](board_pinout-zh_CN.md)

All assignments below are cross-checked against three sources:

1. `ESP32-P4-WIFI6-datasheet.pdf` — Waveshare schematic (board-level wiring).
2. `ESP32-P4-WIFI6-details-inter.jpg` — Waveshare silkscreen reference
   (the labelled board photo). This is the visual ground truth when
   wiring on a breadboard.
3. `ESP32-P4 Series Datasheet v1.1` (Espressif) — chip-level pin
   functions, strapping rules, and reserved pins.

When the firmware references a GPIO number, **this file is the source
of truth**. Update the relevant table here *before* changing any
driver, then propagate the change into the `*_PIN` defines in
`firmware/main/`.

The ESP32-P4 chip has 55 GPIOs total (GPIO0–GPIO54). The Waveshare
board exposes **27** of them on the user-facing 2×20 headers (the
others are consumed by on-board peripherals or are package-dedicated
signals — see §3).

---

## 1. User-facing headers — physical layout

Looking at the board with the USB-C / SD-card edge at the top, ESP32-C6
module at the bottom:

```
              ┌───────────────────────┐
       (top)  │ ◯ Type-C  ◯ MicroSD ◯ │  (top)
              ├───────────────────────┤
    GPIO52  ──┤                       ├──  VBUS         (5 V in)
    GPIO51  ──┤                       ├──  VSYS         (battery / 5 V)
       GND  ──┤                       ├──  GND
    GPIO31  ──┤                       ├──  EN           (chip enable)
    GPIO30  ──┤  ┌──────────────┐     ├──  3V3          (3.3 V out)
    GPIO29  ──┤  │              │     ├──  GPIO20
    GPIO28  ──┤  │   ESP32-P4   │     ├──  GPIO21
       GND  ──┤  │              │     ├──  GND
    GPIO50  ──┤  │              │     ├──  GPIO22
    GPIO49  ──┤  └──────────────┘     ├──  GPIO23
     GPIO5  ──┤                       ├──  RUN          (system reset)
     GPIO4  ──┤    [DISPLAY FFC]      ├──  GPIO26
       GND  ──┤                       ├──  GND
     GPIO3  ──┤    [CAMERA FFC]       ├──  GPIO27
     GPIO2  ──┤                       ├──  GPIO32
     GPIO8 ──┤    H4 (C6 debug)      ├──  GPIO33
     GPIO7 ──┤    IO9 GND RX  TX     ├──  GPIO46
       GND  ──┤                       ├──  GND
    GPIO24  ──┤    [USB MX1.25 P1]    ├──  GPIO47
    GPIO25  ──┤    V  D-  D+  G       ├──  GPIO48
              ├───────────────────────┤
   (bottom)   │  ◯  ESP32-C6 module ◯ │  (bottom)
              └───────────────────────┘
              (left header)         (right header)
```

Both headers count from the **top** (USB-C side) downward.

### Left header pins (top → bottom)

| Silkscreen | Net      | Current allocation             | Notes                                          |
|------------|----------|--------------------------------|------------------------------------------------|
| 52         | GPIO52   | free                           |                                                |
| 51         | GPIO51   | free                           |                                                |
| GND        | GND      | —                              | recommended LCD GND return                     |
| **31**     | GPIO31   | **LCD DC**                     | plain GPIO output (software-toggled)           |
| **30**     | GPIO30   | **LCD SCK**                    | SPI2_CK_PAD — IO_MUX direct                    |
| **29**     | GPIO29   | **LCD MOSI**                   | SPI2_D_PAD  — IO_MUX direct                    |
| **28**     | GPIO28   | **LCD CS**                     | SPI2_CS_PAD — IO_MUX direct                    |
| GND        | GND      | —                              |                                                |
| **50**     | GPIO50   | **LCD BL (LEDC PWM)**          | Backlight on TK024F304189-SPI.                 |
| 49         | GPIO49   | free (was LCD RST in an earlier bring-up build) | TK024F304189-SPI has on-board RC reset.        |
| **5**      | GPIO5    | **BTN2 — MODE** (LP_IO, deep-sleep wake) | LP_IO required so MODE long-press can sleep and a press can wake — see §3.4 and the power-button plan. JTAG MTDO default is moot: project doesn't use JTAG. |
| 4          | GPIO4    | free                           | ⚠ JTAG MTMS default — using disables JTAG.     |
| GND        | GND      | —                              |                                                |
| 3          | GPIO3    | free                           | ⚠ JTAG MTDI default — using disables JTAG.     |
| 2          | GPIO2    | free                           | ⚠ JTAG MTCK default — using disables JTAG.     |
| **SCL/8**  | GPIO8    | **I²C0 SCL** (codec + IMU)     |                                                |
| **SDA/7**  | GPIO7    | **I²C0 SDA** (codec + IMU)     |                                                |
| GND        | GND      | —                              |                                                |
| DM/24      | GPIO24   | reserved                       | ⚠ USB Serial/JTAG default (USB1P1_N0).          |
| DP/25      | GPIO25   | reserved                       | ⚠ USB Serial/JTAG default (USB1P1_P0).          |

### Right header pins (top → bottom)

| Silkscreen | Net      | Current allocation        | Notes                                          |
|------------|----------|---------------------------|------------------------------------------------|
| VBUS       | +5 V     | —                         | USB-C VBUS in. Shorted to USB connector.       |
| VSYS       | +5 V     | —                         | Battery / external 5 V input.                  |
| GND        | GND      | —                         |                                                |
| EN         | CHIP_PU  | —                         | Hold low → chip in reset.                      |
| 3V3        | +3.3 V   | —                         | 3.3 V output from on-board LDO (≤500 mA).      |
| **20**     | GPIO20   | **IMU INT** (BNO085, polled — not IRQ yet)  |                              |
| **21**     | GPIO21   | **IMU RST** (BNO085 active-low reset)       |                              |
| GND        | GND      | —                         |                                                |
| **22**     | GPIO22   | **BTN3 — UP** (list scroll / menu up)       |                                  |
| **23**     | GPIO23   | **BTN4 — DOWN** (list scroll / menu down)   |                                  |
| RUN        | RUN      | —                         | System reset button net.                       |
| **26**     | GPIO26   | **BTN1 — IMU Tare / cage**| Short = live software tare; long ≥ 3 s = persist software tare to NVS; very-long ≥ 10 s = IMU factory reset |
| GND        | GND      | —                         |                                                |
| **27**     | GPIO27   | **BARO_INT** (BMP388, carrier board) | data-ready IRQ; driver implemented (`baro_task.c`, polling ~10 Hz) |
| **32**     | GPIO32   | **GPS RX** (carrier board) | P4 UART **TX** → GPS RXD; driver implemented (`gps_task.c`, UART1 NMEA) |
| **33**     | GPIO33   | **GPS TX** (carrier board) | GPS TXD → P4 UART **RX**; (was LCD MOSI in an earlier build) |
| **46**     | GPIO46   | **GPS PPS** (carrier board) | 1 Hz pulse for time discipline; (was LCD CS earlier) |
| GND        | GND      | —                         | ⚠ Between GPIO46 and GPIO47 — easy to misplace |
| 47         | GPIO47   | free                      | (was LCD SCK in an earlier bring-up build)     |
| 48         | GPIO48   | free                      | (was LCD DC in an earlier bring-up build)      |

### Other access points (board midline)

- **BOOT button** — pulls **GPIO0** low at reset. Hold during power-on
  to enter UART download mode. **GPIO0 is a strapping pin and is not
  available as an application GPIO** (it's not broken out anyway).
- **RESET button** — pulls CHIP_PU low. Standalone reset.
- **MIC** — board's electret, routed through the ES8311 codec.
- **DISPLAY FFC (J2, 22-pin)** — MIPI-DSI panel connector. Uses
  dedicated DSI pins on the chip; does not consume GPIOs.
- **CAMERA FFC (J1, 22-pin)** — MIPI-CSI camera connector. Same:
  dedicated CSI pins, no GPIO cost.
- **H4 (4-pin C6 debug header)** — `IO9 / GND / RXD / TXD`. Flashes
  the on-board ESP32-C6 module via an external USB-UART. Not on any
  P4 GPIO — standalone path.
- **USB (MX1.25 4-pin P1)** — USB 2.0 **HS** OTG. Wired straight to
  ESP32-P4 chip pins 49/50 (the dedicated USB HS PHY). This is the
  RTL-SDR data path. **Not GPIO24/25** — that's a separate FS PHY.

---

## 2. On-board peripherals (firmware-fixed)

These GPIOs and signals are committed by the Waveshare board itself
(not by the firmware) and **cannot be reassigned**. Most aren't even
broken out to the user headers — they're internal traces.

### MicroSD card slot (TF1, SDMMC 4-bit)

51 kΩ pull-ups to 3V3 on every data line and CMD; use **SDMMC Slot 0**
(GPIO39-44 are the P4's dedicated Slot 0 pins — Slot 1 is taken by the
ESP-Hosted SDIO link to the C6). On IDF ≥ 6.0 the SDMMC host controller
can only be initialised once (IDF issue #16233): ESP-Hosted initialises
it first, so the SD mount must stub out `host.init`/`host.deinit`
(see `firmware/main/pk_sdcard.c` and the esp_hosted
`examples/host_sdcard_with_hosted` workaround).

| TF pin   | Function   | ESP32-P4 GPIO |
|----------|------------|---------------|
| 5        | CLK        | GPIO43        |
| 3        | CMD        | GPIO44        |
| 7        | D0         | GPIO39        |
| 8        | D1         | GPIO40        |
| 1        | D2         | GPIO41        |
| 2        | D3 / CD    | GPIO42        |

No card-detect pin to a P4 GPIO — detect via mount-retry.

### ESP32-C6 co-processor (ESP-Hosted SDIO slave)

The C6-MINI-1 hosts Wi-Fi + BLE; the P4 talks to it as an ESP-Hosted
SDIO slave. Pins match Espressif's eval-board layout, so upstream
`esp_hosted` examples work without GPIO overrides.

| C6 pad | Function           | ESP32-P4 GPIO |
|--------|--------------------|---------------|
| IO19   | CLK                | GPIO18        |
| IO18   | CMD                | GPIO19        |
| IO12   | D0                 | GPIO14        |
| IO13   | D1                 | GPIO15        |
| IO14   | D2                 | GPIO16        |
| IO15   | D3                 | GPIO17        |
| EN     | RESET (P4 software view is active-high) | GPIO54        |
| IO2    | (boot strap)       | GPIO6         |

Configured via:
```
CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=54
CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y
```
(see `firmware/sdkconfig.defaults`).

On this Waveshare board, P4 GPIO54 reaches the C6 EN path through
board-level logic, so the reset signal is active-high from the P4
software point of view. Do not change this to active-low; the common
failure mode is SDIO CMD5 returning `0x107 INVALID_RESPONSE`.

### USB-to-UART bridge (CH343P U4) — `idf.py monitor` console

| Signal             | ESP32-P4 GPIO |
|--------------------|---------------|
| RXD (PC → P4)      | GPIO37        |
| TXD (P4 → PC)      | GPIO38        |

`GPIO35` is wired through R35 (4.7 kΩ) to the CH343P auto-reset /
auto-bootloader transistor pair (U5, MMDT3906DW). Don't use GPIO35
as an application GPIO — it would fight the auto-reset circuit.
(Moot in practice: GPIO35 isn't broken out anyway.)

### I²C0 (ES8311 codec + BNO085 IMU + BMP388 baro)

Bus is shared; addresses don't collide.

| Signal | ESP32-P4 GPIO | On left header? |
|--------|---------------|-----------------|
| SDA    | GPIO7         | yes (SDA/7)     |
| SCL    | GPIO8         | yes (SCL/8)     |

Slaves currently on the bus:
- **ES8311 audio codec** — `0x18` (on-board)
- **BNO085 IMU** — `0x4A` (or `0x4B` if SA0 strap pulled high)
- **BMP388 barometer** — `0x76` (SDO→GND) — carrier board; driver implemented (`baro_task.c`, polling ~10 Hz)

### I²S0 (ES8311 codec audio)

All on-board; not broken out to user headers.

| Signal | ESP32-P4 GPIO | Direction              |
|--------|---------------|------------------------|
| MCLK   | GPIO13        | P4 → codec             |
| SCLK   | GPIO12        | P4 ↔ codec             |
| LRCK   | GPIO10        | P4 → codec             |
| DSDIN  | GPIO9         | P4 → codec (playback)  |
| ASDOUT | GPIO11        | codec → P4 (capture)   |

### Speaker amplifier (NS4150B U11)

| Signal           | ESP32-P4 GPIO |
|------------------|---------------|
| PA_CTRL (enable) | GPIO53        |

Drives the 8 Ω 2 W speaker via the MX1.25 SPK header (board midline,
near the C6 module).

### On-board NOR flash (octal-SPI, GD25Q256EYIGR, 32 MB)

On the chip's dedicated MSPI bus (chip package pins 27–33). Not muxed
to any GPIO. Default partition table fits comfortably within 32 MB
(`CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y` already in `sdkconfig.defaults`).

### On-board PSRAM (32 MB, HEX-SPI on-die)

ESP32-P4NRW32 — internal stacked PSRAM, runs at 200 MHz in HEX mode
on v1.x silicon. Bound to chip-package pins, not muxed to any GPIO.

### Power LED (D1)

3V3 indicator only; no GPIO control.

---

## 3. Current add-on peripherals (user-wired)

### ST7789 LCD via SPI2 — TK024F3036 module on `TK024F304189-SPI` breakout

**Status**: Verified working (2026-05-16). PFD renders over SPI2
IO_MUX direct on the left header at 40 MHz; `display: TK024F3036
320x240 ready` followed by `pfd: PFD 30 FPS` in the serial log.

The bare TK024F3036 FPC is **parallel-default** (39-pin). The
`TK024F304189-SPI` breakout (silkscreen "TK024F304189-SPI" on the back
edge) hard-straps `IM1_2 = 3.3V` to force the panel into 4-wire SPI
mode, adds an RC reset network on `RST`, and exposes the SPI signals
on a `P3` 13-pin header (and on two 1×4 PH2.0 sockets, electrically
identical to the 13-pin header — pick whichever connector is convenient).

The `P3` silkscreen reads, top-to-bottom on the breakout:

```
GND  3V3  BL  D/C  CS  SCK  MISO  MOSI    ← LCD signals
T_CS  T_SCK  T_MISO  T_MOSI  T_INT       ← XPT2046 touch IC (not used)
```

Wire as follows (all signals on the **left** header):

| Breakout label | ESP32-P4 pin | Header position           | Notes                                       |
|----------------|--------------|---------------------------|---------------------------------------------|
| GND            | any GND      | left (right under GPIO 28) | several to choose from                     |
| 3V3            | 3V3          | right header              | 3.3 V from on-board LDO                     |
| BL             | **GPIO50**   | left header               | LEDC PWM, 20 kHz, 8-bit duty                |
| D/C            | **GPIO31**   | left header               | plain GPIO output (software-toggled)        |
| CS             | **GPIO28**   | left header               | **SPI2_CS_PAD — IO_MUX direct**             |
| SCK            | **GPIO30**   | left header               | **SPI2_CK_PAD — IO_MUX direct**             |
| MISO           | not wired    | —                         | `PK_LCD_PIN_MISO = -1`                      |
| MOSI           | **GPIO29**   | left header               | **SPI2_D_PAD — IO_MUX direct**              |

The five `T_*` pins on `P3` are the resistive-touch lines into the
on-breakout XPT2046 IC. Leave unconnected — the current firmware does
not drive touch input.

Physical layout (left header, top → bottom around the LCD signals):

```
   GND        ← (optional LCD GND return)
   GPIO 31    ← LCD D/C
   GPIO 30    ← LCD SCK
   GPIO 29    ← LCD MOSI
   GPIO 28    ← LCD CS
   GND        ← (good LCD GND return, short trace back to host)
   GPIO 50    ← LCD BL
   GPIO 49    ← (free)
```

The four SPI signals occupy four contiguous header pins with **no GND
in the middle**, so a 4-pin Dupont housing across GPIO 28–31 is safe
to use as long as the housing pin order matches the table above.

> ⚠️ **Why this layout?** ESP32-P4 SPI2's IO_MUX direct pins are
> **GPIO 28–31** (datasheet v1.1, Table 2-3: `SPI2_CS_PAD`,
> `SPI2_D_PAD`, `SPI2_CK_PAD`, `SPI2_Q_PAD` on the F2 alternate of
> these GPIOs). All four are on the Waveshare left header, so SPI2
> can bypass the GPIO matrix and stay valid up to ST7789's 80 MHz
> ceiling. The earlier assignment (GPIO 33/46/47/48 on the right
> header) routed through the GPIO matrix instead, and worse, sat
> across a header GND pad that turned multi-pin Dupont housings into
> guaranteed shorts. That assignment is deprecated.

> ⚠️ **Right-header GND gotcha (legacy note)**: `GPIO46 → GND → GPIO47`
> are three adjacent pins on the right header. A multi-pin Dupont
> housing planted across this region will short one signal to ground.
> Verified failure mode: SPI host writes look OK in logs, but the
> panel shows the unmodulated transflective backlight tint (uniform
> pale blue) because CS/SCK/MOSI/DC is shorted to GND. The left-header
> layout above avoids this entirely.

SPI bus is configured at `PK_LCD_SPI_HZ = 40 MHz` in
`firmware/main/display.h`. The ST7789 data sheet allows higher clocks
in suitable layouts, but any move toward 60-80 MHz should be verified
on the actual cable or PCB rather than documented as assumed margin.

### BNO085 IMU via I²C0 — GY-BN008X 10-pin breakout

Verified hardware: **`GY-BN008X`** (silkscreen on board, purple PCB,
through-hole 1×10 0.1" header). 10 pins total + one `BOOT` solder pad
on the side that we leave open. The breakout has an on-board 3.3 V
LDO and 4.7 kΩ I²C pull-ups, so 3.3 V from the ESP host is fine.

Shares the on-board I²C0 bus with the ES8311 codec (different
addresses; no conflict). The firmware uses SH-2 / SHTP Rotation
Vector reports at 100 Hz.

10-pin wiring (top → bottom on the GY-BN008X header):

| # | Breakout label  | Connect to             | Why                                                      |
|---|-----------------|------------------------|----------------------------------------------------------|
| 1 | `VCC`           | ESP **3V3** (right hdr)| On-board LDO accepts 3.3 V directly                      |
| 2 | `GND`           | ESP **GND** (any)      |                                                          |
| 3 | `SCL`           | ESP **GPIO 8** (left "SCL/8") | I²C0 clock; shared with codec                     |
| 4 | `SDA`           | ESP **GPIO 7** (left "SDA/7") | I²C0 data;  shared with codec                     |
| 5 | `AD0`           | **GND**                | I²C 7-bit address strap → `0x4A` (matches firmware)      |
| 6 | `CS`            | **3V3**                | SPI chip-select; in I²C mode hold high to disable SPI    |
| 7 | `INT`           | ESP **GPIO 20** (right "20") | Data-ready (active-low); currently polled, not used as IRQ |
| 8 | `RST`           | ESP **GPIO 21** (right "21") | Hard reset (active-low), pulsed at `pk_imu_init()`  |
| 9 | `PS1`           | **GND**                | Protocol-select bit 1                                    |
| 10| `PS0`           | **GND**                | Protocol-select bit 0  → `PS1=0, PS0=0` = I²C mode       |

The side `←BOOT` pad is the BNO08X `BOOTN#` (DFU entry). Leave open
for normal operation.

> ⚠️ `PS0`, `PS1`, `AD0` and `CS` must **not** be left floating. The
> GY-BN008X has no internal pull-up/down on these pins. Floating
> protocol-select bits cause BNO085 to land in UART or SPI mode at
> power-on and stop responding on I²C — the symptom in the firmware
> is `imu: enable_rotation_vector: ESP_ERR_INVALID_RESPONSE` followed
> by `PFD will run without attitude`.

#### Mounting orientation

BNO085 outputs an **absolute-orientation quaternion** referenced to
Earth's gravity + magnetic-north frame, so the sensor identifies
"which way is down" on its own — there's no requirement to power it
up in any specific orientation. But the chip body's X/Y/Z axes are
what the quaternion is expressed in. The breakout silkscreen (back
side) shows the body frame:

```
     X →
     ┌───┐
   Z •  │
     │  │
     └──┘
       Y ↓
```

For the PFD to interpret `roll / pitch / yaw` in the aerospace NED
convention used by `firmware/main/imu_task.c:188-209`
(`quat_to_euler` comment), the body axes must be glued to the
"aircraft" frame:

- **chip +X → device forward** (toward the nose, where the PFD is pointing)
- **chip +Y → device right** (right wing)
- **chip +Z → device down** (toward the belly when level)

If physical packaging forces a different mounting, two software fixes
are available — pick whichever fits the build:

1. **Constant rotation in firmware** — multiply the incoming
   quaternion by a fixed mounting quaternion before
   `quat_to_euler()`. Cheap, deterministic, zero runtime calibration.
2. **BNO085 internal reorient** — send SH-2 *Set Reorientation*
   (command `0x02` on the control channel). The chip stores the
   offset and applies it to every subsequent report. Survives a soft
   reset only; `Save DCD` is needed to persist across power-on.

The current firmware includes a fixed mounting quaternion in
`firmware/main/imu_task.h`. Keep the physical mounting and that
constant in agreement; changing one without the other produces
misleading roll, pitch, and heading.

#### Tare / cage button (BTN1, GPIO 26)

Driven by `button_task.c`. See §3.4 below.

### Tact buttons

Four active-low momentary tact switches, each between the named GPIO
and any GND pad. `button_task.c` enables the internal pull-up on all
four, so no external resistors are needed:

```
GPIO ────┬──── tact switch ──── GND
         │
         └─ INPUT_PULLUP (enabled by button_task gpio_config)
```

Pressed → GPIO reads `0`. Released → reads `1`. Polled at 50 Hz with
40 ms debounce.

| Button | GPIO | Header | Function                                |
|--------|------|--------|-----------------------------------------|
| **TARE** (BTN1) | 26 | right | tare / persist / factory reset (short / long / very-long) |
| **MODE** (BTN2) | **5** | **left** | short = cycle PFD → TRAFFIC → LIST → SETTINGS → ABOUT → DIAG; long = power off |
| **UP**   (BTN3) | 22 | right | target select / setting adjust / scroll up |
| **DOWN** (BTN4) | 23 | right | target select / setting adjust / scroll down |

TARE / UP / DOWN cluster on the **right header** so their wiring stays on
one side of the breadboard. MODE sits on **GPIO5 on the left header**
because GPIO0–15 are the only LP_IO pins on this chip — the LP_IO
domain is the only one still powered during deep sleep, so a press on
GPIO5 (low level) can wake the device. HP_IO pins (anything ≥16) are
fully unpowered while sleeping and can't act as wake sources. The
left/right split is acceptable: MODE doesn't share a gesture with the
other three buttons.

#### Press semantics

| Press kind                          | TARE                 | MODE              | UP        | DOWN      |
|-------------------------------------|----------------------|-------------------|-----------|-----------|
| **Short** (released within < 3 s)   | context-sensitive: Settings moves the cursor, ADS-B LIST binds own-ship, other modes snapshot current pose as zero (RAM only) | cycle PFD → TRAFFIC → LIST → SETTINGS → ABOUT → DIAG → PFD … | select/adjust/scroll up | select/adjust/scroll down |
| **Long**  (held ≥ 3 s)              | persist current tare to NVS (survives reboot) | enter deep sleep; next MODE press wakes / cold-boots | *(suppressed)* | *(suppressed)* |
| **Very-long** (held ≥ 10 s)         | **factory reset** — wipe NVS tare + BNO's persisted reorientation + DCD, reinit chip | — | — | — |
| **Combo** (UP + DOWN both held ≥ 5 s, second press landing within 1 s of first) | — | — | **BLE pairing window** (firmware records request; mobile UI handling not implemented yet) | — |

#### Calibration / heading-reset workflow

BNO085 magnetometer fusion is **continuously self-learning** — you
don't have to manually calibrate. But it only converges while the
device is **rotating** (figure-8 / multi-axis motion). If the device
sits still, `acc` stays at 0 forever. When `acc` finally reaches 2 or
3, the heading is usable for this device's situational-awareness UI.
The BNO085 saves Dynamic Calibration Data on its own schedule; a TARE
long-press persists only the ESP32-side software tare quaternion to
NVS.

If the heading is wrong even after a reboot, either a stale TARE
is saved in NVS or the BNO's persisted DCD is bad. Use **TARE
very-long-press** to factory-reset:

```
TARE very-long-press 10s
   → fires pk_imu_factory_reset()
   → wipes the persisted software tare (NVS key on ESP32)
   → clears BNO's persisted reorientation matrix + DCD
   → pulses RST and replays SH-2 init
   → fusion engine restarts with no prior calibration in flash

Now do figure-8 motion for ~15 s.
   → watch `imu: rpy = ... (acc=N)` log line
   → acc climbs 0 → 1 → 2 → 3 as mag fusion converges
   → BNO persists DCD into its own internal flash automatically
     as accuracy improves — no user action needed

When acc reaches 2 or 3, hold the device in the desired "zero"
orientation, then:
   TARE short-press           → snapshot this pose as the new zero
   TARE long-press 3s         → save that zero pose to ESP32 NVS
                                (so the next boot wakes up zeroed)
```

**Why UP / DOWN don't fire single-button long press**: if they did,
holding UP alone for 3 s would emit `UP_LONG`, then a UP+DOWN combo
landing 2 s later would arrive *after* the `UP_LONG` event — two
distinct gestures for what feels like one hold. Suppressing single-key
long-press on UP and DOWN resolves this cleanly while leaving TARE /
MODE long presses untouched.

**Why only TARE emits very-long-press (≥10 s)**: factory reset is a
destructive action that should require a sustained, unambiguous hold
the user clearly meant to make. Putting it on the same button as the
much more frequent "live tare" + "persist tare" gestures (TARE short
/ long) keeps related-by-meaning operations under one finger, with
the danger gradient (3 s long = recoverable; 10 s very-long = wipe)
matching the press effort. MODE's long-press slot is used for
soft power off / deep sleep instead — see
`docs/superpowers/plans/2026-05-21-power-button.md`.

#### Firmware structure

- `firmware/main/button_task.c` — GPIO polling + per-button FSM (4
  states: RELEASED / PRESSING / HELD_SHORT / HELD_LONG) + combo
  detector pass. Reports `(id, event)` pairs to the registered
  callback.
- `firmware/main/main.c::on_button_event()` — single dispatch point.
  Routes TARE SHORT by current UI mode (Settings language toggle,
  ADS-B LIST own-ship bind, otherwise IMU tare), TARE LONG / VERY_LONG
  to `pk_imu_tare_persist()` / `pk_imu_factory_reset()`,
  MODE SHORT to `pk_ui_toggle_mode()`, MODE LONG to
  `pk_power_enter_sleep()`, UP/DOWN to list/About scrolling, and the
  combo to the BLE-pairing request log path.
- `firmware/main/ui_state.c` — holds current `pk_ui_mode_t` and the
  list cursor; mutex-protected for concurrent reads from
  `pfd_task` (render) and `button_task` (input).
- `firmware/main/pfd.c::pfd_task` — once per frame, branches on
  `pk_ui_get_mode()`: PFD render path (horizon/ladder/tape/...)
  or `pk_adsb_list_render()` for the ADS-B list view.

### BMP388 barometric pressure sensor via I²C0 (carrier board)

**Status**: Driver implemented — `baro_task.c`, polling ~10 Hz over I²C0.
Provides barometric altitude + vertical speed (QNH-adjustable); results
written to `g_baro_state` and rendered on the PFD right-side panel and
DIAG view.

Shares the on-board I²C0 bus with the ES8311 codec and BNO085 IMU
(distinct addresses, no conflict).

| BMP388 pin | Net      | ESP32-P4 pin | Notes                              |
|------------|----------|--------------|------------------------------------|
| VCC        | +3V3     | 3V3          |                                    |
| GND        | GND      | GND          |                                    |
| SCL        | I²C0 SCL | GPIO8        | shared bus                         |
| SDA        | I²C0 SDA | GPIO7        | shared bus                         |
| SDO        | GND      | GND          | I²C address strap → `0x76`         |
| CSB        | +3V3     | 3V3          | tie high to select I²C mode        |
| INT        | BARO_INT | **GPIO27**   | data-ready IRQ (optional)          |

### GPS receiver (GT-U8 / ATGM336H) via UART + PPS (carrier board)

**Status**: Driver implemented — `gps_task.c`, UART1 NMEA parsing.
Provides own-ship position / velocity / course and a 1 PPS pulse for
precise time discipline.

GOOUUU **GT-U8** module (AT6558 / ATGM336H GNSS core). Wide-range
3.3–5 V supply via an on-board LDO; the GNSS core and its UART run at
3.3 V, so the UART lines connect **directly** to the ESP32-P4 with no
level shifting.

| GPS pin | Net      | ESP32-P4 pin | Notes                                                |
|---------|----------|--------------|------------------------------------------------------|
| V (VCC) | +3V3     | 3V3          | silkscreen reads "5v", but 3.3 V is fine on this wide-range module — do **not** assume 5 V is required |
| G (GND) | GND      | GND          |                                                      |
| T (TXD) | GPS_TX   | **GPIO33**   | GPS → **P4 UART RX** (crossed)                        |
| R (RXD) | GPS_RX   | **GPIO32**   | **P4 UART TX** → GPS (crossed)                        |
| P (PPS) | GPS_PPS  | **GPIO46**   | 1 Hz pulse-per-second; rising edge marks the UTC second |

> UART is crossed the standard way (GPS TX → P4 RX, GPS RX → P4 TX).
> `GPIO46/47` bracket a header GND on the dev-board headers; on the
> carrier board these are fixed traces so the gotcha doesn't apply.

---

## 4. Freely-assignable GPIOs

After accounting for everything above, the following P4 GPIOs are
free for new application use:

```
GPIO47  GPIO48                                    (right header, lower half)
GPIO49  GPIO51  GPIO52                            (left header)
```

That's **5** GPIOs with no caveats. The carrier board consumed four of
the former free pool: GPIO27 → BMP388 INT, GPIO32/33 → GPS UART,
GPIO46 → GPS PPS (see §3). Before the carrier board there were 9.

> Note: `GPIO33 / GPIO46 / GPIO47 / GPIO48` were used by the LCD in
> an earlier bring-up build, then freed when the LCD moved to the SPI2
> IO_MUX direct pins on the left header (GPIO 28–31, see §3). On the
> carrier board GPIO33 (GPS TX) and GPIO46 (GPS PPS) are used again;
> only GPIO47/48 remain free here. GPIO46 and GPIO47 still bracket a
> header GND, so prefer individual jumpers over multi-pin housings.

**Available but with caveats** — usable if you accept the trade-off:

| GPIO    | Header     | Caveat                                                                |
|---------|------------|-----------------------------------------------------------------------|
| GPIO2   | left       | JTAG MTCK default — using disables JTAG over the USB Serial/JTAG port |
| GPIO3   | left       | JTAG MTDI default — same                                              |
| GPIO4   | left       | JTAG MTMS default — same                                              |
| GPIO24  | left "DM"  | USB Serial/JTAG D− default (USB1P1_N0) — using disables on-chip USB JTAG |
| GPIO25  | left "DP"  | USB Serial/JTAG D+ default (USB1P1_P0) — same                         |

(GPIO5 was previously in this table as JTAG MTDO; it now hosts BTN2 — MODE, see §3.4.)

In practice JTAG-via-USB-Serial isn't used by this project (we have
the CH343P UART console and SWD via the C6 debug header), so
GPIO2–4 and GPIO24/25 are de-facto available. But if a future
contributor ever wants USB JTAG, they'd have to free these up first.

**Not in the free pool**:
- `GPIO0` (BOOT strap; not broken out anyway)
- `GPIO22 / GPIO23 / GPIO26 / GPIO5` (BTN3 / BTN4 / BTN1 / BTN2 —
  see "Tact buttons" above; BTN2 = MODE on GPIO5 because deep-sleep
  wake needs an LP_IO pin)
- All on-board-only GPIOs (`6`, `9–19`, `34–45`, `53`, `54`) — not on
  user headers.

---

## 5. Pin-assignment policy

When introducing new peripherals in later phases:

1. **Pick from the "freely-assignable" pool above** first; only
   reach into the "available with caveats" group if you've exhausted
   the no-caveat pool *and* JTAG-via-USB-Serial isn't a future need.
2. **Update this file *before* writing the driver.** Add a row to
   the relevant table, then have the driver `#include` the number
   from a shared header — never hard-code GPIO numbers in `.c` files
   that don't also live next to a `*_PIN` define.
3. **Sanity-check against `firmware/sdkconfig.defaults`.** Some IDF
   defaults reserve GPIOs for JTAG, console UART, hosted SDIO, etc.
4. **Avoid the right-header GND between GPIO46 and GPIO47** when
   designing connectors — see the LCD wiring gotcha above.
