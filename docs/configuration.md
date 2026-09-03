# Configuration Reference

Chinese version: [`configuration-zh_CN.md`](configuration-zh_CN.md)

This document lists the Pilot Kit Box firmware configuration options that users or maintainers are likely to change. If you only want a default build, follow [`BUILD.md`](BUILD.md) and keep `firmware/sdkconfig.defaults` unchanged.

Open the configuration UI from the firmware directory:

```bash
cd firmware
./build.sh menuconfig
```

To make a configuration change part of the project defaults, add the `CONFIG_*` line to `firmware/sdkconfig.defaults`. The generated `sdkconfig` is local and should remain machine-specific.

## Expansion Board Profile (v3 / v4)

| Option | Default | Meaning |
|---|---|---|
| `CONFIG_PK_BOARD_PROFILE_V4` | `y` | Build for the **v4 board family** (current default). |
| `CONFIG_PK_BOARD_PROFILE_V3` | `n` | Build for the **v3 board family**. |

The choice is the board **family**, not a revision. `v3` / `v4` (and a future
`v5`) name the family; `V3.9` / `V4.3` are merely their current revisions. A new
revision does not need a new profile — a V4.4 board still runs the `v4` image.
The angles below were measured on V3.9 and V4.3, and
`hardware/test_firmware_board_profile_contract.py` compares the firmware table
against the live `kicad_pcb` files, so a future revision that rotates a sensor
turns red instead of shipping silently.

These are a mutually exclusive Kconfig `choice`; exactly one must be selected. Selecting
neither does not fall back to a default — the build stops with an `#error`.

This is unrelated to the ESP32-P4 silicon revision in the next section. `CONFIG_ESP32P4_*`
describes the main SoC stepping; this option describes the **expansion board** PCB revision.
Getting one wrong gives no hint about the other.

### Why it cannot be probed at runtime

The two families place their sensors at different angles (measured on their current revisions V3.9 and V4.3):

| Part | v3 | v4 |
|---|---:|---:|
| U4 BNO085 (IMU) | 0 deg | **+90 deg** |
| U5 BMP388 (baro) | 0 deg | -90 deg |
| U6 QMC5883P (magnetometer) | 0 deg | -90 deg |
| U7 ATGM336H (GNSS) | 0 deg | 0 deg |

U4 and U5/U6 rotate in **opposite** directions, so this is not a whole-board 90-degree
rotation; the two attitude-bearing parts each carry their own board-to-body transform.

Picking the wrong profile fails badly but quietly: the PFD still shows an attitude and still
tracks motion, it is just rolled by 90 degrees. That is hard to notice on a bench and easy to
notice in flight, which is why the firmware refuses to guess.

The only board-varying I2C device is the SY6970 PMIC, and it marks the powered/unpowered
**variant**, not the board revision — V3 and an unpowered V4 both fail to ACK it.
`hardware/test_firmware_board_profile_contract.py` fails if `SY6970`, `0x6A` or `PMIC`
appears in the `pk_board` sources.

### Switching to v3

Interactive:

```bash
cd firmware
./build.sh menuconfig
#   -> Pilot Kit Box
#     -> Expansion board profile  --->
#        ( ) v3 (expansion-board-v3, validated on V3.9)
#        (X) v4 (expansion-board-v4, validated on V4.3)
./build.sh build
```

Non-interactive (scripts / CI). `sdkconfig` is gitignored, so this only affects your checkout:

```bash
cd firmware
sed -i '' 's/^# CONFIG_PK_BOARD_PROFILE_V3 is not set$/CONFIG_PK_BOARD_PROFILE_V3=y/' sdkconfig
sed -i '' 's/^CONFIG_PK_BOARD_PROFILE_V4=y$/# CONFIG_PK_BOARD_PROFILE_V4 is not set/' sdkconfig
./build.sh build
```

Use `sed -i` without the empty argument on Linux. Reverse both substitutions to go back to v4.

### Confirming which profile was built

Before flashing, exactly one of these lines exists:

```bash
grep PK_BOARD_PROFILE firmware/build/config/sdkconfig.h
# v3 -> #define CONFIG_PK_BOARD_PROFILE_V3 1
# v4 -> #define CONFIG_PK_BOARD_PROFILE_V4 1
```

After flashing, the boot log prints the profile and its mounting quaternion:

```
I (xxxx) imu: board profile v3: q_body_fix = 0.7071068 0.0000000 0.7071068 0.0000000
I (xxxx) imu: board profile v4: q_body_fix = 0.5000000 0.5000000 0.5000000 -0.5000000
```

