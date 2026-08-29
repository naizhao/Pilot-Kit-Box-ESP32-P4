# Pilot Kit Box · 4.3″ Touchscreen UX Specification

Chinese version: [`box-4.3-ux-spec-zh_CN.md`](box-4.3-ux-spec-zh_CN.md)

**Status**: finalized (2026-07-27) · **v2.1 revision 2026-08-02**
**Target firmware branch**: `v4`
**Visual mockup**: [`box-4.3-ux-spec.html`](box-4.3-ux-spec.html) — drawn to true millimeters, opens
directly in the browser, includes a display calibration tool. **Synced to v2.1**; the two agree.
Where they disagree, this file and the code are authoritative.

The layout source of truth lives in the constants section of `firmware/main/nav_grid_page.h` —
this file describes intent; that file holds the numbers.

---

## 1. Background

The pre-migration baseline was a 2.4″ 320×240 SPI panel + four physical buttons; the hardware has
since switched to the Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3 (480×800 native portrait, PPA rotated
270° counter-clockwise = 90° clockwise, used as an **800×480 landscape** display, GT911 capacitive
touch). The controller supports five touch points, but the current firmware reads only the first.

The final target panel is a 5″ 800×480 transflective (same resolution); this spec ports 1:1.

### Product positioning (drives every trade-off)

One device serves two user groups:

| User | Scenario |
|---|---|
| **Hobbyist** | watching nearby aircraft from the ground → recording + attitude in the air → sharing after landing |
| **Pilot** | airborne backup instrument (**covering for a Garmin that dies of heat**) + traffic avoidance + TF-card logging + GDL90 feed to the app |

The shared home page is "PFD + translucent radar overlay along the bottom".

---

## 2. ⚠️ Physical Size Constraints (highest priority)

| Panel | Resolution | Physical size | Pixel density | Actual height of a 16px CJK glyph |
|---|---|---|---|---|
| old 2.4″ | 320×240 | 48.8 × 36.6 mm | 6.56 px/mm · 167 PPI | 2.44 mm |
| **new 4.3″** | 800×480 | **93.7 × 56.2 mm** | **8.54 px/mm · 217 PPI** | 1.87 mm ⚠ |
| final 5″ | 800×480 | 108.9 × 65.3 mm | 7.35 px/mm · 187 PPI | 2.18 mm |

**The 4.3″ landscape panel is roughly the size of a credit card (85.6 × 54 mm).**

> **More resolution buys sharpness, not space.**
> The new panel's 217 PPI is 30% higher than the old 167 PPI — the same pixel count is
> **physically smaller** on the new screen.

### Type scale (hard constraints)

| Level | Physical | 4.3″ | 5″ | Use |
|---|---|---|---|---|
| XL | 5.0 mm | 43 px | 37 px | PFD current values |
| L | 3.5 mm | 30 px | 26 px | Page titles |
| **M** | **3.0 mm** | **26 px** | **22 px** | **Body workhorse** |
| S | 2.5 mm | 21 px | 18 px | Labels / units (**the top status bar uses M**, see §5.1) |
| XS | 2.1 mm | 18 px | 16 px | **hard floor**, extremely secondary use only |

**Any font size below 18 px (2.1 mm) is forbidden.** The old panel's 8×8 CJK glyphs (1.22 mm)
map to a mere 10 px on the new panel — permanently retired.

### Per-page capacity (from the 4.3″ measured glyph heights)

| Page | Capacity |
|---|---|
| ATC board | **7 columns × 7 rows** (type and squawk live in details) |
| Radar right column | **4 cards** |
| Diagnostic cards | **1 core value line per card** (details on a separate page) |
| Settings page | **7 rows fill the screen**; the 8th item requires scrolling |

---

## 3. Interaction Model

### 3.1 Physical buttons: all removed

- deep sleep removed (measured standby power could not be brought down; no practical value)
- all four buttons (TARE/MODE/UP/DOWN) deleted; `button_task.c` retired
- the on-board POWER key is a **hardware power switch** (wired to the power-management IC, not a
  GPIO; unreadable from software)
- **100% touch input at the software level**

