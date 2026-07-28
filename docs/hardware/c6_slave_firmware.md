# Flashing the ESP-Hosted slave firmware onto the C6 module

Chinese version: [`c6_slave_firmware-zh_CN.md`](c6_slave_firmware-zh_CN.md)

> 📌 **What this is**: a **one-time setup** every freshly-unboxed
> Waveshare ESP32-P4-WIFI6 board needs to go through if you want
> Bluetooth (and, in a future phase, Wi-Fi) to work. The P4 firmware
> we ship talks to the on-board C6 over SDIO using Espressif's
> "ESP-Hosted" protocol; out of the factory the C6 runs a different
> firmware (AT commands) and doesn't understand us.
>
> Once you've flashed the C6 with the matching slave firmware, the
> setup is **persistent** — power cycles, P4 re-flashes, brick attacks
> on the P4 side, none of that affects the C6's slave image. You only
> repeat this dance:
>   - per brand-new board (one-time per board), or
>   - when Espressif publishes a new major ESP-Hosted release whose
>     wire protocol drifted (rare; the component manager pin both
>     sides to the same version).
>
> ⏱️ **Time required**: ~30 minutes including clone + build + wiring +
> flashing.
>
> 🛠️ **What if I don't want to do this right now?** Fine — the P4
> firmware has a build-time toggle to skip BLE entirely:
>
> ```bash
> cd firmware
> idf.py menuconfig
> # → Pilot Kit Box → uncheck [*] Initialise BLE GATT server at boot
> idf.py build && idf.py -p /dev/cu.usbmodem* flash monitor
> ```
>
> Everything else (RTL-SDR / LittleFS / UART / LCD / IMU) keeps working;
> only the BLE GATT advertise / GDL90 notify path is disabled. Come
> back here whenever you're ready to enable BLE.
>
> Parent guide: [`docs/BUILD.md`](../BUILD.md). This document is what
> the parent's Section 3 links to.

## 1. What you need

- USB-to-serial adapter (any 3V3 TTL UART will do — CP2102 / CH340 /
  FT232 / etc. Note the P4's CH343P Type-C bridge **does not** work
  for flashing C6 — it's wired to the P4, not the C6).
- 3 jumper wires (female-to-female works fine).
- 1 short / paperclip / extra wire to bridge IO9 to a board GND while
  flashing (board-local short, not connected to the USB-UART).
- ESP-IDF v6.0 or newer, exported into the shell (same install you
  use for the host firmware works fine). Slave sources live at
  `firmware/managed_components/espressif__esp_hosted/slave/`; just
  copy that directory anywhere outside the P4 project and build it
  as a standalone ESP-IDF project against `esp32c6`.

## 2. Wire the C6 debug header (H4)

The C6's UART0 + bootloader strap (IO9) are broken out to a 4-pin
header on the back of the Waveshare board labelled `IO9 / GND / RXD /
TXD` (item 10 in the silkscreen, schematic header `H4`). You wire
**3 of the 4 pins to the USB-UART**, plus a **board-local short on
IO9 to GND**:

| H4 pin | Function | Wired to |
|---|---|---|
| 1 | C6 IO9 (bootloader strap) | **Board-local short to any P4 GND** with a paperclip — keeps C6 in download mode during flash, remove after flashing |
| 2 | GND | GND of the USB-UART |
| 3 | C6 U0RXD (input to C6) | TX of the USB-UART |
| 4 | C6 U0TXD (output from C6) | RX of the USB-UART |

> Don't wire IO9 to the USB-UART's DTR — the H4 header doesn't carry
> the chip RESET line, so there's no way for esptool to toggle reset
> automatically. Use `--before no-reset` and power-cycle the board
> manually with the BOOT button held (see §4 step-by-step below).
>
> The C6's RESET pin is tied to P4's GPIO54. On the Waveshare board
> there's an inverter / level shifter between P4-GPIO54 and C6-EN,
> so from P4 software the reset signal is ACTIVE-HIGH (we drive
> GPIO54 HIGH to assert reset, LOW to release). Pressing the board's
> RST button on the front resets the P4, not the C6, so a full power
> cycle is the only reliable way to reboot C6 cold.

## 3. Build the slave image

> 💡 **Quickest path — skip this section** and use esphome's
> prebuilt binary (see the Alternative box in Section 4). Only build
> the slave yourself if you want to tweak its config or vendor a
> different ESP-Hosted version.

The slave sources are already vendored under
`firmware/managed_components/espressif__esp_hosted/slave/`. Copy
that directory anywhere outside the P4 project (the slave's
`partitions.esp32c6.csv` collides with the P4 project's partition
table at build time):

```bash
cp -R firmware/managed_components/espressif__esp_hosted/slave \
      ~/hosts/c6_slave
cd ~/hosts/c6_slave
. ~/.espressif/tools/activate_idf_v6.0.1.sh
export PATH="$IDF_PATH/tools:$PATH"   # eim's activate doesn't add this
idf.py set-target esp32c6
```

The slave's `sdkconfig.defaults.esp32c6` already turns on
`CONFIG_ESP_SDIO_HOST_INTERFACE=y` + `CONFIG_BT_LE_HCI_INTERFACE_USE_RAM=y`
which is what we need; no menuconfig pass is strictly required.

```bash
idf.py build
# Output: build/network_adapter.bin (~1.2 MB)
```

The SDIO slave pins on ESP32-C6 are **hardware-fixed** (HS_SLAVE
peripheral, not GPIO-matrix routable):

| C6 GPIO | Function | Wired on Waveshare board to P4 GPIO |
|---|---|---|
| 18 | SDIO_CMD | P4 GPIO 19 |
| 19 | SDIO_CLK | P4 GPIO 18 |
| 20 | SDIO_D0  | P4 GPIO 14 |
| 21 | SDIO_D1  | P4 GPIO 15 |
| 22 | SDIO_D2  | P4 GPIO 16 |
| 23 | SDIO_D3  | P4 GPIO 17 |

You can't (and don't need to) change these on the C6 side. The P4
host pins are configured separately in our `sdkconfig.defaults`.

