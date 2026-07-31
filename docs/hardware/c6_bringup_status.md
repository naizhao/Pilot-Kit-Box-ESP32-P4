# C6 hosted slave bring-up — RESOLVED

Chinese version: [`c6_bringup_status-zh_CN.md`](c6_bringup_status-zh_CN.md)

> ✅ **Resolved 2026-05-14**: BLE GATT server advertising as
> `Pilot Kit Box-XXXXXX` (per-board MAC suffix) confirmed
> end-to-end. ESP32-P4 host ↔ ESP32-C6 slave SDIO + VHCI is up;
> NimBLE talks to the C6's BLE controller; the Pilot Kit Box
> mobile app can now scan and connect.

## What it took to fix

There were **four** stacked bugs hiding behind the original
`sdmmc_init_ocr: send_op_cond returned 0x107` failure. None of them
were ESP-Hosted internals — all three were on the Pilot Kit Box
side. In order of "things that masked the next thing":

### 1. Hosted reset option — `RESET_ACTIVE_HIGH=y`, not `LOW`

Found via [ESPHome issue #10393](https://github.com/esphome/esphome/issues/10393):
user `sobiso`'s working YAML for the same C6 wiring uses
`active_high: true`. ESPHome's `__init__.py` translates that
directly to `CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y`. We had the
opposite.

The later Rev1.2 schematic audit corrected our original mechanism:
P4 GPIO54 connects directly to C6 EN through R34, 0 Ω. There is no
inverter or level shifter. `RESET_ACTIVE_HIGH=y` remains the
hardware-verified ESP-Hosted configuration for this project, but it
must not be explained by a nonexistent board inverter. With our
previous `ACTIVE_LOW` setting, the slave's
SDIO peripheral was already running from the cold POR but the
controller couldn't respond. Hence `0x107 INVALID_RESPONSE` on CMD5
indefinitely.

Fix: `firmware/sdkconfig.defaults` line ~120.

### 2. Missing `esp_hosted_connect_to_slave` + `bt_controller_init` + `enable`

Once SDIO came up, `nimble_port_init()` succeeded (VHCI transport
attached) but every HCI command timed out:

```
NimBLE: HCI wait for ack returned 19
NimBLE: ogf=0x03, ocf=0x0003 : BLE_HS_ETIMEOUT_HCI (controller unresponsive)
```

Reason: the C6 slave's BT controller does **not** auto-start at
boot. The host has to drive an RPC handshake first:
`Feature_Command_BT_Init` → `Feature_Command_BT_Enable`. Found by
reading the upstream example
`managed_components/.../examples/host_nimble_bleprph_host_only_vhci/main/main.c`:

```c
esp_hosted_connect_to_slave();        /* brings up SDIO */
esp_hosted_bt_controller_init();      /* RPC: slave runs esp_bt_controller_init() */
esp_hosted_bt_controller_enable();    /* RPC: slave runs esp_bt_controller_enable() */
nimble_port_init();                   /* now the VHCI on the other side will answer */
```

The first three calls are required by ESP-Hosted-MCU's protocol but
not mentioned in the BLE protocol spec; you only spot them by
diffing against the working example. Fix lives in `ble_gatt_init()`
(firmware/main/ble_gatt.c).

### 3. ESP-Hosted SDIO queues were too large for early internal RAM pressure

ESP-Hosted's default SDIO queues allocate large DMA-capable internal
RAM pools early during startup. With PSRAM, USB, LCD, BLE, and the
rest of this firmware enabled, the default queue depth can fail before
`app_main()` has a chance to recover.

Fix: keep the SDIO queue depth at 8 for the BLE-only use case:

```text
CONFIG_ESP_HOSTED_SDIO_TX_Q_SIZE=8
CONFIG_ESP_HOSTED_SDIO_RX_Q_SIZE=8
```

### 4. `vTaskDelayUntil(&deadline, 0)` in GDL90 emitter

A latent bug in `emitter_task` — we passed `xTimeIncrement = 0`,
which FreeRTOS rejects with
`assert failed: xTaskDelayUntil tasks.c:1499 (( xTimeIncrement > 0U ))`.
Never fired before because the task only spawns after BLE is up,
and BLE was never up until step 1+2 landed. Fixed by switching to
the canonical pattern:

```c
TickType_t last_wake = xTaskGetTickCount();
const TickType_t period = pdMS_TO_TICKS(BLE_EMIT_PERIOD_MS);
while (1) { /* work */ vTaskDelayUntil(&last_wake, period); }
```

### Bonus: 31-byte adv overflow

After BLE controller came up, `ble_gap_adv_set_fields` returned
`BLE_HS_EINVAL (4)`. Cause: flags(3 B) + name "PilotKitBox"(13 B) +
complete 128-bit UUID(18 B) = 34 B, three bytes over the 31 B
advertisement cap. Standard split fix: name in adv, UUID in scan
response (`ble_gap_adv_rsp_set_fields`). (Production has since
moved to a 20-byte "Pilot Kit Box-XXXXXX" name with a MAC suffix,
so the split is still mandatory — even without the UUID the longer
name + flags alone is now 25 B, which still fits in the 31 B adv
once UUID is out of the way.)

### 6. 4.3-inch regression: DSI PHY grabs the LDO, C6 never boots (2026-07-28)

After the move to the 4.3-inch MIPI-DSI panel this link broke again, with a
signature unlike any of the above:

```text
I (11863) H_SDIO_DRV: Card init success, TRANSPORT_RX_ACTIVE
I (11865) transport: Waiting for esp_hosted slave to be ready
I (24926) transport: Not able to connect with ESP-Hosted slave device
I (26442) H_SDIO_DRV: Host is resetting itself, to avoid any sdio race condition
```

The SDIO **physical layer is entirely healthy** — CMD5, CIS, Function 1 ready
bit `IOR: 0x06`, 512-byte blocks, 4-bit negotiation `BUS_WIDTH: 0x42`. What
never arrives is the slave's INIT event, so the host times out after 13 s,
resets the slave, hits `failed to read registers` on the retry, and finally
reboots the whole P4 — a 26-second loop.

Elimination, all measured on hardware:

| Experiment | Result |
|---|---|
| esp_hosted 2.12.11 → 2.12.7 (match the C6 image) | still fails |
| BLE init moved ahead of the PFD render task | still fails |
| `SDIO_RESET_DELAY_MS` 1500 → 200 | still fails |
| SDIO clock 40 → 20 MHz | still fails |
| **Skip the PFD render task** (no LVGL/PPA/GT911/temp sensor) | still fails |
| **Skip `pk_display_init()`** | **handshake completes in 72 ms** |

The 2.4-inch board, running a line-for-line identical hosted config, receives
`ESP_PRIV_IF` → `Identified slave [esp32c6]` **32 ms** after
`Open data path at slave`. That board drives an SPI panel and never touches
the DSI PHY.

**Root cause**: the DSI PHY claims LDO channel 3 and drives it to 2.5 V
(`PK_LCD_DSI_PHY_LDO_CHANNEL` / `_MV` in `display.h`). That disturbance is
enough to keep the freshly-reset C6 from booting. Its SDIO peripheral still
answers card-layer commands, which makes the failure deeply misleading — it
looks like the slave is alive when only the peripheral is.

**Fix**: call `ble_gatt_init()` *before* `pk_display_init()` in `main.c`.
Handshake first, panel second; the extra second lands inside the splash
window. This ordering is a hard constraint — any future init step that
touches an LDO or perturbs power must come after the hosted handshake.

## Verified end-to-end steady-state log

```
I (9588) transport: Identified slave [esp32c6]
I (9636) transport: Base transport is set-up, TRANSPORT_TX_ACTIVE
I (9692) vhci_drv: Host BT Support: Enabled
I (9696) vhci_drv:      BT Transport Type: VHCI
I (10681) transport: Attempt connection with slave: retry[0]
I (10682) transport: Transport is already up
I (10684) ble_gatt: NimBLE host task running
I (10686) ble_gatt: GDL90 emitter task running
I (10690) pilot_kit: BLE GATT service up — advertised name landed in on_sync
I (10694) ble_gatt: BLE address 8c:fd:49:0b:5a:8a type=0
I (10698) NimBLE: GAP procedure initiated: advertise;
I (10717) ble_gatt: advertising as "Pilot Kit Box-0B5A8A"
I (10950) pfd: PFD 30 FPS | ...
I (11212) dsp: stream 0.00 MB/s | ...
... (sustained steady-state)
```

No HCI timeouts. No panics. No reboot loop. PFD 30+ FPS, DSP 1 Hz
heartbeat, NimBLE GAP procedure running.

## A/B test with esphome's prebuilt 2.12.7

Both our own C6 build and esphome's prebuilt v2.12.7 binary
(`https://esphome.github.io/esp-hosted-firmware/v2.12.7/network_adapter_esp32c6.bin`,
sha256 `ee7c546e…`) work equivalently. The C6 currently has
**esphome's prebuilt** flashed at ota_0; either is fine — esphome's
is just a convenient prebuilt that saves you the standalone slave
project setup. Our own build steps are documented in
[`c6_slave_firmware.md`](c6_slave_firmware.md).

## Outstanding (not blockers)

- iOS Current Time Service (CTS) GATT client read happens on first
  connect; haven't yet confirmed an iOS phone actually pushes the
  time. To test once a mobile client connects.
- BLE security: currently no pairing / bonding. Acceptable for ADS-B
  broadcast visibility only, but no private, control, or cockpit-
  sensitive data should be added before bonding / encryption is
  designed and documented.
- ESP-Hosted upstream has open issues
  ([#167](https://github.com/espressif/esp-hosted-mcu/issues/167),
  [#180](https://github.com/espressif/esp-hosted-mcu/issues/180))
  about long-run SDIO instability on v1.3 silicon and BLE scan
  stalls. We haven't run long enough to hit them. If we see
  `Unrecoverable host sdio state` in production, those are the two
  threads to follow.
