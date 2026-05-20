# Pilot Kit Box — User Guide

A daily-driver guide for the four-button Pilot Kit Box hardware. For
the engineer-facing pinout, GPIO assignments, and SH-2 command
implementation details, see `docs/hardware/board_pinout.md`.

---

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
| **TARE** | Live tare — snapshot current pose as the new "zero" (yaw/roll/pitch all reference it; not saved across reboot) | **Persist** the current tare to NVS so it survives reboot | **Factory reset** — wipe NVS tare + BNO's persisted DCD + reinit the chip. Use this if heading is stuck wrong after a reboot. |
| **MODE** | Cycle screen: **PFD → ADS-B LIST → ABOUT → PFD** | *(reserved — power on/off; currently a TODO log until Phase C lands)* | — |
| **UP**   | Scroll list selection up | *(suppressed — reserved for the combo)* | — |
| **DOWN** | Scroll list selection down | *(suppressed — reserved for the combo)* | — |

### Combo gestures

| Gesture | Action |
|---|---|
| **UP + DOWN held together ≥ 5 s** | Open BLE pairing window. (Flutter side not implemented yet — logs a TODO message.) |

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

This only zeroes `yaw` (heading), leaving `roll` and `pitch` referenced
to gravity. It's the same gesture you'd do mid-flight on a real PFD
when the compass drifts.

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
| **PFD** *(default)* | Primary flight display: sky/ground horizon, pitch ladder, bank arc, heading tape, attitude readout, ADS-B aircraft count. |
| **ADS-B LIST** | Scrollable list of tracked aircraft. Top half: ICAO, callsign, altitude, speed, heading. Bottom half: detail pane for the highlighted row. |
| **ABOUT** | Project version, build time, hardware summary, calibration status. |

Use **UP / DOWN** in `ADS-B LIST` mode to move the highlight.

---

## 6. Troubleshooting one-pager

| Symptom | Likely cause | Fix |
|---|---|---|
| Heading stuck at a single value, doesn't respond to rotation | Magnetometer fusion uncalibrated (`acc=0`) | Figure-8 for 15 s, see §2 |
| Heading wrong even after rebooting | Bad DCD persisted to BNO085 flash, or a stale TARE saved to NVS | **TARE very-long-press (10 s)** → factory reset, then re-calibrate (§2) |
| Screen shows uniform pale blue on first boot | LCD wiring fault: a signal line shorted to GND | See `docs/hardware/board_pinout.md` §3 wiring diagram |
| `acc` never climbs past 1 even after lots of motion | Magnetic interference (laptop, phone, speakers, metal table) | Move to a clean environment, retry |
| Heading drifts slowly over minutes | Normal — small DCD nudge from background fusion | Short-press TARE to re-zero |