## 4. Flash the C6

### Recommended: use the project script

Download the pinned image to the default location:

```bash
curl -L -o firmware/network_adapter_esp32c6.bin \
    https://esphome.github.io/esp-hosted-firmware/v2.12.7/network_adapter_esp32c6.bin
```

Then run:

```bash
firmware/tools/flash_c6_hosted.sh
```

The script validates that the image is an ESP32-C6 `network_adapter`
2.12.7 image configured for 4 MB / DIO / 80 MHz, including its image
hashes. It polls `/dev/cu.usbserial-*` once per second and starts only
one esptool connection after the port appears. This avoids repeatedly
opening a macOS USB-UART device, which can produce
`termios.error: (22, 'Invalid argument')`.

Useful modes:

```bash
# Validate the ESP-IDF environment and image without touching hardware
firmware/tools/flash_c6_hosted.sh --check-only

# Flash one board through an explicit port
firmware/tools/flash_c6_hosted.sh --port /dev/cu.usbserial-0001

# Flash multiple boards; unplug the UART after each successful board
firmware/tools/flash_c6_hosted.sh --batch
```

No Enter key is required while waiting. Batch mode detects removal of
the current serial port once per second before waiting for the next
board.

**Power-on sequence matters here**. H4 has no RESET line, so esptool
can't auto-toggle reset; you have to manually arrange for both chips
to power up with the right strap pin held low:

- **C6**: IO9 strap held LOW at power-on → C6 ROM enters download
  mode and waits for esptool.
- **P4**: BOOT button held while powering up → P4 ROM enters
  download mode and **doesn't run our firmware**. This matters
  because our P4 firmware drives GPIO54 (which is wired to C6's EN
  through an inverter on this board) — if P4 runs while we're
  flashing C6, it'll reset C6 mid-write and corrupt the image.

Step-by-step:

1. **Unplug P4 Type-C** (board completely powered off, no LEDs on).
2. **Wire the 3 dupont lines** per §2 (UART GND/TX/RX → H4-2/3/4).
3. **Bridge H4-1 (IO9) to any P4 GND pin** with a paperclip or a
   short wire — board-local short, do **not** route this through the
   UART adapter.