GPIOs freed: peripheral needs drop from 10 lines to 6 (IMU INT/RST ×2, BARO_INT ×1, GPS ×3).

### 3.2 FAB + full-screen nav grid

| Top-level pages | Sub-pages |
|---|---|
| FAB = **☰** tap opens the full-screen nav grid | FAB = **←** tap goes back |

#### Why not a horizontal dock

A horizontal dock sliding out from the FAB's side (nav tabs ×94 px | separator | action area
"Level") simply **does not fit** at 800×480:

```
7 tabs × 94 ＋ separator 8 ＋ action area 94 = 761 px
usable width after FAB side margins          = 720 px      → 41 px overflow
```

The 8th item never fits, and the tabs are already down to 94 px — thinner breaks the 44 px touch
floor, and wrapping defeats the form factor itself. Hence the **full-screen nav grid**
(layout source of truth `sim/proto-navgrid/proto.c`). Before proposing "tap the FAB to pop a
horizontal toolbar" again, redo the three-line width arithmetic above.

#### Grid layout (`nav_grid_page.c`)

| Zone | Geometry | Contents |
|---|---|---|
| Top bar | y 0–48, **not masked** | exposes the **underlying page's own** top bar (on PFD that is the §5.1 status bar: satellites / targets / REC / temperature / Bluetooth / battery — status must stay visible while the menu is open) |
| Grid | y 48–360, 4 columns × 2 rows, cells **200 × 156** | each cell: 64 px icon + M-size label |
| Page dots | y 368, 12 px | status indication only, **not tappable** |
| Action bar | y 392–480, **88 px** tall, three equal segments | Level (warning orange) | screen brightness | close |

- **Two pages**: page 1 holds 7 items (PFD / traffic / map / list / search / log / tools) + 1 empty
  cell; page 2 holds 3 items (diagnostics / settings / about). The split point is **frequency of
  use, not filling the page** — the few things you tap most in flight must not be pushed back just
  to fill page 1.
- **Cell positions are pinned; the last page is not centered**: if positions floated with item
  counts, the same function would sit at different coordinates on the two pages and fingers could
  not learn them — and learnability is the grid's entire advantage over a list.
- **Unimplemented "log" and "tools" are greyed placeholders**, not removed from the layout: when
  they ship, not a single layout number changes. Tapping them gives no toast; the greying is the
  message.
- **The underlying page dims by 9%** (not a black overlay): the horizon's light/dark boundary
  stays faintly legible so the pilot keeps an attitude reference during the two seconds the menu
  is open; but the white-on-black altitude/speed tapes become unreadable.
- **FAB hides on open**: the grid fills the screen and its hit-testing precedes LVGL's; a leftover
  FAB would be "something you can't tap that covers a cell", and since it is draggable, which cell
  it covers is unpredictable. The exit is written on the screen (action bar "close").

**Three exits** (no physical buttons; if any one fails, two remain):
① the action bar's "close" ② keep swiping right on page 1 ③ auto-collapse after 6 s idle

> The initial value inherited the dock's 5 s (to preserve the existing feel); after the
> 2026-08-02 hardware walkthrough it became **6 s**: two pages of ten items hold more than the
> dock did, and 5 s was not enough to page through before collapse.
> The source of truth is `NAV_IDLE_MS` in `nav_grid_page.c`; this value can only be tuned on the
> device — do not derive it at a desk.

**Gestures**: swipe left/right to page, threshold 60 px (≈7 mm) with horizontal displacement
> 2× vertical; past the threshold the page flips **immediately**, without waiting for release;
> one long drag can flip several pages in one stroke. "Level" still requires a 1 s long press
> (a short tap only hints).

#### Draggable FAB + position memory (same feel as the Pilot Kit app)

- long-press **200 ms** to enter drag mode (avoids clashing with taps); FAB goes translucent +
  bright edge + heavier shadow (**do not use transform_scale**: it forces LVGL onto the TRANSFORM
  layer, the 64 KB pool cannot allocate a whole buffer, and single-threaded LVGL dies busy-waiting)
- dragging tracks the finger in real time; vertical position is free
- on release it **snaps horizontally to the nearest left/right edge**; lingering mid-screen over
  content is not allowed