The two values differ, so this single line identifies the image. Check it when swapping boards.

### Symmetric two-board builds (release / CI)

To produce both images without touching `sdkconfig`, use separate build
directories. Sharing one directory leaves stale objects from the previous
profile behind, which shows up as "I changed it but nothing happened":

```bash
cd firmware
./build.sh -B build_v3 \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.v3" \
  -DSDKCONFIG=build_v3/sdkconfig build

./build.sh -B build_v4 \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.v4" \
  -DSDKCONFIG=build_v4/sdkconfig build
```

Artifacts land in `build_v3/pilot_kit_box.bin` and
`build_v4/pilot_kit_box.bin`. Replace `build` with `-p <port> flash` to flash.

`sdkconfig.defaults` deliberately does not pin a profile; otherwise neither
fragment could override it. `hardware/test_firmware_board_profile_contract.py`
enforces that.

### The build prints it too

At configure time — the first build, and after every `menuconfig` or
`sdkconfig` change — CMake prints:

```
-- Pilot Kit: expansion board profile = v4 (expansion-board-v4)  ...
```

Together with the boot log line, the profile is visible at both moments a
human is present, so nothing depends on remembering that it was selected.

To change the project-wide default, add the `CONFIG_PK_BOARD_PROFILE_*` line to
`firmware/sdkconfig.defaults`. It is absent today, so the Kconfig `default` (v4) applies.

## Hardware Target

| Option | Default | Meaning |
|---|---|---|
| `CONFIG_IDF_TARGET` | `"esp32p4"` | Target chip. Do not change for this board. |
| `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` | `y` | Enables support for ESP32-P4 v0.x / v1.x silicon. Current Waveshare ESP32-P4-WIFI6 boards are commonly v1.3. |
| `CONFIG_ESP32P4_REV_MIN_100` | `y` | Minimum supported silicon revision is v1.0. |

ESP32-P4 v1.x and v3.x are not binary-compatible. If a future Waveshare board ships v3.x silicon, disable `SELECTS_REV_LESS_V3`, select the correct `REV_MIN_300` or `REV_MIN_301`, rebuild, and reflash.

## Flash And Partitions

| Option | Default | Meaning |
|---|---|---|
| `CONFIG_ESPTOOLPY_FLASHSIZE_32MB` | `y` | Waveshare board has 32 MB Nor Flash. |
| `CONFIG_ESPTOOLPY_FLASHSIZE` | `"32MB"` | Flash-size string. |
| `CONFIG_PARTITION_TABLE_CUSTOM` | `y` | Use project partition table. |
| `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` | `"partitions.csv"` | Partition table path relative to `firmware/`. |

Current `partitions.csv` layout:

```text
nvs,       data, nvs,      0x9000,    0x6000
phy_init,  data, phy,      0xf000,    0x1000
factory,   app,  factory,  0x10000,   0xc00000
storage,   data, spiffs,   ,          0xa00000
```

The `storage` partition is mounted through LittleFS and currently stores rotating ADS-B raw ts-line logs. This layout uses about 22.5 MiB of the 32 MiB flash and reserves the remaining ~9.5 MiB for future expansion.

The 12 MiB `factory` partition is now mostly headroom: the 8 MiB aircraft database moved to the microSD card, so the app image is about 2.9 MB and occupies roughly 23% of the partition.

## USB Host And RTL-SDR

| Option / Constant | Default | Meaning |
|---|---|---|
| `CONFIG_USB_HOST_HUBS_SUPPORTED` | `y` | Enables USB hub support; some RTL-SDR dongles expose internal hubs. |
| `CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE` | `512` | Allows full descriptor reads from RTL2832U devices. |
| `PK_RTLSDR_FREQ_HZ` | `1090000000UL` | ADS-B / Mode-S center frequency. Defined in `firmware/main/pilot_kit.h`. |
| `PK_RTLSDR_SAMPLERATE_HZ` | `2000000UL` | 2 MSPS sample rate for ADS-B. |
| `PK_IQ_RINGBUF_SIZE_BYTES` | `512 * 1024` | IQ buffering headroom between USB producer and DSP consumer. |

The current recommended low-cost SDR dongle uses the **FC0013** tuner. The firmware queries the tuner gain table at runtime and selects maximum manual gain, so it is not hard-coded to one tuner gain value.

## BLE / ESP-Hosted

