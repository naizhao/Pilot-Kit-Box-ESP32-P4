# Pilot Kit Box — User Guide

A daily-driver guide for the four-button Pilot Kit Box hardware. For
the engineer-facing pinout, GPIO assignments, and SH-2 command
implementation details, see `docs/hardware/board_pinout.md`.

---

## 1. Buttons at a glance

The board has **four physical buttons** on the right-side header,
plus one **combo gesture**:

```
       TARE   MODE    UP    DOWN
       (26)   (27)   (22)   (23)
        |      |      |      |
        ●      ●      ●      ●
```

| Button | Short press (released < 3 s) | Long press (held ≥ 3 s) |
|--------|------------------------------|--------------------------|
| **TARE** | Reset heading to 0° (`yaw` only — roll/pitch stay referenced to gravity) | Persist current calibration to BNO085 flash. *Refused when `acc < 2`* — calibrate first. |
| **MODE** | Cycle screen: **PFD → ADS-B LIST → ABOUT → PFD** | **Factory reset** the IMU (clear tare + clear DCD + reinit). Use this if heading is stuck wrong. |
| **UP**   | Scroll list selection up | *(no single-button long press — reserved for the combo)* |
| **DOWN** | Scroll list selection down | *(no single-button long press — reserved for the combo)* |

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
MODE long-press 3 s
```

You'll see in the serial log:

```
W (xxx) imu: factory reset: clear tare + clear DCD + reinit BNO085
            — fusion engine restarts from scratch
I (xxx) imu: factory reset complete — start figure-8 motion to let
            BNO085 re-learn magnetometer calibration
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

### Step 3 — Persist the clean calibration

Hold the device level, with the **chip's +X axis pointing toward
your intended "forward" direction** (the silkscreen X/Y/Z arrows
are on the back of the BNO085 breakout):

```
TARE long-press 3 s
```

Serial log:

```
I (xxx) imu: full reorient (acc=2): tare(XYZ) + persist + save DCD
```

This writes the clean calibration **into BNO085 internal flash**, so
the next power-cycle starts already calibrated — you don't need to
repeat figure-8 every time.

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
3. **Once `acc=3` is reached and persisted to flash via TARE
   long-press**, the calibration survives power-cycles. Next boot
   starts at high accuracy directly.

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
| Heading wrong even after rebooting | Bad DCD persisted to BNO085 flash | MODE long-press → factory reset, then re-calibrate (§2) |
| TARE long-press refused with `accuracy < 2` log | (intended) — guard against poisoning flash again | Calibrate first (§2 step 2), retry |
| Screen shows uniform pale blue on first boot | LCD wiring fault: a signal line shorted to GND | See `docs/hardware/board_pinout.md` §3 wiring diagram |
| `acc` never climbs past 1 even after lots of motion | Magnetic interference (laptop, phone, speakers, metal table) | Move to a clean environment, retry |
| Heading drifts slowly over minutes | Normal — small DCD nudge from background fusion | Short-press TARE to re-zero |