4. **Plug the USB-UART adapter into the computer first** (not the
   board's Type-C yet). Verify `ls /dev/cu.usbserial-*` (macOS) or
   `ls /dev/ttyUSB*` (Linux) shows the adapter — only the adapter is
   powered at this point, the P4/C6 are still dark.
5. **Press and hold the BOOT button** on the front of the P4 board.
6. **Plug the P4 Type-C into the computer** while still holding BOOT.
   In this single instant:
   - P4 sees BOOT low → halts in download mode → GPIO54 stays high-Z
     and never touches C6.
   - C6 sees IO9 low (your paperclip) → halts in ROM download mode →
     ready for esptool over UART.
7. **Release BOOT** (the strap is latched, releasing is harmless).
8. **Run the esptool flash command** with `--before no-reset` (since
   esptool can't toggle a reset line that isn't there):

   ```bash
   # If you built the slave yourself (path B):
   esptool --chip esp32c6 -p /dev/cu.usbserial-XXXX -b 460800 \
       --before no-reset --after hard-reset write-flash \
       --flash-mode dio --flash-freq 80m --flash-size 4MB \
       0x0      build/bootloader/bootloader.bin \
       0x8000   build/partition_table/partition-table.bin \
       0xd000   build/ota_data_initial.bin \
       0x10000  build/network_adapter.bin

   # If you downloaded esphome's prebuilt (path A) — only the app:
   esptool --chip esp32c6 -p /dev/cu.usbserial-XXXX -b 460800 \
       --before no-reset --after hard-reset write-flash \
       --flash-mode dio --flash-freq 80m --flash-size 4MB \
       0x10000 /tmp/network_adapter_esp32c6.bin
   ```

   Watch for `Hash of data verified.` That's success.

9. **Unplug P4 Type-C** (kill all power again).
10. **Remove the IO9 ↔ GND short**. (If you leave it, next power-on
    C6 enters download mode again and won't run the firmware you
    just wrote.)
11. **Plug P4 Type-C back in** — normal cold boot. C6 sees IO9 high
    this time, runs the hosted slave; P4 runs our firmware; on the
    P4 console you should see:

    ```
    I (xxxx) transport: Identified slave [esp32c6]
    I (xxxx) ble_gatt: advertising as "Pilot Kit Box-XXXXXX"
    ```

The UART dupont wires can stay connected — they don't interfere
with normal operation. Or pull them off; doesn't matter for BLE.

> 💡 **Alternative — esphome's prebuilt binary** (path A above):
> esphome publishes a matching network_adapter.bin at
> <https://esphome.github.io/esp-hosted-firmware/manifest/esp32c6.json>.
> Grab the v2.12.7 URL, drop it to `/tmp/`, and flash only `0x10000`
> (keep your own bootloader + partition table). Verified bit-for-bit
> functionally equivalent to our own build for ESP-Hosted purposes.

### Troubleshooting — flash drops out mid-write ("chip stopped responding")

If `write-flash` dies partway through, and **at a different
percentage each run** (e.g. 0% one time, 21% the next):

```text
Writing at 0x0006c190 [=====>     ]  21.0% 147456/702923 bytes...
A fatal error occurred: The chip stopped responding.
```

This is **not** a corrupt binary (the `Compressed N bytes` line
matching the file size proves the upload started fine), and **not
necessarily "baud too high."** Read the pattern instead:

- **Dies at a different % each time** → bad contact / supply /
  auto-reset glitch.
- **Dies at the same % every time** → supply sags when flash erase +
  write current spikes.

Work it in this order (cheapest first):

1. **Contact & power.** Reseat every dupont wire, plug the USB-UART
   straight into the computer (no hub / extension cable), and unplug
   other high-draw USB devices to free up current. A flaky jumper is
   the single most common cause of *random* drop-outs.
2. **Lower the baud.** Walk `-b` down `460800 → 230400 → 115200 →
   57600`. The ROM handshake is always 115200; `-b` only sets the
   post-stub transfer speed, so anything below 115200 actually slows
   the whole transfer.
3. **Manual download mode + reset fully disabled** (most reliable).
   Put C6 + P4 into download mode by hand exactly as in §4 steps 1-7,
   then run the §4 flash command with `--after hard-reset` changed to
   `--after no-reset` (and `-b` dropped per step 2). H4 carries no
   RESET line anyway, and on failure esptool otherwise tries to pulse
   RTS (the `Hard resetting via RTS pin…` line right before the
   traceback) — disabling the after-reset removes that noise.
   Power-cycle the board yourself once the write completes.

## 5. Verify from the P4 side

> ✅ **Bring-up status**: fully working as of 2026-05-14 with
> `CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y` + the
> `connect_to_slave` → `bt_controller_init` → `bt_controller_enable`
> → `nimble_port_init` sequence in `ble_gatt_init()`. Background
> writeup in [`c6_bringup_status.md`](c6_bringup_status.md).

### How to confirm C6 itself is healthy (independent of P4 host)

Wire just CP2102 GND + RX (no TX needed) to H4-2 and H4-4, then
power-cycle the board. C6's UART0 prints its full ESP-Hosted slave
boot log at 115200 baud — look for:

```
I (449) co-pro-main: ESP-Hosted-MCU Slave FW version :: 2.12.7
I (455) co-pro-main: Transport used :: SDIO only
I (471) SDIO_SLAVE: Using SDIO interface
I (488) co-pro-main: host reset handler task started
I (491) main_task: Returned from app_main()
```

If you see those lines, C6 is good and the problem is on the P4 host.

### Once the SDIO bring-up is fixed (`PK_BLE_ENABLED=y`)

Flash the Pilot Kit Box firmware as usual:

```bash
cd ~/path/to/Pilot-Kit-Box-ESP32-P4/firmware
idf.py -p /dev/cu.usbmodem* flash monitor
```

Expected console additions after flashing P4 firmware with BLE enabled:

```
I (xxx) ble_gatt: NimBLE host task running
I (xxx) ble_gatt: BLE address aa:bb:cc:dd:ee:ff type=1
I (xxx) ble_gatt: advertising as "Pilot Kit Box-AABBCC"
```

where `AABBCC` is the upper-case hex of the last 3 bytes of the
shown BLE address (e.g. `8c:fd:49:0b:5a:8a` → name ends `-0B5A8A`).
Stable across reboots, unique per board.

## 6. Verify from a phone

Use any BLE scanner (nRF Connect on Android / iOS, LightBlue, etc.):

1. Filter by name prefix `Pilot Kit Box-` (not the full name — each
   board has a different suffix).
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
