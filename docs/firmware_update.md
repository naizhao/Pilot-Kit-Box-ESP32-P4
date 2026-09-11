# ESP32-P4 Firmware Release And Web Flashing

Chinese version: [`firmware_update-zh_CN.md`](firmware_update-zh_CN.md)

This document explains how maintainers publish Pilot Kit Box ESP32-P4 firmware releases, and how end users update the ESP32-P4 main firmware and the RP2040 1090 receiver firmware (UF2) from a browser.

## Scope

- Applies to devices whose on-board ESP32-C6 has already been flashed with the ESP-Hosted slave firmware.
- Updates the ESP32-P4 main firmware and the RP2040 1090 receiver firmware (UF2).
- Does not update the ESP32-C6 co-processor firmware.
- Does not require end users to install ESP-IDF, Python, CMake, or Ninja.

## Maintainer Release Flow

1. Confirm that `firmware/` builds locally.
2. Before a formal release, update `firmware/version.txt` to the product version, for example `v0.8.0`.
   Normal local builds show `v0.8.0-<git-short-sha>`. Release CI passes
   `PROJECT_VER=v0.8.0`, so the boot splash, ABOUT page, and release
   assets use the exact formal version.
3. Create and push the matching tag:

   ```bash
   git tag v0.8.0
   git push origin v0.8.0
   ```

4. GitHub Actions runs `.github/workflows/release-esp32p4-firmware.yml`.
5. CI builds `firmware/` with the release version from the tag or manual workflow input.
6. CI creates or updates the GitHub Release assets and deploys the GitHub Pages flasher.

Before first use of GitHub Pages, confirm these repository settings:

- Settings -> Pages -> Build and deployment -> Source: `GitHub Actions`
- Actions permissions allow writing Releases and Pages.

## Version Source

- The default product version lives in `firmware/version.txt`; the current value is `v0.8.0`.
- Normal local builds use `firmware/version.txt` as the base and append the current git short hash for traceability.
- CI passes `-DPROJECT_VER="$RELEASE_VERSION"` so the embedded firmware version, manifest version, and asset names match.
- If a board-prefixed tag is used, for example `esp32p4-v0.8.0`, the release script normalises it to product version `v0.8.0` so asset names do not repeat `esp32p4`.

## Release Assets

All ESP32-P4 assets include `esp32p4` in the filename to leave room for future boards.

### Every release ships two sets: v3 and v4

The v3 and v4 board families mount the IMU 90 degrees apart and the
firmware applies the transform for the profile it was built with, so **every
release builds both**. Asset names carry `v3` or `v4`.

Flashing the wrong profile does not fail: the artificial horizon still shows a
live attitude, simply rolled by 90 degrees, which is easy to miss on a bench.
The web flasher therefore offers no default button — the profile must be picked
explicitly. The boot log line `imu: board profile v3|v4` confirms what was
flashed.

For release `v1.2.3`, the v4 set (replace `v4` with `v3` for the other):

| File | Purpose |
|---|---|
| `pilot-kit-box-esp32p4-v4-v1.2.3-factory.bin` | Merged binary for web flashing at offset `0x0` |
| `pilot-kit-box-esp32p4-v4-v1.2.3-bootloader.bin` | Maintainer troubleshooting asset, flash at offset `0x2000` |
| `pilot-kit-box-esp32p4-v4-v1.2.3-partition-table.bin` | Maintainer troubleshooting asset, flash at offset `0x8000` |
| `pilot-kit-box-esp32p4-v4-v1.2.3-app.bin` | Maintainer troubleshooting asset, flash at offset `0x10000` |
| `manifest-esp32p4-v4.json` | ESP Web Tools manifest |
| `SHA256SUMS-esp32p4-v4.txt` | Checksums |
| `pilot-kit-box-esp32p4-v4-v1.2.3.zip` | Complete downloadable package |

### Building a single profile (verification / troubleshooting)

Actions -> **Release ESP32-P4 firmware** -> *Run workflow*, set `board_profile`
to `v3` or `v4` (default `both`).

