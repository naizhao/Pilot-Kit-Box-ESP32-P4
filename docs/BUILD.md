# Build And Flash Guide

Chinese version: [`BUILD-zh_CN.md`](BUILD-zh_CN.md)

This guide is for developers bringing up the Pilot Kit Box ESP32-P4 firmware from a clean machine. It covers ESP-IDF installation, repository clone, optional one-time ESP32-C6 slave flashing for BLE, ESP32-P4 build/flash, expected boot logs, and troubleshooting.

Experienced ESP-IDF users can skip to [Build](#build) after checking the prerequisites.

## Prerequisites

### Required Hardware

| Item | Notes |
|---|---|
| **Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3** | Integrated P4NRW32 + C6, 4.3-inch ST7701 DSI display, GT911 touch, 32 MB flash and 32 MB PSRAM. |
| **USB-C data cable** | Used for power, flashing, and serial monitoring. Charge-only cables will not work. |
| **macOS / Linux / Windows host** | Any host that can run ESP-IDF. Commands below use macOS/Linux paths; Windows uses an ESP-IDF PowerShell and `COMx` ports. |

### Optional Hardware

| Item | Purpose |
|---|---|
| RTL-SDR FC0013 USB dongle | 1090 MHz ADS-B reception. FC0013 is currently recommended because it keeps BOM cost low. |
| USB-C OTG adapter or powered USB hub | Connects the H2 native USB HS port to a USB-A RTL-SDR dongle. |
| BNO085 / GY-BN008X IMU module | Attitude fusion for the PFD. |
| USB-UART adapter | Required once per fresh board to flash the ESP32-C6 hosted slave firmware for BLE. |

### Software

- Git 2.20+
- Python 3.10+
- CMake 3.16+
- Ninja
- ESP-IDF v6.0.1

## Install ESP-IDF v6.0.1

Use ESP-IDF v6.0.x. Older v5.x releases do not support the required ESP32-P4 component set, and newer major versions have not been validated for this firmware.

### macOS / Linux

The recommended path is Espressif Installation Manager:

```bash
curl -L https://dl.espressif.com/dl/eim/eim-installer.sh | bash
```

Select:

- IDF version: `v6.0.1`
- Target: `all`, or at least `esp32p4` and `esp32c6`
- Install path: the default `~/.espressif`

Activate the environment in every new shell:

```bash
source ~/.espressif/tools/activate_idf_v6.0.1.sh
```

Verify:

```bash
idf.py --version
```

Expected:

```text
ESP-IDF v6.0.1
```

### Windows

Install ESP-IDF v6.0.1 with Espressif's Windows installer, then run all `idf.py` commands inside the generated "ESP-IDF v6.0.1 PowerShell" shortcut.

## Clone The Repository

```bash
cd ~/repos
git clone --recursive https://github.com/naizhao/Pilot-Kit-Box-ESP32-P4.git
cd Pilot-Kit-Box-ESP32-P4
```

If the repository was cloned without submodules:

```bash
git submodule update --init --recursive
```

`firmware/components/esp32-rtl-sdr/` is a submodule pointing at the Pilot Kit fork of `esp32-rtl-sdr`; it contains the patched asynchronous librtlsdr path used on ESP32-P4.

## One-Time ESP32-C6 Slave Flashing For BLE

BLE is enabled by default with `CONFIG_PK_BLE_ENABLED=y`. A new Waveshare
ESP32-P4-WIFI6-Touch-LCD-4.3 board ships with factory AT firmware on the C6,
which does not speak the ESP-Hosted / NimBLE host protocol used by the P4
firmware. Flash the C6 hosted slave image once per board before using BLE.

If you do not need BLE yet, disable it before building:

```bash
cd firmware
idf.py menuconfig
# Pilot Kit Box -> uncheck "Initialise BLE GATT server at boot"
```

For the full wiring and flashing procedure, see [`hardware/c6_slave_firmware.md`](hardware/c6_slave_firmware.md).

Required items:

- 3.3 V USB-UART adapter
- Three jumper wires for GND / RXD / TXD
- A short wire or paperclip to short C6 IO9 to board GND during boot

P1 download-header wiring:

| P1 pin | Board label | Connect to |
|---|---|---|
| 1 | C6_TXD | USB-UART RX |
| 2 | C6_RXD | USB-UART TX |
| 3 | C6_IO9 | Short to any board GND during C6 download-mode boot |
| 4 | GND | USB-UART GND |

Fast path using the prebuilt ESPHome image:

```bash
curl -L -o firmware/network_adapter_esp32c6.bin \
    https://esphome.github.io/esp-hosted-firmware/v2.12.7/network_adapter_esp32c6.bin

# Polls /dev/cu.usbserial-* once per second, validates the image,
# and flashes one board without repeatedly reopening the UART.
firmware/tools/flash_c6_hosted.sh

# For multiple boards; unplug the USB-UART after each successful write.
firmware/tools/flash_c6_hosted.sh --batch
```

The script uses the tested 115200-baud, no-reset sequence and keeps
`ESP_IDF_VERSION=6.0`. Use `--check-only` to validate the environment
and image without touching hardware.

After flashing, remove the IO9-to-GND short and power-cycle the board. A working C6 path logs:

```text
transport: Identified slave [esp32c6]
ble_gatt: advertising as "Pilot Kit Box-XXXXXX"
```

## Configure

Use the project wrapper where possible:

```bash
cd firmware
./build.sh set-target esp32p4
```

The wrapper sets the `ESP_IDF_VERSION=6.0` environment variable required by ESP-Hosted Kconfig under ESP-IDF v6.0.1 and forwards to `idf.py`.

Important defaults already live in `firmware/sdkconfig.defaults`:

- `CONFIG_IDF_TARGET="esp32p4"`
- `CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y`
- custom `partitions.csv` with a 10 MiB LittleFS storage partition and optional MicroSD file backend
- ESP32-P4 v1.x silicon support for current Waveshare boards
- ESP-Hosted SDIO pins for the on-board C6
- USB host hub support for RTL-SDR dongles
- BLE enabled by default through `CONFIG_PK_BLE_ENABLED=y`

For details, see [`configuration.md`](configuration.md).

## Build

```bash
cd firmware
./build.sh build
```

The first build may take 5-15 minutes because ESP-IDF Component Manager downloads dependencies and the project compiles a large C codebase. Incremental builds are normally much faster.

Expected success output includes:

```text
Project build complete. To flash, run:
 idf.py flash
```

Main build artifacts:

| File | Purpose |
|---|---|
| `build/bootloader/bootloader.bin` | Second-stage bootloader |
| `build/partition_table/partition-table.bin` | Partition table |
| `build/pilot_kit_box.bin` | Main application |

## Connect The Board

Use H1, the Type-C port marked `USB TO UART`. It is the CH343P bridge for P4
flashing and monitoring.

The RTL-SDR data path is H2, the separate Type-C port marked `USB`, connected
to the P4 native USB 2.0 HS PHY. P1 is the C6 UART download header.

### Find The Serial Port

macOS:

```bash
ls -la /dev/cu.usbmodem*
```

Linux:

```bash
ls -la /dev/ttyACM* /dev/ttyUSB*
```

Windows:

Use Device Manager and look under "Ports (COM & LPT)" for the USB serial port.

## Flash And Monitor

macOS / Linux:

```bash
cd firmware
source ~/.espressif/tools/activate_idf_v6.0.1.sh
./build.sh -p /dev/cu.usbmodem5B7B0255751 flash monitor
```

Windows:

```powershell
idf.py -p COM3 flash monitor
```

Exit monitor with `Ctrl-]`.

If automatic reset fails:

1. Hold BOOT.
2. Tap RESET.
3. Release BOOT.
4. Run the flash command again.

## Expected Boot Logs

With the C6 slave already flashed and no optional peripherals attached, a healthy boot should include lines like:

```text
pilot_kit: Pilot Kit Box (ESP32-P4) boot
pilot_kit: IQ ring buffer ready: 524288 B (BYTEBUF)
pilot_kit: USB host stack online — spawning SDR + DSP tasks
rec_file: LittleFS mounted at /storage
pk_sd: no microSD card at boot (will keep probing)
sdr: USB client registered, waiting for RTL-SDR enumeration
display: ST7701 DSI ready: logical 800x480 -> PPA 90 CW -> native 480x800, 2 DPI buffers, app framebuffer 750 KiB PSRAM
pilot_kit: IMU init failed (...) — PFD will run without attitude
pfd: pfd_task running (G1000 landscape)
transport: Identified slave [esp32c6]
ble_gatt: advertising as "Pilot Kit Box-XXXXXX"
```

Optional hardware expectations:

| Action | Expected result |
|---|---|
| Attach RTL-SDR to H2 USB HS OTG | `USB NEW_DEV`, `Tuned to 1090000000 Hz`, `Sampling at 2000000 S/s`, DSP stream around 2.00 MB/s |
| Power the integrated 4.3-inch display | 800x480 boot splash appears for at least 3 seconds, then the PFD renders |
| Wire the BNO085 IMU | `imu: rpy = ... (acc=N ...)` logs update and PFD horizon follows motion |
| Fit the GT-U8 module | GPS/BeiDou fix, satellite/SNR, antenna, and system-time rows update on DIAG |
| Fit the BMP388 | PFD and DIAG show pressure, QNH-adjusted altitude, and vertical speed |
| Insert a FAT32 MicroSD card | DIAG shows mounted capacity; select `LOG = MICROSD`, reboot, and confirm logs under `/sdcard` |
| Enable BLE with flashed C6 | BLE scanner sees `Pilot Kit Box-XXXXXX` |

## Troubleshooting

### Bootloader requires chip revision v3.1+

Current Waveshare ESP32-P4-WIFI6 boards are often ESP32-P4 v1.x silicon. Ensure `sdkconfig.defaults` includes:

```text
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y
```

Then regenerate `sdkconfig` and rebuild.

### ESP-Hosted boots as H2/SPI instead of C6/SDIO

Symptom:

```text
assert failed: spi_mempool_create spi_drv.c:141
```

Cause: ESP-IDF v6.0.1 does not export `ESP_IDF_VERSION` for ESP-Hosted Kconfig. Use `firmware/build.sh`, which sets `ESP_IDF_VERSION=6.0`.

```bash
cd firmware
./build.sh reconfigure
./build.sh build
```

### `ble_transport_ll_init` aborts

The C6 is not responding to ESP-Hosted. Flash the C6 hosted slave firmware first, or disable `CONFIG_PK_BLE_ENABLED`.

### CMD5 `sdmmc_init_ocr: send_op_cond returned 0x107`

On Waveshare ESP32-P4-WIFI6 the C6 reset polarity must be active-high from the P4 software point of view:

```text
CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y
```

See [`hardware/c6_bringup_status.md`](hardware/c6_bringup_status.md).

### LittleFS mount fails

The partition table may not have been flashed. Run a full erase and flash:

```bash
./build.sh -p <PORT> erase-flash
./build.sh -p <PORT> flash
```

### MicroSD is not used for logs

MicroSD selection is persisted but takes effect only at boot. Insert a
FAT32 card, set `SETTINGS -> LOG` to `MICROSD`, and reboot. If the card
is absent or cannot mount at boot, the writer falls back to LittleFS.
Use DIAG to check card capacity and the active `LOG` backend.

### RTL-SDR does not enumerate

Check the H2 USB HS OTG adapter and power budget. RTL-SDR dongles can draw a
few hundred mA; use a powered hub or stable 5 V supply if necessary.

## Daily Development Loop

```bash
cd firmware
source ~/.espressif/tools/activate_idf_v6.0.1.sh
./build.sh build
./build.sh -p /dev/cu.usbmodem* flash monitor
```

For faster rebuilds, enable ccache:

```bash
export IDF_CCACHE_ENABLE=1
./build.sh build
```
