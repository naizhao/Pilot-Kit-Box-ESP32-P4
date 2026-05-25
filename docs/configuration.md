# Configuration Reference

Chinese version: [`configuration-zh_CN.md`](configuration-zh_CN.md)

This document lists the Pilot Kit Box firmware configuration options that users or maintainers are likely to change. If you only want a default build, follow [`BUILD.md`](BUILD.md) and keep `firmware/sdkconfig.defaults` unchanged.

Open the configuration UI from the firmware directory:

```bash
cd firmware
./build.sh menuconfig
```

To make a configuration change part of the project defaults, add the `CONFIG_*` line to `firmware/sdkconfig.defaults`. The generated `sdkconfig` is local and should remain machine-specific.

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
factory,   app,  factory,  0x10000,   0x400000
storage,   data, spiffs,   ,          0x1000000
```

The `storage` partition is mounted through LittleFS and currently stores rotating ADS-B raw ts-line logs. Space remains for future OTA A/B partitions.

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

Do not change reset polarity to active-low on this board. Waveshare's board-level reset path makes GPIO54 active-high from the P4 software point of view. Wrong polarity causes SDIO CMD5 failures.

For low internal-RAM pressure, SDIO queues are reduced from upstream defaults:

```text
CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE=8
CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE=8
```

## Own-Ship Binding

PFD ALT / GS / VS can be sourced from a known ADS-B transponder rather than only from the IMU.

| Option | Default | Meaning |
|---|---|---|
| `CONFIG_PK_OWN_ICAO` | `0x000000` | 24-bit ICAO address. Non-zero values make PFD use matching ADS-B altitude, ground speed, and vertical speed. |
| `CONFIG_PK_OWN_STALE_AGE_MS` | `5000` | How long own-ship ADS-B data remains fresh before PFD readouts return to `---`. |

Runtime alternative: in ADS-B LIST, highlight an aircraft and short-press TARE. That RAM-only binding is cleared on reboot.

## Storage / LittleFS

Defined in `firmware/main/record_sink_file.c`:

| Constant | Default | Meaning |
|---|---|---|
| `FILE_QUEUE_DEPTH` | `256` | Queue depth between DSP and file writer task. |
| `FILE_ROTATE_BYTES` | `1 * 1024 * 1024` | Each log file rotates at 1 MiB. |
| `FILE_KEEP_COUNT` | `12` | Keep 12 log files, about 12 MiB total. |

The file sink writes lines shaped as:

```text
<epoch_ms> *<MODE_S_HEX>;
```

This matches the raw ts-line format used by the BLE Raw characteristic and offline post-processing tools.

## Display / ST7789

Defined in `firmware/main/display.h`:

```c
#define PK_LCD_SPI_HOST       SPI2_HOST
#define PK_LCD_PIN_SCLK       30
#define PK_LCD_PIN_MOSI       29
#define PK_LCD_PIN_MISO       -1
#define PK_LCD_PIN_CS         28
#define PK_LCD_PIN_DC         31
#define PK_LCD_PIN_RST        -1
#define PK_LCD_PIN_BL         50
#define PK_LCD_SPI_HZ         (40 * 1000 * 1000)
```

Current verified wiring uses the left header:

- CS: GPIO28
- MOSI: GPIO29
- SCK: GPIO30
- DC: GPIO31
- BL: GPIO50

GPIO28/29/30 are SPI2 IO_MUX direct pins, which avoids the older right-header GPIO-matrix wiring and the GPIO46 / GND / GPIO47 shorting hazard.

## IMU / BNO085

Defined in `firmware/main/imu_task.c`:

| Constant | Default | Meaning |
|---|---|---|
| `IMU_I2C_PORT` | `I2C_NUM_0` | Shared I2C0 bus. |
| `IMU_I2C_SDA` | `7` | SDA. |
| `IMU_I2C_SCL` | `8` | SCL. |
| `IMU_I2C_HZ` | `400000` | I2C fast mode. |
| `IMU_I2C_ADDR` | `0x4A` | BNO085 default address when AD0 is grounded. |
| `IMU_PIN_INT` | `20` | Data-ready pin, currently polled rather than IRQ-driven. |
| `IMU_PIN_RST` | `21` | Active-low hard reset pin. |

The firmware enables Rotation Vector reports at 100 Hz (`report_interval_us = 10000`).

Euler convention:

- Roll: X-axis rotation, right-wing-down positive.
- Pitch: Y-axis rotation, nose-up positive.
- Yaw: Z-axis rotation, 0..360 degrees clockwise when viewed from above.

## Buttons And Power

Button GPIOs:

| Button | GPIO | Short press | Long press | Very-long press |
|---|---:|---|---|---|
| TARE | 26 | Context-sensitive: IMU tare, Settings language toggle, or ADS-B LIST own-ship binding | Persist current IMU tare to NVS | IMU factory reset |
| MODE | 5 | Cycle PFD -> ADS-B LIST -> SETTINGS -> ABOUT | Enter ESP32-P4 deep sleep; next MODE press wakes | None |
| UP | 22 | Scroll up in list/About | Suppressed for combo detection | None |
| DOWN | 23 | Scroll down in list/About | Suppressed for combo detection | None |

Very-long TARE press (10 seconds) performs IMU factory reset: clears NVS tare, clears BNO persisted DCD / legacy reorientation state, and reinitialises the IMU.

UP + DOWN held together for 5 seconds is reserved for a BLE pairing-window flow; current firmware records the request while mobile UI handling remains unimplemented.

## Logging

Common ESP_LOG tags:

| Tag | Module |
|---|---|
| `pilot_kit` | boot sequence and app-level init |
| `sdr` | RTL-SDR USB control and re-init |
| `dsp` | IQ decode and 1 Hz dashboard |
| `adsb` | decoded ADS-B message logs |
| `rec_file` | LittleFS writer |
| `ble_gatt` | NimBLE host, GATT, GDL90 emitter |
| `display` | ST7789 panel |
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
| `rec_file` | 0 | 3 | 4 KiB | LittleFS writer |
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