- position persists to NVS: `fab_side`(L/R) + `fab_y`(0~100%)
- the grid is full-screen and **does not mirror with the FAB side** (mirroring was a dock-era need)
- the calibration wizard page shows no FAB

### 3.3 Gestures

| Gesture | Effect |
|---|---|
| one-finger vertical swipe | list scrolling (radar right column, ATC board, settings, diagnostics details) |
| one-finger horizontal swipe | nav grid paging (threshold 60 px, horizontal > 2× vertical) |
| swipe right | back from sub-pages; on nav grid page 1 it closes the grid |
| swipe down | close the details drawer |

---

## 4. Navigation Structure and Back Rules

> **No physical buttons = no system-level back.** A sub-page without a designed way back is a
> trap. Every path below must be covered in implementation.

### 4.1 Page classification

| Type | Members | Properties |
|---|---|---|
| **Top-level pages** (7) | PFD / traffic radar / map / ATC board / diagnostics / settings / about | peers; switch only via **FAB → full-screen nav grid** |
| **Modal layer** (full-screen, stacked over some page) | nav grid / search / airport details / keyboard editor | not a stop on `pk_ui_mode_t`; hides the FAB on open; the exit is written on its own screen; precedence decided single-point by `pk_ui_modal_top()` |
| **Sub-pages** (full-screen replacement) | diagnostics → subsystem details ×8 | FAB becomes ←; top bar shows "← Diagnostics"; the FAB is no longer the menu key |
| **Overlays** (do not replace the top level) | target details drawer / format confirmation / calibration wizard / toast | each with its own close + tap-outside-to-close |

> Top-level pages grew from 6 to 7 (the map page), with "log" and "tools" still to come as
> placeholders — a horizontal dock cannot hold that count, which is precisely why §3.2 switched
> to the grid. Adding items to the grid just adds a page; "does not fit" no longer exists.

### 4.2 Back paths

| Where | Way back (any one works) | Returns to |
|---|---|---|
| Top-level page | — (top level) | — |
| Subsystem details page | ① top bar "← Diagnostics" ② FAB (now ←) ③ swipe right | Diagnostics overview |
| Target details drawer | ① "close" ② tap the list area above ③ swipe down | ATC board (selected row kept) |
| Format confirmation dialog | ① "cancel" ② tap the mask ③ **auto-cancel after 5 s timeout** | Settings page |
| Calibration wizard | ① "later" ② auto-exits when acc≥2 holds | the page it was entered from |

**Special case**: "bind as own-ship" in the target details drawer is a **cross-layer jump** — it
closes the drawer and jumps **straight to the PFD** (so the pilot immediately sees the horizon
referenced to the new own-ship), not back to the ATC board.

### 4.3 Principles

- **At most 2 layers, no back stack.** Sub-pages are only reachable from diagnostics; the back
  target is uniquely determined.
- **The FAB position never moves**; the user memorizes exactly one spot — "the round button in
  the bottom-right corner".

---

## 5. Page Specifications

### 5.1 PFD home

```
top bar       48 px   HDG 082° | (sat)17 (ac)6 (●)REC (temp)88°C | (BT) (batt)100%
main area    292 px   speed tape 100 | attitude 600 | altitude tape 100
bottom HSI   140 px   compass + traffic overlay
```

- conversion strip under the speed tape: km/h + mph (carried over from the existing design)
- conversion strip under the altitude tape: VS + m/s (GS/VS no longer get their own corner text)
- **the attitude area is deliberately empty** — no track vector / bank / G meters (on a
  transflective panel, fewer elements read better)

#### Top bar (status bar) — source of truth `pfd_statusbar.c` / `pfd_layout.h`

The early layout was dot + letter abbreviations like `HDG · ●GPS ●REC ●BLE 🔋 · ADSB count`.
**Obsolete**: dots can only say "present/absent", not "how many satellites" or "how many
aircraft", and abbreviations like `ADSB`/`REC` eat 60–90 px per item at 26 px. The middle and
right segments are now fully iconized: icons carry the semantics, numbers carry the reading (an
isolated `100%` could be battery or brightness).

