# ESP32-P4 Firmware Release And Web Flashing

Chinese version: [`firmware_update-zh_CN.md`](firmware_update-zh_CN.md)

This document explains how maintainers publish Pilot Kit Box ESP32-P4 firmware releases, and how end users update the ESP32-P4 main firmware from a browser.

## Scope

- Applies to devices whose on-board ESP32-C6 has already been flashed with the ESP-Hosted slave firmware.
- Updates only the ESP32-P4 main firmware.
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

## Limits

- iPhone / iPad Safari does not support Web Serial and cannot flash directly.
- Android browser Web Serial support is inconsistent and is not the primary path.
- This path does not handle first-time ESP32-C6 flashing; the C6 must already contain the hosted slave image.
- Future board variants need their own workflow, manifest path, and filename prefix. Do not reuse `esp32p4` asset names for other boards.
