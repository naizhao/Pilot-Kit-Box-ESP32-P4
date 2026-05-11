# Flashing the ESP-Hosted slave firmware onto the C6 module

The Pilot Kit Box firmware delegates **all** Bluetooth + Wi-Fi work to
the ESP32-C6-MINI-1 daughter-chip. That C6 needs its own firmware —
specifically, the **ESP-Hosted slave** image — before the P4-side
NimBLE host can do anything. This document is the one-time bring-up
guide.

## 1. What you need

- USB-to-serial adapter (any 3V3 TTL UART will do — the P4's CH343P
  Type-C bridge already works if you don't have a spare one).
- 4 jumper wires.
- A checkout of [`espressif/esp-hosted`](https://github.com/espressif/esp-hosted),
  matching the major version vendored under our
  `firmware/managed_components/espressif__esp_hosted/` (currently the
  same `*` version that the IDF Component Manager resolved).
- ESP-IDF v5.4 or newer, exported into the shell (same install you
  use for the host firmware works fine).

## 2. Wire the C6 debug header (H4)

The C6's UART0 + bootloader strap (IO9) are broken out to a 4-pin
header on the back of the Waveshare board labelled `IO9 / GND / RXD /
TXD` (item 10 in the silkscreen, schematic header `H4`):

| H4 pin | Function | Connect to USB-serial adapter |
|---|---|---|
| 1 | C6 IO9 (bootloader strap) | **Hold this LOW during reset → release after esptool starts** |
| 2 | GND | GND |
| 3 | C6 U0RXD (input to C6) | TX of the adapter |
| 4 | C6 U0TXD (output from C6) | RX of the adapter |

> The C6's RESET line is tied to the P4's GPIO54 (active-low). Either
> press the board's RST button to reset both chips, or run `idf.py`'s
> `--before default-reset` which toggles the equivalent serial line.

## 3. Build the slave image

```bash
cd ~
git clone --recursive https://github.com/espressif/esp-hosted.git
cd esp-hosted/slave_drv  # or `slave/` depending on version
. ~/.espressif/v6.0.1/esp-idf/export.sh   # the host install works
idf.py set-target esp32c6
idf.py menuconfig
```

In menuconfig set:

```
Component config → ESP-Hosted config →
    Host Interface Choice → SDIO Slave
    Bluetooth Support → NimBLE (matches our host)
    SDIO Slave Pin Configuration →
        CLK = 18, CMD = 19, D0 = 14, D1 = 15, D2 = 16, D3 = 17
        (must match the host pins — see docs/hardware/board_pinout.md)
```

Then:

```bash
idf.py build
```

## 4. Flash the C6

While holding C6_IO9 (H4 pin 1) **low** with a jumper to GND:

```bash
idf.py -p /dev/cu.usbserial-XXXX --baud 460800 flash
```

The slave's `idf.py flash` will trigger the C6's bootloader via the
host-side `--before default-reset` sequence. Watch the output — you
should see `Hash of data verified.` and `Hard resetting via RTS pin...`

Release the IO9 jumper, press the **RST** button on the front of the
P4 board to reboot both chips, and disconnect the H4 wiring.

## 5. Verify from the P4 side

Flash the Pilot Kit Box firmware as usual:

```bash
cd ~/path/to/Pilot-Kit-Box-ESP32-P4/firmware
idf.py -p /dev/cu.usbmodem* flash monitor
```

Expected console additions vs. Phase 2:

```
I (xxx) ble_gatt: NimBLE host task running
I (xxx) ble_gatt: BLE address aa:bb:cc:dd:ee:ff type=1
I (xxx) ble_gatt: advertising as "PilotKitBox"
```

If you instead see something like `nimble_port_init: ESP_ERR_TIMEOUT`
or a `HCI: command timed out`, the C6 isn't responding over SDIO.
Double-check:

1. The SDIO pin assignments on both sides match what's in this doc.
2. The C6 was actually flashed (re-flash and watch the upload progress
   in `slave_drv`'s `idf.py monitor`).
3. The RESET line works — bridge `GPIO54` low briefly, then high; the
   C6 should reboot and re-establish the SDIO link.

## 6. Verify from a phone

Use any BLE scanner (nRF Connect on Android / iOS, LightBlue, etc.):

1. Filter by name `PilotKitBox`.
2. Connect.
3. Expand the `1090AD5B-0000-1000-8000-1090AD5B0000` service.
4. Subscribe to the three notify characteristics:
   - `…0001` Traffic Report — receives a GDL90 binary frame
     per tracked aircraft every second.
   - `…0002` Heartbeat — receives a 11-byte GDL90 Heartbeat
     frame every second.
   - `…0003` Raw ts-line — receives `<ts_ms> *<HEX>;` ASCII
     per CRC-valid Mode-S frame.

The Pilot Kit mobile app subscribes to the same UUIDs and parses
GDL90 into traffic targets shown on the moving map.

## When the slave firmware needs reflashing

You only need to redo this when:

- ESP-Hosted publishes a major version with an updated wire protocol
  (the component registry usually pins both sides to the same
  protocol version — check release notes).
- The Waveshare board ships with non-ESP-Hosted C6 firmware (e.g. AT
  command set, demo blinky). Brand new boards typically do; one-time
  reflash is required.

The host (Pilot Kit Box) firmware can be re-flashed independently
once the slave has the right image — no need to repeat steps 2-4 for
every iteration on our side.
