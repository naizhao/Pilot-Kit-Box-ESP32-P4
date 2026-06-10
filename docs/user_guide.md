# Pilot Kit Box — User Guide

A daily-driver guide for the four-button Pilot Kit Box hardware. For
the engineer-facing pinout, GPIO assignments, and SH-2 command
implementation details, see `docs/hardware/board_pinout.md`.

Chinese version: [`user_guide-zh_CN.md`](user_guide-zh_CN.md)

---

## Safety boundary

Pilot Kit Box is a situational-awareness and development device, not
a certified flight instrument, backup instrument, navigation source,
or collision-avoidance system. Use certified aircraft instruments,
approved procedures, visual scan, and applicable regulations for all
flight decisions.

## 1. Buttons at a glance

The board has **four physical buttons** plus one **combo gesture**.
TARE / UP / DOWN cluster on the right-side header; MODE sits on the
left-side header at GPIO5 because the deep-sleep wake hardware needs
an LP_IO pin (see `docs/hardware/board_pinout.md` §3.4 for the why):

```
   left header      right header
       MODE         TARE   UP    DOWN
       ( 5)         (26)  (22)   (23)
        |            |     |      |
        ●            ●     ●      ●
```

| Button | Short press (< 3 s) | Long press (≥ 3 s) | Very-long press (≥ 10 s) |
|--------|---------------------|--------------------|---------------------------|
| **TARE** | Context-sensitive: move the selected SETTINGS row; bind the highlighted ADS-B LIST aircraft as own-ship; live IMU tare on other pages | **Persist** the current IMU tare to NVS so it survives reboot | **Factory reset** — wipe NVS tare + BNO's persisted DCD + reinit the chip. Use this if heading is stuck wrong after a reboot. |
| **MODE** | Cycle screen: **PFD → TRAFFIC → ADS-B LIST → SETTINGS → ABOUT → DIAG → PFD** | **Soft power off** — backlight off + ESP32-P4 deep sleep; press MODE again to wake/cold-boot | — |
| **UP**   | Previous target in TRAFFIC/LIST; adjust SETTINGS row; scroll ABOUT/DIAG up | *(suppressed — reserved for the combo)* | — |
| **DOWN** | Next target in TRAFFIC/LIST; adjust SETTINGS row; scroll ABOUT/DIAG down | *(suppressed — reserved for the combo)* | — |

### Combo gestures

| Gesture | Action |
|---|---|
| **UP + DOWN held together ≥ 5 s** | Open BLE pairing window. The current firmware records the request; mobile UI handling is not implemented yet. |

---

## 2. First-time use (or: my heading is completely wrong)

If the on-screen heading drifts, sticks at a wrong number, or won't
respond to device rotation, the BNO085's magnetometer fusion is
either uncalibrated or its flash holds a poisoned calibration.
The recovery is:

### Step 1 — Factory reset the IMU

```
TARE very-long-press 10 s
```

You'll see in the serial log:

```
W (xxx) imu: factory reset: wipe SW tare + NVS + BNO persisted
            state + reinit chip
I (xxx) imu: BNO: clearing persisted reorientation (identity quat + persist)
I (xxx) imu: BNO: clearing persisted DCD (mag/gyro/accel zero offsets)
```

### Step 2 — Figure-8 motion (~15 seconds)

Hold the device in your hand and **draw 8-shapes in the air**,
slowly, sweeping through several different orientations (not just
rotation around one axis). This is the same motion the iOS / Android
compass calibration screen asks for.

You can watch progress in the 1 Hz serial log:

```
imu: rpy = ... (acc=0 ...)   ← starting from nothing
imu: rpy = ... (acc=1 ...)   ← converging
imu: rpy = ... (acc=2 ...)   ← good enough to tare
imu: rpy = ... (acc=3 ...)   ← high confidence
```

> If the device has a screen on, a calibration wizard overlay will
> appear automatically once the firmware sees `acc=0` for a while —
> it'll show a figure-8 hint and the current `acc` quality. It
> auto-dismisses once `acc` reaches 2 and stays there for a few
> seconds.

### Step 3 — Persist your chosen zero pose

Hold the device level, with the **chip's +X axis pointing toward
your intended "forward" direction** (the silkscreen X/Y/Z arrows
are on the back of the BNO085 breakout). First take a live tare:

```
TARE short-press           ← snapshot this pose as the new zero
```

Then save it so it survives a reboot:

```
TARE long-press 3 s
```

Serial log:

```
I (xxx) imu: software tare: captured (w,i,j,k) = ...
I (xxx) imu: software tare persisted to NVS (survives reboot)
```

This writes your chosen zero-attitude quaternion to the ESP32's NVS.
The BNO085's magnetometer calibration (DCD) is a separate thing —
the fusion engine persists that into BNO internal flash on its own
as it converges. You don't need to trigger DCD save manually.

---

## 3. Daily-use heading reset