Icons come from **Material Symbols Rounded** (Google, Apache-2.0), baked by
`firmware/scripts/gen_pfd_icons.py` into a **30 × 30 px, 4bpp grayscale** glyph table
(`pfd_icon_font.h`) that shares the **same alpha-blending path as text** (`pk_aa_blit_4bpp`);
edge handling is identical. The FILL axis is chosen per icon: satellite / targets / record /
temperature / Bluetooth use filled (holds up better than outlines at small sizes), **battery uses
outline** (a filled battery cannot show its charge level).

Top-bar text is **M (26 px)**, not S.

| Segment | Slot (left to right) | Icon (Material name) | Contents and color | Data source |
|---|---|---|---|---|
| **left** (persistent) | HDG | none — cyan `HDG` text label | `082°` green; `---°` grey when no heading | `pk_own_heading_resolve()`: ADS-B bound aircraft > IMU yaw > GPS track (≥2 kt) |
| middle | satellites | `satellite_alt` | count green; `NO FIX` **red** when no fix | `pk_gps_get()` |
| middle | ADS-B target count | `connecting_airports` (draws **two** aircraft — the intent is "N around us" plural semantics) | count green | `aircraft_state_snapshot()`, 60 s freshness window |
| middle | record | `fiber_manual_record` | `REC` red, **only while a log is being written** | `rec_active` |
| middle | temperature | `thermometer_alert` (thermometer + exclamation, more apt than a generic triangle) | `88°C` warning orange, **only when over-temperature** | `pk_soc_temp_get()`, see below |
| **right** (persistent) | Bluetooth | `bluetooth` | **icon only, no text** (the symbol speaks for itself), cyan; not drawn at all when disconnected | `ble_gatt_is_connected()` |
| **right** (persistent) | battery | `battery_android_*` | percentage, see below | `pk_batt_get()` |

Geometry (`pfd_layout.h`): bar height 48 · left margin 12 · right margin 16 · icon↔value gap 2 ·
intra-group gap 8 (`PFD_BAR_GAP_LABEL`) · between phrases 16 (`PFD_BAR_GAP_WORD`).

**Degradation rules** (items are dropped whole, not hidden):

- middle-segment priority is **GPS > ADSB > REC > TEMP**, ranked by "how bad is it not to know";
  when space runs out, drop from the lowest-priority end, and **GPS is never dropped**. A 320
  panel fits one middle item, an 800 panel fits four; the rule adapts by itself so nobody
  hand-computes coordinates per panel size.
- **left HDG and right Bluetooth/battery never degrade** — the latter two are "the device itself"
  status, not "the flight" status.
- the right segment reserves **worst-case width** (two icons + `100%`), so the middle segment does
  not shift when Bluetooth disconnects; but the middle segment **centers itself** in the remaining
  space, so it re-centers as a whole when REC / temperature appear or vanish.

**All-empty state** (photo `images/empty-4.3-pfd.png`): `HDG ---°` (grey) · satellite icon +
`NO FIX` (red) · targets icon + `0` (green) · **no** REC / temperature / Bluetooth · battery shown
as usual (the battery is soldered to the board; "not connected" is not a state).

**The battery's two special states**:

- the icon is the `battery_android` family's **alert → _0…_6 → full, a nine-step continuous
  scale**, mapped linearly in one step: `step = (pct×8+50)/100` — **the shape itself is the
  reading**. The previous `battery_horiz_*` had three steps; 20% and 4% rendered as the same empty
  shell distinguishable only by color. The top step uses `full` rather than `_6`: `_6` keeps a
  black notch inside, so as "100%" it would forever look one bar short.
- color: normal **white** (avionics convention: white = normal / informational; green is reserved
  for readings the user should actively confirm as valid: heading, satellites, target count) ·
  `<25%` warning orange · **`≤6%` red, switching to the alert icon at the same time** (the
  threshold is exactly the alert step's coverage on the nine-step scale; turning red and swapping
  the icon happen together, not separately). Photo `images/ui-4.3-battery-low.png`.