A single-profile run produces **only a downloadable workflow artifact**. It does
not publish GitHub Release assets and does not publish the Pages site: its
`dist/site` holds only half the manifests, and publishing it would leave the
other profile's button on the updater page returning 404. The Pages deploy
workflow emits a notice and skips quietly when no site artifact is present.

**Tag pushes always build both**; `board_profile` applies to manual runs only.

On the Pages site each profile gets its own directory:
`firmware/esp32p4/v3/latest/` and `firmware/esp32p4/v4/latest/`. The old
profile-less path `firmware/esp32p4/latest/` is **no longer generated** — it
amounted to a default button that silently flashed one profile.

The web flasher uses the merged binary because ESP Web Tools recommends a single merged image for ESP-IDF v4+ firmware. CI produces it with `esptool merge-bin`.

## End-User Web Update Flow

1. Open the GitHub Pages flasher in Chrome or Edge.
2. Connect Pilot Kit Box to the Type-C port near the BOOT button with a USB-C data cable.
3. Click "Connect and flash ESP32-P4 firmware".
4. Pick the Pilot Kit Box USB serial port in the browser serial picker.
5. If the page asks whether to erase data, choose to keep data for normal firmware upgrades.
6. Wait for flashing to finish; the device reboots automatically.

If connection fails:

1. Hold BOOT.
2. Tap RESET.
3. Release BOOT.
4. Return to the web page and reconnect.

## RP2040 Co-processor Firmware (1090 MHz, expansion board)

The RP2040 on the v3/v4 expansion board decodes 1090 MHz Mode-S and runs its
own firmware (`adsb1090`). It is a separate chip: the web flasher cannot reach
it (no Web Serial), and it is not part of the ESP32-P4 assets above. The image
is board-family independent — there is no v3/v4 profile to pick.

### Routine update — BOOTSEL drag-and-drop

1. Get the UF2: the flasher page's RP2040 download card links the latest
   `adsb1090.uf2`, and every release also ships
   `pilot-kit-box-rp2040-<version>.uf2` as a Release asset.
2. Hold **BOOTSEL** on the expansion board and plug the RP2040's USB port into
   the computer. The RP2040 enumerates as a USB mass-storage drive (`RPI-RP2`).
3. Drag the `.uf2` onto the drive. The RP2040 reboots into the new firmware
   automatically.

To build the image from source instead, see
[`../firmware/rp2040/README.md`](../firmware/rp2040/README.md) for toolchain
requirements, pinned dependencies, and `./build.sh`.

### Recovery from bad firmware

Repeat the same BOOTSEL procedure. The RP2040's USB mass-storage mode lives in
its boot ROM and does not depend on the flashed firmware, so a broken image
cannot lock you out: enter BOOTSEL again and drop the UF2 once more. (ROM
capability, guaranteed by design; not yet verified on a real board — bench
validation pending.) Note that the v4 board removed the RP2040 SWD test
points, so BOOTSEL is the practical recovery path there; a debugger would
require flying wires to the chip's SWCLK/SWDIO pins.

### CC1312R firmware (978 MHz front-end)

There are **two different paths**, and which one you need depends on whether the
chip has ever been programmed. Getting this wrong wastes a lot of time, so read
the distinction first.

| Situation | Path | Needs external hardware? |
|---|---|---|
| Factory-blank chip (never programmed) | cJTAG probe | **Yes** — one time only |
| Any later firmware update | RP2040 proxy over the ROM serial bootloader | No |

#### Why a blank chip needs a probe

The CC13x2 ROM contains a serial bootloader, but it is gated by CCFG:
`BOOTLOADER_ENABLE` is **only** enabled when it reads `0xC5` (TRM SWCU185G
Table 11-15). A factory-blank chip has erased CCFG — all `0xFF` — so the
bootloader is **disabled**. It is a *field-update* mechanism, not a
*first-programming* mechanism. This is why every TI LaunchPad ships with an
XDS110 on board.

An RP2040 bit-bang cJTAG engine exists in `firmware/rp2040/cjtag.c` and was
written for exactly this case, but it has **not** been made to work on real
hardware (see the internal handover doc). Do not plan around it.

#### Path A — first flash, with a cJTAG probe