BLE uses the on-board ESP32-C6 as a controller over ESP-Hosted SDIO. The ESP32-P4 itself has no native Bluetooth controller.

| Option | Default | Meaning |
|---|---|---|
| `CONFIG_PK_BLE_ENABLED` | `y` | Project option. When enabled, `app_main()` initialises the BLE GATT server. Requires the C6 hosted slave firmware. |
| `CONFIG_BT_ENABLED` | `y` | Enables the Bluetooth host stack. |
| `CONFIG_BT_CONTROLLER_DISABLED` | `y` | Required because the controller is on the C6, not on the P4. |
| `CONFIG_BT_NIMBLE_ENABLED` | `y` | Uses NimBLE host. |
| `CONFIG_BT_NIMBLE_TRANSPORT_UART` | `n` | Must be off; HCI runs through ESP-Hosted VHCI, not UART H4. |
| `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL` | `y` | Pilot Kit Box acts as a BLE peripheral. |
| `CONFIG_BT_NIMBLE_ROLE_CENTRAL` | `y` | Required so firmware can read iOS Current Time Service for clock sync. |
| `CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU` | `256` | Large enough for one GDL90 Traffic Report notification. |

Waveshare C6 SDIO wiring:

```text
CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE=y
CONFIG_ESP_HOSTED_SDIO_SLOT_1=y
CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS=y
CONFIG_ESP_HOSTED_SDIO_PIN_CLK=18
CONFIG_ESP_HOSTED_SDIO_PIN_CMD=19
CONFIG_ESP_HOSTED_SDIO_PIN_D0=14
CONFIG_ESP_HOSTED_SDIO_PIN_D1=15
CONFIG_ESP_HOSTED_SDIO_PIN_D2=16
CONFIG_ESP_HOSTED_SDIO_PIN_D3=17
CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE=54
CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y
CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE=y
```

Do not change the verified ESP-Hosted reset option to active-low on this
project. Rev1.2 connects GPIO54 directly to C6 EN through R34, 0 Ω; there is
no board inverter. The `RESET_ACTIVE_HIGH` setting is verified driver
behavior, and the wrong option causes SDIO CMD5 failures.

For low internal-RAM pressure, SDIO queues are reduced from upstream defaults:

```text
CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE=8
CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE=8
```

## Own-Ship Binding

PFD and traffic geometry can use a known ADS-B transponder as the
manual own-ship source. GPS is the automatic fallback when no manual
binding is active.

| Option | Default | Meaning |
|---|---|---|
| `CONFIG_PK_OWN_ICAO` | `0x000000` | 24-bit ICAO address. Non-zero values make PFD use matching ADS-B altitude, ground speed, and vertical speed. |
| `CONFIG_PK_OWN_STALE_AGE_MS` | `5000` | How long own-ship ADS-B data remains fresh before PFD readouts return to `---`. |

The current 4.3-inch touch UI does not expose the former TARE-button runtime
binding gesture. Use the build-time ICAO option or GPS own-position.

## Demo Mode (runtime switch, NVS)

Setup page, second-from-last row. There is **no build-time Kconfig option** —
it is pure runtime state kept in NVS:

| Namespace | Key | Type | Default |
|---|---|---|---|
| `pk_demo` | `demo_on` | `u8` | `0` (off) |

When enabled, the four data-source getters below return synthetic values from
`demo_data.c` regardless of whether the hardware is present. The takeover
happens in the data layer, not in the pages, so no page needs to know demo
mode exists:

| Entry point | File |
|---|---|
| `pk_imu_sample_get()` | `imu_task.c` |
| `pk_gps_get()` | `gps_task.c` (this also covers `pk_own_ship_resolve()`, which falls back to GPS) |
| `pk_baro_get()` | `baro_task.c` |
| `aircraft_state_snapshot()` / `aircraft_state_get_own()` | `aircraft_state.c` |

Synthetic targets never enter the real fusion table, so they never reach
`record_sink` (on-disk ts logs still contain only real frames) and never
pollute CPR decoding. The GDL90 path is suppressed explicitly in the
`ble_gatt.c` emitter task — see [`ble_protocol.md`](ble_protocol.md) §6.5.

Safety constraints: off by default, only enabled explicitly from the Setup
page, and the annunciator cannot be turned off while it is on (red DEMO
badge in the top bar, red frame around the screen, red banner on the boot
splash). There is deliberately **no auto-exit timeout** — a mode changing
back without the user noticing is more dangerous than staying in demo mode.