Once calibrated, you don't need to factory-reset every time. To
re-align the heading to "the direction I'm currently facing" — just
like the DG-sync button on a real flight instrument — use the short
press:

```
TARE short-press
```

This captures the current corrected attitude as the temporary
software-tare reference. It is useful after moving or re-mounting the
device, but it does not change the safety boundary above.

---

## 4. About automatic magnetometer calibration (the same as your phone)

BNO085's 9-DOF fusion **continuously self-calibrates** while the
device is moving — the engine cross-references magnetometer data
against the gyroscope and accelerometer to estimate and remove
hard-iron / soft-iron offsets. This is identical to the auto-cal in
iOS Compass, Android compass apps, and DJI's gimbal calibration.

The catch:

1. **Fusion only learns while the device is rotating.** Sitting still
   gives the engine no new information.
2. **Magnetic interference matters.** Keep the device away from
   speakers, motors, laptops (yes, including the one running your
   serial monitor), and large metal surfaces while calibrating.
3. **DCD persistence is automatic.** Once the fusion engine reaches
   high accuracy, the BNO085 saves the calibration into its own
   internal flash on its own schedule. You don't trigger this with
   a button — it just happens in the background. (The TARE
   long-press on this device persists your chosen zero pose to the
   ESP32's NVS — a separate thing from BNO's DCD.)

If you move to a magnetically very different location (different
city, indoor → outdoor, near a large new metal structure), the
fusion engine will *update* the calibration in the background
automatically. You only need to do a manual factory reset if it
gets stuck.

---

## 5. Screen modes (cycled by MODE short-press)

| Mode | What it shows |
|------|---------------|
| **PFD** *(default)* | Primary flight display: horizon, pitch ladder, bank arc, heading/HSI, speed tape, barometric altitude/vertical speed, GPS status, and ADS-B count. The HSI overlays forward traffic. |
| **TRAFFIC** | 360-degree traffic radar with heading-up/north-up orientation, 2/5/10/20 NM ranges, relative altitude, and trend arrows. UP/DOWN selects a target and exposes its detail bar. Requires GPS or a manually bound own-ship position. |
| **ADS-B LIST** | Scrollable list of tracked aircraft. Top half: ICAO, callsign, country, altitude, speed, heading, vertical speed, squawk, and type. Bottom half: detail pane for the highlighted row. Short-press **TARE** here to bind the highlighted ICAO as own-ship for PFD ALT / GS / VS. |
| **SETTINGS** | TARE moves through Language, QNH, MAP, RANGE, LOG, and FORMAT SD. UP/DOWN changes the selected value. QNH moves in 0.25 hPa steps; MAP selects heading-up/north-up; RANGE selects 2/5/10/20 NM; LOG selects Flash/MicroSD and takes effect after reboot. FORMAT SD requires a second press within five seconds and refuses while logging to the card. |
| **ABOUT** | Project version, build time, hardware summary, and calibration status. UP/DOWN scrolls. |
| **DIAG** | Live SDR/DSP, BLE, GPS/BeiDou satellite and SNR, system time, BMP388/QNH, MicroSD, active log backend, written-count, and drop-count diagnostics. UP/DOWN scrolls. |
| **COMPASS CAL** *(automatic overlay)* | Figure-8 calibration wizard. It appears automatically when BNO085 accuracy stays at 0 for long enough, exits after convergence, and can be dismissed with MODE. |

Settings values are persisted in NVS. If MicroSD is selected but absent
at boot, the file sink falls back to LittleFS for that boot.

---

## 6. Troubleshooting one-pager

| Symptom | Likely cause | Fix |
|---|---|---|
| Heading stuck at a single value, doesn't respond to rotation | Magnetometer fusion uncalibrated (`acc=0`) | Figure-8 for 15 s, see §2 |
| Heading wrong even after rebooting | Bad DCD persisted to BNO085 flash, or a stale TARE saved to NVS | **TARE very-long-press (10 s)** → factory reset, then re-calibrate (§2) |
| Screen shows uniform pale blue on first boot | LCD wiring fault: a signal line shorted to GND | See `docs/hardware/board_pinout.md` §3 wiring diagram |
| `acc` never climbs past 1 even after lots of motion | Magnetic interference (laptop, phone, speakers, metal table) | Move to a clean environment, retry |
| Heading drifts slowly over minutes | Normal — small DCD nudge from background fusion | Short-press TARE to re-zero |
| TRAFFIC shows `NO OWN POS` | GPS has no fix and no usable manual own-ship binding exists | Move to open sky for a fix, or bind the own aircraft in ADS-B LIST |
| LOG shows `MICROSD (REBOOT)` | File backend is selected only at boot | Keep the card inserted and reboot; a missing card falls back to Flash |
| FORMAT SD shows `IN USE BY LOG` | The active writer is using the MicroSD card | Select Flash, reboot, then format the card |
