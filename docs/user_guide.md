# Pilot Kit Box — user guide

Chinese version: [`user_guide-zh_CN.md`](user_guide-zh_CN.md)

This guide describes the current **4.3-inch touch board**. The previous
2.4-inch four-button carrier is historical hardware under `docs/jlc/`.

## Safety boundary

Pilot Kit Box is a situational-awareness and development device, not a
certified flight instrument, navigation source, backup instrument or
collision-avoidance system. Use approved instruments, procedures, visual
scan and applicable regulations for every flight decision.

## 1. Board controls and ports

| Control / port | Daily use |
|---|---|
| Key3 **POWER** | Short press to power on; hold about two seconds to power off |
| Key1 **RESET** | Restart the ESP32-P4 |
| Key2 **BOOT** | Service control for download mode; not a UI key |
| H1 `USB TO UART` | P4 flashing, power and serial monitor |
| H2 `USB` | Native USB 2.0 HS OTG. Same nets as J3-27/25: with the carrier fitted the RTL-SDR plugs into the carrier's USB-A and H2 stays empty; on a bare board connect the dongle here |
| Touchscreen | All page navigation and user actions |

The legacy `button_task.c` is not compiled into the current firmware. There are no
MODE, TARE, UP or DOWN application buttons on the Rev1.2 board.

## 2. Touch navigation

A floating action button (FAB) stays near one screen edge.

- Tap **☰** to open the full-screen navigation grid.
- Page 1: **PFD**, **Traffic**, **Map**, **List**, **Search**, **Log**,
  **Tools**; page 2: **Diag**, **Setup**, **About**. Log and Tools are greyed
  placeholders (the pages are not implemented yet) and do not respond to taps.
- Swipe left/right to flip grid pages, or keep swiping right on page 1 to
  close the grid. The grid also closes after six seconds idle, or from the
  action bar's **Close** button.
- Hold the FAB for about 200 ms and drag it. It snaps to the left or right
  edge, and its side and vertical position are saved in NVS.
- On a diagnostic detail page, the FAB becomes **←**. Use it, the title-bar
  back control, or a right swipe to return.
- The current GT911 driver uses the first contact only. Use one finger.

### Level the attitude display

Open the navigation grid and hold **Level** in the bottom action bar for one
second. The firmware captures the current attitude reference and persists it
to NVS. A short tap only displays
“Hold 1 s to level the horizon.”

This touch UI does not expose the old ten-second factory-reset/DCD-wipe
gesture. Do not follow instructions written for the former TARE button.

## 3. First start and compass calibration

If BNO085 calibration quality remains zero, a full-screen figure-eight guide
appears automatically.

1. Move away from speakers, motors, laptops, phones and large steel objects.
2. Hold the device securely and make slow figure-eight motions through several
   orientations, rather than spinning around one axis.
3. Continue until accuracy reaches at least 2 and remains stable. The guide
   then closes automatically; **Later** dismisses it without calibrating.
4. Put the installed unit in its normal level pose, open the navigation grid
   and hold **Level** for one second.

BNO085 continuously refines its magnetic calibration while the unit moves.
Leveling stores the Pilot Kit software attitude reference; it is separate
from the BNO085 internal calibration state.

The current firmware assumes a vertical IMU breakout installation: chip face
toward the pilot, header on the pilot's left and VCC at the top. If your IMU
is installed differently, the firmware mounting transform must be changed
and verified; leveling alone cannot correct a wrong axis mapping.

## 4. Main pages

| Page | What it shows and how to use it |
|---|---|
| **PFD** | Attitude, pitch and bank, heading/HSI, ground speed, barometric altitude/vertical speed, GPS and traffic status |
| **Traffic** | 360° traffic radar; touch range/orientation controls and targets for selection/detail |
| **Map** | Offline basemap with an aviation overlay (airports, navaids, fixes); tap a symbol for its detail page |
| **List** | Tracked-aircraft table; tap headers to sort, drag to scroll and tap a row to open its detail drawer |
| **Search** | Search the local aviation databases for airports, navaids and fixes; open details or show one on the map |
| **Setup** | Language, QNH, map orientation, range, log backend, demo mode and MicroSD formatting controls |
| **About** | Project version, build information and hardware summary |
| **Diag** | Live SDR/DSP, BLE, GPS, IMU, barometer, storage, temperature and other subsystem cards; tap a card for detail |