- **charging**: plays the `battery_android_frame_1…6 → frame_full` **frame animation** — a static
  bolt only says "plugged in"; animation says "charge is actually going in". 300 ms per frame,
  2.1 s per cycle (in a cockpit, any motion near 1 Hz gets flagged by peripheral vision as an
  alert worth a glance — and charging is the last state that needs attention); phase is computed
  from `uptime_ms` rather than counting rendered frames, so firmware and simulator stay in sync
  despite different frame rates. While charging the color is **always white**: a recovering low
  battery is not an anomaly needing action, and painting it red would manufacture a false alarm.
  Photo `images/ui-4.3-charging.png`.

**Temperature**: reads the **SoC junction temperature** (`soc_temp.c`, 1 Hz sampling), not the
BMP388 that measures cabin ambient — in direct sun the two can differ by 20–30 ℃, and "the device
itself is overheating" is a different matter. Threshold `CONFIG_PK_SOC_TEMP_WARN_C`: currently
**85 ℃ trigger / 78 ℃ clear** (7 ℃ hysteresis, so the slot does not flicker in and out and train
the pilot to ignore it). The 78 ℃ in photos is a mock value that forces the slot lit in the
simulator (`sim/main.c`), not the real trigger point.

**Demo mode**: a 96 × 32 DEMO badge lives at the far right of the top bar (drawn on the LVGL
widget layer); all right-aligned content yields via `pk_ui_topbar_right_limit()`, and the middle
segment yields along with it — the top bar already has a "drop by priority when it does not fit"
mechanism; telling it the new usable width is enough. Photo `images/demo-4.3-pfd.png`.

**This status bar exists only on the PFD page.** Every other full-screen page draws its own top
bar (same 48 px height, title also M): traffic = title + HDG + target count + orientation/range;
ATC board = title + target count + sort description + RESET (only when not the default sort);
map = title + tile zoom `Z12` in the top-right; settings / diagnostics / about have **only a
title in the top-left**; the diagnostics subsystem details page is topped by LVGL's `← Diagnostics`
back bar, with the subsystem name drawn on the first content line
(`diag_page.c :: draw_detail_header`); the compass calibration wizard is a full-screen page with
**no top bar**. The nav grid and toasts stack over the current page, exposing **the underlying
page's own** top bar.

### 5.2 Traffic radar page

- left radar 520 px (range rings 6/13/20 NM, own ship centered), right column 280 px with 4
  scrollable cards
- **own-ship symbol: top-down aircraft silhouette** (yellow, 6.5 mm)
  - heading-up mode: nose always points to the top of the screen
  - north-up mode: rotated by current heading
  - switched by the settings page's "map orientation"
- **target symbols: top-down aircraft silhouettes** (3.8 mm), **each rotated by its own ADS-B
  track** — head-on vs. same-direction must be readable at a glance, which a triangle cannot say
- right-column cards: eight-way bearing arrow + callsign + distance + altitude (with climb rate) + speed
- range toggled by +/− buttons at the radar's bottom-right (**no pinch-zoom**: range has three or
  four steps, nothing that needs pinch continuity, and two-finger gestures are the first to fail
  with gloves or turbulence)

### 5.3 ATC board page

7 columns × 7 rows, row height 48 px, font 21 px, vertically scrollable.

| Col | Bearing | Callsign | Distance | Altitude | V/S | Speed | Heading |

red background = threat. Tap a row → details drawer (230 px tall, 2 board rows stay visible
above); the drawer completes the cut columns: type / SQK / ICAO / airline / country.

### 5.4 Settings page

Row height 64 px, control buttons 38 px tall.

| # | Item | Control |
|---|---|---|
| 1 | language | segmented (中文 / EN) |
| 2 | QNH | stepper (− / value / +) |
| 3 | map orientation | segmented (heading up / north up) |
| 4 | radar range | segmented (10 / 20 / 40) |
| 5 | **screen brightness** | segmented (low / mid / high / auto) **new · required** (was the UP/DOWN keys) |
| 6 | **day/night palette** | segmented **new** (paves the way for the transflective panel) |
| 7 | log storage | segmented (Flash / SD card) |
| 8 | format SD | danger button (**requires scrolling to reach**) |