Any probe that speaks **cJTAG (2-wire IEEE 1149.7)** works. Plain SWD/JTAG
probes do **not**: the CC13x2 has no SWD, so DAPLink / ST-Link / generic
CMSIS-DAP are unusable regardless of price.

Known-good options: TI XDS110 (standalone or on any TI LaunchPad), SEGGER
J-Link with cJTAG support (`-if cJTAG`).

The board has **no debug header and no test points** on these nets, so solder
directly to the pins (verified against the PCB pad/net data):

| Signal | Solder point | Alternative |
|---|---|---|
| TMSC | U10 pin 24 (CC1312R) | U8 pin 27 (RP2040) |
| TCKC | U10 pin 25 | U8 pin 28 |
| RESET_N | R47, the pad nearer the chip (0402 — much easier than a QFN pin) | U10 pin 35 |
| GND / VTref | any ground / 3V3 | |

Before connecting the probe, type `Z` in the RP2040 console. That parks
GPIO16/17/18 in high-impedance and suspends the SPI master — otherwise the
RP2040 drives against the probe, and its recovery logic periodically pulls
RESET_N low and kills the probe's session. Reset the RP2040 to restore.

Then flash `firmware/cc1312r/build/adsb978_cc13.bin` to address `0x0` with
your probe's usual tooling (OpenOCD, UniFlash, or J-Link).

#### Path B — all later updates, through the RP2040

```sh
python3 tools/cc13_flash.py /dev/ttyACM0 firmware/cc1312r/build/adsb978_cc13.bin
```

This uses the CC1312R's ROM serial bootloader over SSI0, entered through the
backdoor pin (DIO13 = `SUBG_SYNC`, driven by RP2040 GPIO15). 1090 decode pauses
during the transfer and resumes afterwards.

The tool refuses images that would disable the bootloader or its backdoor —
see "Anti-brick gate" below. `--force` overrides it.

Interactive equivalent in a serial terminal: type `U`, then send 4 bytes of
little-endian length followed by the image. The device prints `BSL-DONE` on
success, or `BSL-FAIL: <reason>` naming the failing step.

#### Anti-brick gate

`tools/cc13_flash.py` parses the CCFG carried in the image **before it erases
anything** and refuses to flash if the result would be unrecoverable:

- `BOOTLOADER_ENABLE` not `0xC5` — the update channel would be gone for good
- `BL_ENABLE` not `0xC5` — the backdoor would be gone, so a non-booting image
  could no longer be replaced
- `BL_PIN_NUMBER` not 13, or `BL_LEVEL` not active-high — the backdoor would be
  on the wrong pin or would trigger on every power-up
- an image too short to contain CCFG at all — flashing erases the whole bank,
  so the CCFG area becomes `0xFF`, which has the same effect as disabling it

Recovering from any of these means going back to Path A: dismantling the unit
and soldering to QFN pins. That is why the gate refuses by default.

The image built from this repository satisfies the gate; a build-time check
(`firmware/cc1312r/check_ccfg.py`) enforces it so the setting cannot silently
regress to the SDK default, which disables the bootloader.

#### Troubleshooting

| Symptom | Likely cause |
|---|---|
| `BSL-FAIL: no response` | Chip has never been programmed (bootloader disabled), or it is running an image whose CCFG disabled the bootloader → Path A |
| `BSL-FAIL: CRC mismatch` | Data all written but checksum differs. The TRM does not state the CRC32 polynomial; we use standard IEEE 802.3. Suspect that assumption before suspecting the flash |
| `BSL-FAIL: erase refused` | `BANK_ERASE_DIS` set in CCFG |
| Tool refuses before touching the port | Anti-brick gate — read its message; it names the exact field |
| No serial port | Flash the RP2040 first: hold BOOTSEL, plug USB, drop in the UF2 |

## Limits

- iPhone / iPad Safari does not support Web Serial and cannot flash directly.
- Android browser Web Serial support is inconsistent and is not the primary path.
- This path does not handle first-time ESP32-C6 flashing; the C6 must already contain the hosted slave image.
- Future board variants need their own workflow, manifest path, and filename prefix. Do not reuse `esp32p4` asset names for other boards.