## Storage / LittleFS / MicroSD

At boot, the file sink reads its NVS setting. It writes `/sdcard` when
MicroSD is selected and mounted; otherwise it writes the `/storage`
LittleFS partition. A requested but absent card falls back to Flash.
The SETTINGS `LOG` row takes effect after reboot. `FORMAT SD` requires
a second press within five seconds and refuses while logging to the card.

Defined in `firmware/main/record_sink_file.c`:

| Constant | Default | Meaning |
|---|---|---|
| `FILE_QUEUE_DEPTH` | `256` | Queue depth between DSP and file writer task. |
| `FILE_ROTATE_BYTES` | `1 * 1024 * 1024` | Flash files rotate at 1 MiB. |
| `FILE_KEEP_COUNT` | `12` | File-count ceiling; actual retention is capped by the 10 MiB partition. |
| `SD_ROTATE_BYTES` | `16 * 1024 * 1024` | MicroSD files rotate at 16 MiB. |
| `SD_KEEP_COUNT` | `64` | Keep about 1 GiB on MicroSD. |

The file sink writes lines shaped as:

```text
<epoch_ms> *<MODE_S_HEX>;
```

This matches the raw ts-line format used by the BLE Raw characteristic and offline post-processing tools.

MicroSD uses SDMMC Slot 0 in 4-bit mode:

```text
CLK=GPIO43  CMD=GPIO44  D0..D3=GPIO39..42
```

There is no card-detect GPIO. `sd_detect` retries an absent card every
three seconds and checks a mounted card every two seconds. The writer
does not migrate backends during one boot; after card removal, reinsert
and reboot or select Flash and reboot.

## Display / ST7701 MIPI-DSI

Defined in `firmware/main/display.h`:

```c
#define PK_DISPLAY_W                     800
#define PK_DISPLAY_H                     480
#define PK_LCD_PIN_RST                    27
#define PK_LCD_PIN_BL                     26
#define PK_LCD_PIN_BL_EN                  33
#define PK_LCD_DSI_LANE_COUNT             2
#define PK_LCD_DSI_LANE_BIT_RATE_MBPS     500
#define PK_LCD_DPI_CLOCK_MHZ              30
```

The panel scans its native 480x800 framebuffer continuously. PPA rotates
the logical 800x480 RGB565-swapped framebuffer 270 degrees CCW
(equivalent to 90 degrees CW), performs
the byte swap, and presents it through two DPI framebuffers at VSYNC.
GPIO26 uses inverted LEDC PWM because it injects the AP3032 feedback node.

## IMU / BNO085

Defined in `firmware/main/imu_task.c`:

| Constant | Default | Meaning |
|---|---|---|
| `IMU_I2C_PORT` | `I2C_NUM_0` | Shared I2C0 bus. |
| `IMU_I2C_SDA` | `7` | SDA. |
| `IMU_I2C_SCL` | `8` | SCL. |
| `IMU_I2C_HZ` | `400000` | I2C fast mode. |
| `IMU_I2C_ADDR` | `0x4A` | BNO085 default address when AD0 is grounded. |
| `IMU_PIN_INT` | `34` | J3 pin 28 = GPIO34 (JLC PCB net IMU_INT). Currently polled; INT not enabled. |
| `IMU_PIN_RST` | `28` | Active-low hard reset pin (J3 pin 16). |

The firmware enables Rotation Vector reports at 100 Hz
(`report_interval_us = 10000`) and polls the device. BNO085 INT is not
wired to GPIO34 (J3 pin 28) but polled; reset is GPIO28. The current driver supports address `0x4A`, so
AD0 must be grounded.

Euler convention:

- Roll: X-axis rotation, right-wing-down positive.
- Pitch: Y-axis rotation, nose-up positive.
- Yaw: Z-axis rotation, 0..360 degrees clockwise when viewed from above.

### Mounting transform q_body_fix

No longer a hard-coded constant in `imu_task.h`. It is selected by expansion
board profile — see the first section of this document:

| Profile | `q_body_fix` (w, x, y, z) | Chip axes to body axes |
|---|---|---|
| v3 (U4 at 0 deg) | `0.7071068, 0, 0.7071068, 0` | +X down, +Y right, +Z aft |
| v4 (U4 at +90 deg) | `0.5, 0.5, 0.5, -0.5` | +X right, +Y up, +Z aft |