Formatting keeps the existing **two-step confirmation state machine**: first tap becomes
"tap again to confirm 5s", reset on timeout; greyed when no card or while writing.

### 5.5 Diagnostics page

2 × 4 cards, each 10.8 mm tall, showing only **title + one core value line**.
The eight slots are fixed: IMU / BARO / GPS / SDR / BLE / LOG / CLK / **SYS**.

**The SYS card is new** and includes chip temperature — the product is positioned as "the backup
when a Garmin dies of heat", so its own temperature must be visible. When over-temperature, the
**PFD top bar's middle segment gains a "thermometer icon + junction temp" slot** (warning orange,
not red; in this project red is reserved for "already broken" like `NO FIX` / `REC` / low
battery). Threshold `CONFIG_PK_SOC_TEMP_WARN_C`: currently **85 ℃ trigger / 78 ℃ clear** (see
`soc_temp.c`, 7 ℃ hysteresis).

**Tapping a card opens the subsystem details page**; the details must keep the full depth the
existing `diag_page.c` already has, notably:

- **GPS**: fix/HDOP, satellites in view, antenna status, lat/lon, **per-constellation SNR bars, one
  row each** (the crux of no-fix debugging — do not stare at `fix=0`; look at SNR and the antenna)
- **IMU**: cal N/3 + roll/pitch/yaw + sample rate
- **BARO**: hPa / alt / VS / temperature / QNH / IIR coefficient

### 5.6 About page

**Pure static identity**: logo / version / build time / dependency versions (IDF, LVGL) /
hardware **model** (no online-status lamps) + QR code.

All ✓ statuses, calibration accuracy, and live values **belong to the diagnostics page**; the two
pages share nothing.

### 5.7 Compass calibration wizard

Auto-enter (acc=0 sustained) / auto-exit (acc≥2 sustained), carrying over the existing logic.

**New "later" button** — dismissal previously relied on the MODE key; with the keys gone, without
this button a user in a magnetically disturbed environment would be **permanently stuck on this
page**. Tapping it suppresses auto-open for the rest of the power cycle (reset on reboot).
This page shows no FAB.

> Once "later" disables auto-open, a **manual entry point** must remain,
> otherwise a user who later wants to calibrate has no path (the auto gate only resets after acc
> actually exceeds 2). The entry lives on the **settings page's second-to-last row, "compass
> calibration"**: item name on the left; the value box on the right shows the current accuracy
> `quality n / 3` (so you can judge at a glance whether to tap); tapping enters this page.
> It goes through `pk_ui_cal_wizard_enter()` rather than the generic page-switch function — that
> call also re-arms the auto-open gate, because "the user came to calibrate on purpose" must
> override a previous "later".
> **It is not in the nav grid**: the grid lists pages; calibration is an action.

---

## 6. Terminology: TARE → LEVEL

`TARE` comes from weighing scales ("zero out the container") and means nothing to pilots. The
function is really **caging** (the traditional gyroscope gesture of pulling the compass outward),
and the current implementation only cages roll/pitch with heading decoupled.

**Adopted**: English `LEVEL`, Chinese 「调平」 (Dynon SkyView uses "Level").

### Four-state interaction

| State | Behavior |
|---|---|
| ① idle | the nav grid's action bar left cell shows "Level" (warning orange) |
| ② **short tap** | **no action**; a toast "hold 1 second to level the horizon" appears and disappears after 1.5 s (`UI_TOAST_DURATION_US` in `ui_state.c`; implemented as show/hide, no fade) |
| ③ held | an orange fill sweeps left-to-right over 1 s; releasing or sliding off cancels (the fill vanishes the same frame) |
| ④ done | the cell flashes green for 200 ms (label inverts to the fill color) + confirming toast; the grid closes automatically after the flash |

② is mandatory — otherwise a tap with no reaction reads as a broken button.

**③'s fill color** is `COL_ACT` darkened onto a 35%-strength same-hue ground (`COL_ACT_FILL` in
`nav_grid_page.c`), not a second hue: fill is the background, the orange label the foreground;
only a same-family pair reads as "this cell is filling itself up". This step gives 5.5:1 against
the label and 2.0:1 against the action-bar ground — squeezed from both ends between "the label
must not be swallowed" and "the progress must be visible". **No interpolation/easing** — the grid
runs at roughly 6 FPS when open (measured on hardware), so 1 s is 6 frames; computing the width
from the current time each frame is enough.