**Log** and **Tools** are greyed placeholders in the grid; those pages are
not implemented yet. The grid switches top-level pages only; detail pages
never show a second navigation grid.

### Storage setting

Selecting MicroSD as the log backend takes effect at the next boot. If no
usable card is present, logging falls back to LittleFS for that boot.
Formatting uses a two-step, five-second confirmation and is refused while
the active logger is writing to the card.

### Demo mode (all data is simulated)

**Setup → DEMO MODE → ON**. While it is on, attitude, GPS, barometer and
ADS-B traffic are **all** replaced by a synthetic data set built into the
firmware, whether or not the real sensors are attached. Turning it off
restores the real sensors immediately — no reboot needed.

It exists so the normal-state layout and interactions can be reviewed on a
real unit with no peripherals attached. Out of the box the IMU, barometer,
GPS and SDR are all absent, so every page renders its degraded state and the
normal-state layout is never visible on hardware.

> ⚠️ **Nothing on screen is real.** Demo mode has no in-flight use. Never
> turn it on in flight.

**How you can tell it is on** (three places, none of them defeatable):

| Where | What you see |
|---|---|
| Top-right of every page | A red **⚠ DEMO** badge |
| Screen edge | A red frame around the whole display |
| Boot splash | A red banner: "DEMO MODE - ALL FLIGHT DATA IS SIMULATED" |

The badge is drawn on the widget layer above every page and has no separate
switch — there is **no state in which you get the fake data without the
marking**. The boot banner is drawn separately because demo mode survives a
reboot (it is stored in NVS) and the splash runs before the widget layer
exists, so the user must be told again on every power-up.

**GDL90 output**: while demo mode is on the box **stops sending** all
Ownship / Traffic reports and emits only the heartbeat, with the GPS-valid
bit forced to 0. On the phone this reads as "receiver online, no GPS, no
traffic". The reasoning is in [`ble_protocol.md`](ble_protocol.md) §6.5:
GDL90 has no "this is simulated" bit on the wire, so an EFB cannot tell fake
traffic from real traffic, and the only safe option is not to send it.
On-disk ADS-B recordings are unaffected — synthetic targets never enter the
decoder or the fusion table.

**No auto-exit**: there is no timeout. Having the mode change back without
the user noticing is more dangerous than staying in demo mode (classic mode
confusion). Running all day at a trade show is a legitimate use. To leave,
turn it off in Setup.

## 5. External-module behavior

- **RTL-SDR:** with the Pilot Kit carrier fitted, plug the dongle into the
  carrier's USB-A plug (J3-27/25) and leave H2 empty. On a bare board, use H2
  through a suitable USB-C OTG adapter or powered hub. The two share the same
  nets, so only one may be occupied. H1 is not the SDR data path and P1 is not
  USB.
- **BNO085:** current driver polls address `0x4A`; INT is wired to GPIO34 but
  unused by firmware.
- **BMP388:** current driver polls address `0x76`; INT is wired to GPIO31 but
  unused by firmware.
- **GPS:** UART1 uses P4 TX GPIO49 and P4 RX GPIO51 at 9600 8N1. Time comes
  from NMEA RMC; optional GPIO50 PPS wiring is not used by current firmware.
- **BLE:** a new board needs the C6 ESP-Hosted slave image flashed once through
  P1. See [`hardware/c6_slave_firmware.md`](hardware/c6_slave_firmware.md).

## 6. Troubleshooting

| Symptom | Check |
|---|---|
| No image or backlight | Confirm stable power; reset the board; inspect the display logs through H1 |
| Touch does not respond | Confirm GT911 is detected at `0x5D` or `0x14`; the factory board uses polling because TP_INT is open |
| Touch is rotated or offset | Confirm the firmware reports logical 800×480 and PPA 90° clockwise |
| Heading does not react correctly | Check the physical IMU mounting first, then run figure-eight calibration and Level |
| Traffic says no own position | Move the GPS antenna to open sky and wait for a fix |
| RTL-SDR does not enumerate | Confirm it is on the carrier USB-A plug (or H2 on a bare board), not H1 or P1, and that only one of the two is occupied; try a powered hub |
| MicroSD selection still says reboot | Leave the card inserted and reboot; absent/bad cards fall back to LittleFS |
| BLE never advertises | Complete the one-time C6 slave flash and check ESP-Hosted startup before DSI |

For connector pin numbers and electrical cautions, use
[`hardware/board_pinout.md`](hardware/board_pinout.md).