Body frame is aerospace NED (+X forward, +Y right, +Z down). Both assume the
enclosure standing upright with the display facing the pilot and the J1 header
edge downward. The values are derived in `firmware/main/pk_board.c` from three
pieces of evidence — datasheet axes, footprint rotation measured on each PCB,
and the enclosure orientation — not hand-tuned.

Do not edit these values by hand. If the physical installation genuinely
changes, edit the corresponding evidence in `pk_board.c` and run
`firmware/test/test_pk_board_mount.c`, which derives its expectations
independently from the physical axis definitions and fails on a wrong value.
Touch leveling cages the horizon only; it is not a substitute for an axis
transform.

## Touch And Power

Rev1.2 has RESET, BOOT and POWER buttons, but no MODE/TARE/UP/DOWN
application buttons. The legacy `button_task.c` remains in source for the
former 2.4-inch carrier but is excluded from `firmware/main/CMakeLists.txt`,
so it is not compiled at all — its hard-coded GPIO26/GPIO23 are LCD_BL_PWM
and GT911 TP_RST on Rev1.2.

The current UI uses GT911 touch:

- tap the FAB to open the full-screen navigation grid (page 1: PFD, traffic,
  map, list, search, log, tools; page 2: diag, settings, about; log and
  tools are greyed placeholders);
- hold and drag the FAB to move it; its position is stored in NVS;
- hold **Level** in the grid's action bar for one second to run
  `pk_imu_tare_persist()`;
- use the back FAB, title-bar back control or right swipe on detail pages.

Key3 POWER powers on with a short press and powers off after an approximately
two-second hold. Key1 RESET restarts the P4; Key2 BOOT is for download mode.
The current touch UI has no factory-reset/DCD-wipe gesture.

## Logging

Common ESP_LOG tags:

| Tag | Module |
|---|---|
| `pilot_kit` | boot sequence and app-level init |
| `sdr` | RTL-SDR USB control and re-init |
| `dsp` | IQ decode and 1 Hz dashboard |
| `adsb` | decoded ADS-B message logs |
| `rec_file` | LittleFS / MicroSD writer |
| `pk_sd` | MicroSD mount, removal, and format |
| `gps` | GT-U8 NMEA/RMC and satellite diagnostics; no PPS GPIO handling |
| `baro` | BMP388 pressure, altitude, and vertical speed |
| `ble_gatt` | NimBLE host, GATT, GDL90 emitter |
| `display` | ST7701 MIPI-DSI + PPA rotation |
| `imu` | BNO085 driver |
| `pfd` | renderer |

## FreeRTOS Tasks

| Task | CPU | Priority | Stack | Owner |
|---|---:|---:|---:|---|
| `usb_host_lib` | 0 | 5 | 4 KiB | USB host lifecycle |
| `sdr` | 1 | 6 | 8 KiB | RTL-SDR control + async IQ producer |
| `dsp` | 1 | 4 | 4 KiB | dump1090-derived decode |
| `imu` | 0 | 5 | 4 KiB | BNO085 polling |
| `pfd` | 0 | 4 | 6 KiB | LCD UI renderer |
| `rec_file` | 0 | 3 | 4 KiB | LittleFS / MicroSD writer |
| `gps` | 0 | 4 | 4 KiB | GT-U8 NMEA/RMC; optional PPS wiring is not consumed |
| `baro` | 0 | 4 | 4 KiB | BMP388 polling |
| `sd_detect` | 0 | 2 | 4 KiB | MicroSD insertion/removal probe |
| `nimble_host` | 0 | 4 | 4 KiB | NimBLE host |
| `ble_emit` | 0 | 3 | 6 KiB | GDL90 / Raw BLE notifications |

## Build Optimisation

Performance build is the default:

```text
CONFIG_COMPILER_OPTIMIZATION_PERF=y
```

If future OTA A/B partitions require smaller binaries, test size optimisation:

```text
CONFIG_COMPILER_OPTIMIZATION_SIZE=y
```

Disabling Bluetooth can save flash space on builds that never use BLE:

```text
CONFIG_BT_ENABLED=n
```

Use ccache for faster local rebuilds:

```bash
export IDF_CCACHE_ENABLE=1
./build.sh build
```

## Adding A New Project Option

Add new user-facing options to `firmware/main/Kconfig.projbuild`, then reference them through `sdkconfig.h`.

Example:

```kconfig
menu "Pilot Kit Box"
    config PK_FOO_BAR
        int "Foo bar limit"
        default 42
        help
            Explain what the option changes.
endmenu
```

Then add the intended default to `firmware/sdkconfig.defaults`.