**④'s 200 ms** is this panel's floor for "at least one frame preserved, yet not read as a stall".
Green (`COL_OK`) was not in the palette at all: in a cockpit green = normal/confirmed and must
stay far from `COL_ACT`'s orange = attention, so no existing color could be re-brightened into
service. The grid therefore closes 200 ms after Level executes: `nav_grid_page.c`'s existing rule
is "closing always waits for release" (if `s_active` were cleared while the finger is still down,
the remaining frames would fall through to the underlying page, and over the map a fresh tap
would be detected and jump into an airport details page); the green flash pushes the close to the
end of `render()`, by which time the finger is off the glass — no conflict with that rule.

---

## 7. Technical Architecture: the LVGL v9 Migration

### 7.1 Rationale

The new UX needs scrollable lists, a details drawer, segmented controls, steppers, FAB expansion
animation, gesture recognition — in pure hand-drawn framebuffer terms that means writing
inertial scrolling, animation interpolation, and event hit-testing from scratch.

LVGL v9 has all of it built in, and `esp_lvgl_adapter` provides PPA rotation and tear prevention
(exactly the render pipeline being rewritten). The Waveshare BSP and peer-vendor examples are all
LVGL v9.

### 7.2 Reuse of existing code

| Element | LVGL vehicle | Existing code |
|---|---|---|
| PFD attitude | `lv_canvas` | ✅ `pfd_attitude.c` |
| speed / altitude tapes | `lv_canvas` | ✅ `pfd_tape.c` / `pfd_speed_tape.c` |
| HSI + traffic overlay | `lv_canvas` | ✅ `pfd_hsi.c` / `pfd_hsi_traffic.c` |
| traffic radar | `lv_canvas` | ✅ `traffic_page.c` / `traffic_geom.c` |
| GPS SNR bars | `lv_canvas` | ✅ `draw_snr_row()` |
| calibration figure-8 animation | `lv_canvas` + `lv_timer` | ✅ `cal_wizard.c` lemniscate |
| QNH stepper / format confirm / Level / toast | `lv_button` / `lv_msgbox` / `lv_bar` | ✅ all logic reused |
| top-bar status bar | **not LVGL** — drawn straight into the framebuffer | ✅ `pfd_statusbar.c` + `pfd_statusbar_icons.c` |
| FAB / back bar (← Diagnostics) / toast / DEMO badge | LVGL widget tree | new |
| nav grid / search / airport details / keyboard | **not LVGL** — drawn straight into the framebuffer | new (see below) |
| **fonts** | `lv_font_conv` subsets, 5 size steps | ❌ **5 hand-rolled fonts (200 KB) retire** |

**The core PFD drawing assets need zero rewriting** — they already "write pixels into a buffer";
point them at the `lv_canvas` buffer and they work.

> **Why the full-screen modal layers do not use the LVGL object tree** (measured 2026-08-02): one
> full-screen layer needs 800×480×2 = 768 KB of draw surface, while
> `CONFIG_LV_MEM_SIZE_KILOBYTES=64` — the LVGL heap is 64 KB total and cannot allocate such a
> layer; on failure `lv_draw.c` under `LV_OS_NONE` does a **bare busy-wait**, and single-threaded
> LVGL deadlocks on the spot (the long-press-FAB freeze was this same pit).
> So the nav grid / search / airport details / keyboard are all hand-drawn onto the RGB565
> framebuffer directly, and LVGL is reserved for the small widgets: FAB, back bar, toast, DEMO
> badge.

### 7.3 Font system

The early 320×240 era had five coexisting hand-rolled fonts totalling about 200 KB — not a design
choice but a compromise forced by the small panel (the `pfd_font_aa.h` comment says so, citing
"gray antialiasing fringes or TTF hinting artifacts"). They, their generators, and their
renderers have all been deleted; two remain:

- `pfd_font.c` (5×7 ASCII bitmap): reserved for tiny 1–2 character annotations
- `pfd_aa_font.c` (generated by `gen_pfd_aa_font.py`, TTF-derived): one set for CJK+Latin, four
  anti-aliasing steps

- unified `lv_font_conv` generation, 5 steps per the type scale
- **the i18n catalog subsetting mechanism stays** (`i18n_catalog.py` → `gen_i18n_assets.py`);
  new Chinese strings must go through it or they render as `?`
- strings pending addition this round: `调平`、`长按 1 秒调平地平仪`、`稍后再说`、`屏幕亮度`、`配色`、`日间`、`夜间`

---

## 8. Hardware Migration Notes

### 8.1 Pin mapping (40-pin header)

The official Waveshare 4.3 BSP confirmed the key consistencies with the existing hardware:

| Function | 4.3 board | existing P4-WIFI6 | |
|---|---|---|---|
| I2C | GPIO7/8 | GPIO7/8 | ✅ same |
| C6 SDIO | GPIO14-19 | GPIO14-19 | ✅ same |
| C6 reset | GPIO54 | GPIO54 | ✅ same |
| microSD | GPIO39-44 | GPIO39-44 | ✅ same |
| LCD backlight / RST | GPIO26 / GPIO27 | (TARE key / BARO_INT) | ⚠️ **conflict** |

**Peripheral re-wiring** (the header actually breaks out 22 free GPIOs against 6 needed — ample
margin):

```
I2C0 (IMU/BMP388)    SCL/SDA pins      unchanged
IMU INT / RST        GPIO34 / GPIO28
BARO_INT             GPIO31           (GPIO27 is taken by LCD RST)
GPS P4 RX / P4 TX / PPS  GPIO51 / 49 / 50 (P4's view; all post-Rev1.2 4.3″ migration values)
RTL-SDR              header DP/DM      = the P4's dedicated USB 2.0 HS PHY
```

### 8.2 Expansion board

The main board already carries the panel, touch, power management, LiPo interface, RTC, USB, and
microSD. The custom PCB therefore downgrades from "full carrier board" to a **peripheral
expansion board**: GPS + IMU + BMP388 + 1090 MHz IFA antenna + 40-pin socket.

- the main-board side uses an **SMD socket** (2×20 / 2.54 mm / 3.5 mm plastic height) → the
  expansion board uses **male pin headers**
- components placed toward the edges (so the GPS ceramic patch and the IFA are not shadowed by
  the main board and panel)
- **must be locked down with M2.5 standoffs**: an SMD socket's mechanical strength is limited, and
  the device lives in continuous vibration
- fit pins only on the positions actually used, to reduce insertion force

---

## 9. Open Items (architecture unaffected; proceed during implementation)

- **transflective day-mode palette** — the target panel's reflective contrast is only 7:1 with
  16.5% gamut; red/yellow may become hard to tell apart, so threat level may need "shape +
  brightness" encoding and a second palette. **Decide after measuring the real panel.**
- toast styling details / splash screen / hobbyist "ground-watching mode" entry
- the 5″ transflective panel is **TN** (viewing angles θL60/θR50/θU60/θD60 @CR>2); the unit must
  be installed facing the pilot squarely — from the right the angle is only 50°
- the target panel's operating ceiling is 70 ℃; a cockpit in direct sun can exceed it — evaluate
  the wide-temperature variant

---

## Appendix: Preview Calibration

The visual mockup `box-4.3-ux-spec.html` is drawn entirely in **millimeters**. CSS mandates
`1 mm = 3.7795 CSS px` (96 px/inch), independent of the real display's DPI —
**on a high-DPI screen it will render at 38%–75% of true size unless calibrated**.

On first open, calibrate against a credit card (85.6 × 54.0 mm); the result is stored in
`localStorage`. Alternatively fix it via the URL parameter `?cal=2.3` or by editing
`DEFAULT_CAL` in the file.

**Implementation follows the pixel values** (26 px / 48 px / 56 px …); millimeters exist only for
human scale checking.

> Designer's display: 3008 × 1692 logical resolution (recorded 2026-07-27)
