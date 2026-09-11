# PCB Stackup & Impedance Quick Reference (Multi-Vendor · Board-Level General Reference)

Chinese version: [`PCB_STACKUP_IMPEDANCE-zh_CN.md`](PCB_STACKUP_IMPEDANCE-zh_CN.md)

> Data source: first-hand data from each vendor's **official impedance calculator**
> (JLCPCB 2026-08-23; HQPCB and JiePei 2026-09-04).
> This document records only official data and quantities back-solved from it; any formula-derived value is labeled with its source.
> Applies to: `expansion-board-v3` / `v4` and all subsequent boards.
>
> **This file was named `JLC_STACKUP_IMPEDANCE` until 2026-09-04, when it was renamed to be vendor-neutral** —
> this is an open-source project, people will fab it anywhere, and recording only one vendor's parameters is useless.

---

## 0. Four Rules (Read This First)

1. **Outer-layer microstrip impedance is set only by the L1–L2 dielectric thickness h and the trace width** —
   total board thickness and layer count are irrelevant. The same 0.34mm trace is 51.5Ω on a 4-layer board but only 31.8Ω on a 6-layer board.
2. **Take trace widths from the official calculator; never settle a value with the bare microstrip formula.** Hand-calc formulas omit the solder mask, and solder mask raises the
   effective Dk, so formula-derived traces always come out too wide (4.6% too wide at this board's scale).
3. **What you must pin down is the "single-ply 3313" spec, not a particular vendor.**
   Glass styles (3313 / 2116 / 1080 / 7628) are industry standards that every vendor carries;
   what differs is the pressed thickness. **When switching vendors, search by glass style, not by vendor name.**
4. 🔴 **Impedance mismatch costs far less than intuition suggests — do the math before spending money on it.**
   The longest RF trace on this board is 11.7mm ≈ λ/13; a mismatch on such a short line never builds a standing wave.
   A 40% impedance error costs only **0.12dB** — see §5.
   **Do not declare a board unusable just because the impedance does not match.**

---

## 1. 50Ω Outer-Layer Microstrip Trace Width Quick Reference

Outer-layer copper is 1oz. **h = L1–L2 dielectric thickness**.

| Stackup | Layers | Finished thickness | Fee | L1–L2 dielectric | **h (mm)** | **Official 50Ω trace width** |
|---|---|---|---|---|---|---|
| `JLC0216A` | 2 | 1.34~1.64mm | Standard | Core 1.5mm 1/1OZ | **1.4300** | **2.6916mm** |
| `JLC04161H-7628` | 4 | 1.59mm ±10% | Standard | 7628 RC49% 8.6mil | **0.2104** | **0.3586mm** |
| `JLC06161H-3313` | 6 | 1.54mm ±10% | **Free** | 3313 RC57% 4.2mil | **0.0994** | **0.1509mm** |
| `JLC06161H-1080C` | 6 | 1.56mm ±10% | Extra fee | 1080 RC67% 3.3mil | **0.0764** | **0.1072mm** |

All four rows are forward-calculation results given directly by the official calculator (impedance mode: single-ended impedance · outer layer, with L1 as the impedance layer and
L2 as the lower reference layer, impedance tolerance 0.5). The first three rows come from this batch of three screenshots; the 1080C row comes from `51849d0`.

> 🔴 **Erratum (2026-08-23, this update)**: the `JLC06161H-3313` cell previously recorded **≈0.158mm**,
> which was derived from an equivalent Dk of 4.5, not an official value. The official forward calculation gives **0.1509mm**; the derived value is 4.6% too wide.
> See §3 for details.

---

### 1.1 Six-layer 1.6mm: three vendors side by side (measured 2026-09-04)

This board is **6 layers, 1.6mm, 1oz outer, 0.5oz inner**. Every stackup the three vendors offer at that spec:

| Vendor | Stackup | L1–L2 glass | **h (mm)** | Official 50Ω width | This board's 0.15mm gives |
|---|---|---|---|---|---|
| JLCPCB | `JLC06161H-3313` (free) | 3313 ×1 | **0.0994** | 0.1509mm | **50.1Ω** ✅ |
| JiePei | `JP06161H-3313B2` (from ¥200) | 3313 ×1 | **0.091** | 0.136mm | **47.7Ω** ✅ |
| HQPCB | `06161HA3-1080` | 1080 ×1 | 0.077 | 4.26mil | 42.4Ω |
| HQPCB | `06161HA4-1080` | 1080 ×1 | 0.077 | 4.26mil | 42.4Ω |
| HQPCB | `06161HA5-2116` | 2116 ×1 | 0.125 | 7.78mil | 57.0Ω |
| JiePei | `JP06161H-7628A1` (**free**) | 7628 ×1 | 0.186 | ~12.5mil | 70.1Ω |
| HQPCB | `06161HA2-7628H` | 7628H ×1 | 0.230 | 15.68mil | 77.2Ω |

HQPCB/JiePei figures come from their own official calculators. The "0.15mm gives" column was
computed with `tools/microstrip_z0.py` after back-solving Dk from each vendor's official data
point, and cross-checks against HQPCB's calculator (we get 7.78mil for the 2116 case; so does theirs).

> **Two vendors, both "single-ply 3313", and h differs by only 8.5% (0.0994 vs 0.091).**
> That is the basis for Rule 3: search by glass style, not by vendor name.

### 1.2 This board's "stackup window"

With the trace width fixed at 0.15mm, the h range that keeps impedance within ±10% (45–55Ω):

    h = 0.085 ~ 0.115 mm      <- send as-is, no file changes

Outside the window is **not** unbuildable, the impedance just drifts; see §5 for the cost.

    h < 0.085     impedance low  (single-ply 1080, 0.077 -> 42Ω)
    h > 0.115     impedance high (2116 0.125 -> 57Ω; 7628 0.19 -> 70Ω)

---

## 2. Full Layer Structure of the Three Stackups

### 2.1 JLC04161H-7628 (4 layers, standard, finished thickness 1.59mm ±10%)

Inner-layer copper is 0.5oz and outer-layer copper is 1oz; total thickness range **1.43 ~ 1.74mm**.

| Layer | Material | Spec | Thickness (mil) | Thickness (mm) |
|---|---|---|---|---|
| L1 | Copper foil | 1oz | 1.38 | 0.0350 |
| | Prepreg | 7628 RC49% 8.6mil | 8.28 | **0.2104** |
| L2 | Core copper | 1.1mm H/HOZ with copper | 0.60 | 0.0152 |
| | Core dielectric | | 41.93 | 1.0650 |
| L3 | Core copper | | 0.60 | 0.0152 |
| | Prepreg | 7628 RC49% 8.6mil | 8.28 | 0.2104 |
| L4 | Copper foil | 1oz | 1.38 | 0.0350 |

Sum = 1.5862mm ✅ Matches the nominal 1.59mm.

### 2.2 JLC0216A (2 layers, finished thickness 1.6mm class)

Outer-layer copper is 1oz; total thickness range **1.34 ~ 1.64mm**.

| Layer | Material | Spec | Thickness (mil) | Thickness (mm) |
|---|---|---|---|---|
| L1 | Core copper | 1.5mm 1/1OZ with copper | 1.18 | 0.0300 |
| | Core dielectric | | 56.30 | **1.4300** |
| L2 | Core copper | | 1.18 | 0.0300 |

Sum = 1.49mm ✅ Exactly the midpoint of the thickness range.

> ⚠️ On the 2-layer board, the outer copper is the core's own **0.0300mm**, not the 0.0350mm of the other three stackups.
> Don't mix them when back-solving Dk.

### 2.3 JLC06161H-3313 (6 layers, the free stackup, finished thickness 1.54mm ±10%)

**This is the one v3 / v4 currently use.** Inner layers 0.5oz, outer layers 1oz; total thickness range **1.38 ~ 1.69mm**.

| Layer | Material | Spec | Thickness (mil) | Thickness (mm) |
|---|---|---|---|---|
| L1 | Copper foil | 1oz | 1.38 | 0.0350 |
| | Prepreg | 3313 RC57% 4.2mil | 3.91 | **0.0994** |
| L2 | Core copper | 0.55mm H/H without copper | 0.60 | 0.0152 |
| | Core dielectric | | 21.65 | 0.5500 |
| L3 | Core copper | | 0.60 | 0.0152 |
| | Prepreg | 2116 RC54% 4.9mil | 4.28 | 0.1088 |
| L4 | Core copper | 0.55mm H/H without copper | 0.60 | 0.0152 |
| | Core dielectric | | 21.65 | 0.5500 |
| L5 | Core copper | | 0.60 | 0.0152 |
| | Prepreg | 3313 RC57% 4.2mil | 3.91 | 0.0994 |
| L6 | Copper foil | 1oz | 1.38 | 0.0350 |

Sum = 1.5384mm ✅ Matches the nominal 1.54mm.

> This stackup is **symmetric**: both L1–L2 and L5–L6 are 0.0994mm. So a 50Ω microstrip on the bottom layer (L6),
> referencing L5, likewise uses a 0.1509mm trace width.

---

## 3. Equivalent Dk: Why a Single 4.5 Won't Do

The materials' nominal Dk is **4.1**, yet the official calculator behaves as if the Dk were higher — the difference comes from the **solder mask**
(solder mask raises the effective Dk, so the same impedance requires a narrower trace). Back-solving each official data point with the Hammerstad-Jensen bare microstrip formula
(`tools/microstrip_z0.py`):

```
叠层                           h        W     官方Z0     反标定Dk
JLC0216A        2层      1.4300   2.6916  50.0000     4.423
JLC04161H-7628  4层      0.2104   0.3586  50.0000     4.517
JLC06161H-3313  6层      0.0994   0.1509  50.0000     4.725
JLC06161H-3313  6层      0.0994   0.3400  31.8238     4.540
JLC06161H-1080C 6层      0.0764   0.1072  50.0000     4.936
```

**The equivalent Dk is not a constant — it rises as the dielectric gets thinner**: 4.42 → 4.52 → 4.73 → 4.94.

This is physically sound: solder mask thickness is fixed (about 0.02mm), so the thinner h is, the larger a share of the total dielectric the solder mask becomes,
and the higher the equivalent Dk. The same holds within one stackup — on the 6-layer 3313, a wide trace (0.34mm) back-solves to 4.54 while a narrow trace
(0.1509mm) back-solves to 4.73, because a narrow trace's fringe fields travel more through the surface solder mask.

> 🔴 **This overturns the old conclusion that "an equivalent Dk of 4.5 is globally self-consistent."**
> 4.5 is accurate only when h ≥ 0.2mm (0.3% error on the 4-layer). On a thin dielectric with h ≈ 0.1mm, it computes the 50Ω
> trace as 0.158mm while the official value is 0.1509mm — **4.6% too wide**.
> Corrected usage: **back-solve once per stackup, and never extrapolate across stackups.**

### 3.1 How Many Ohms Is the 0.15mm Actually Used on the Board

Using this stackup's own calibration value, Dk = 4.725:

| Trace width | Z₀ | Notes |
|---|---|---|
| **0.1509mm** | **50.0Ω** | Official exact solution |
| **0.15mm (actual board value)** | **≈50.1Ω** | 0.3% off, well within the ±10% tolerance |
| 0.34mm | 31.8Ω | Old 4-layer value; badly mismatched on 6 layers |

**0.15mm is correct — no change needed.** One related correction: the "50Ω → 0.1509mm" written early in the archived v3 antenna study (internal engineering notes)
was the official value all along; it was yesterday's 0.158, derived from Dk 4.5, that was off.

---

## 5. What an Impedance Mismatch Actually Costs (do this math first)

Added 2026-09-04. Before that, this document treated "impedance control" as a hard
constraint **without anyone ever computing how much a mismatch actually costs**, which
turned it into a phantom blocker whenever a vendor change came up.

The longest RF trace on this board is **11.7mm**; at 1090MHz, λ ≈ 152mm on microstrip,
so even the longest run is only **λ/13 (28° electrical length)**. A mismatch on a line
that short never builds a standing wave:

| Trace Z0 | Error | \|Γ\| | Mismatch loss |
|---|---|---|---|
| 42Ω | −16% | 0.087 | **0.033 dB** |
| 47.7Ω | −5% | 0.024 | 0.002 dB |
| 57Ω | +14% | 0.065 | **0.019 dB** |
| 70Ω | +40% | 0.167 | **0.12 dB** |
| 77Ω | +54% | 0.213 | **0.20 dB** |

For comparison, the criterion this project quotes repeatedly is "an SWR of 2.0 costs only
0.51dB of mismatch loss, it has never been the main problem." **Every number above is smaller than that.**

### 5.1 What actually deserves attention

Not transmission-line loss, but the **two devices that are sensitive to termination impedance**:

- **SAW filters** (`FL1`/`FL2`): passband shape, insertion loss and out-of-band rejection all depend on source/load impedance
- **LNA** (`U11`): noise match and stability depend on input impedance

But both are matched by their **surrounding matching components** (`C40`–`C44`, `L9`–`L15`),
not by the characteristic impedance of the trace. The trace only carries the signal from A to B.

### 5.2 🔴 The on-board IFA antenna **is** affected by the laminate — retune when changing fabs

> **This section previously claimed "the IFA antenna is unaffected by the stackup". That was
> wrong, and was disproven by measurement on 2026-09-11.** The old reasoning was "the antenna
> area is cleared on all 6 copper layers, so there is no reference plane and resonance depends
> only on top-layer geometry." **The error: "cleared" meant cleared of *copper*, not of
> *dielectric*.** There is still 1.6mm of FR-4 under the antenna trace, and **the laminate's
> εr sets the resonant frequency directly.**

**Same Gerber, two fabs, measured side by side** (V4.4, only ZS1+J7+J6 populated, probed at J7):

| Fab / stackup | SWR minimum | SWR at minimum | Impedance at board (de-embedded) |
|---|---|---|---|
| JLCPCB `JLC06161H-3313` | **1085.0 MHz** | **1.055** | 51.47 + j2.28 Ω |
| JiePei `JP06161H-7628A1` | **1062.5 MHz** | **1.717** | 39.28 + j21.75 Ω |

**22.5MHz lower (2.07%), and the match degraded too.** Back-solving gives an εeff **4.28%
higher**, consistent with 7628 carrying more glass than 3313 (glass εr≈6.1 vs resin εr≈3.2 —
more glass means higher composite εr).

**In trace length: using the 11.5 MHz/mm slope measured during the trim experiments in
`board_meta.py`, that is equivalent to the antenna being ~2.0mm too long.** Pulling it back
to 1085MHz on the JiePei board would require trimming roughly 2mm more.

> 🔴 **For open-source users: `ARM_OUT = 50.000mm` is tied to JLCPCB's 3313 laminate. It is
> not a universal constant.** Fabbing elsewhere will most likely require retuning the IFA.
> Procedure is in `expansion-board-v4/BOM_IFA_TUNING-zh_CN.md`: trim 0.1–0.2mm at a time,
> use 11.5 MHz/mm to estimate how much, and **judge by VNA measurement in the enclosure**.
>
> Note this is **independent of** the transmission-line mismatch discussed in §5: that
> mismatch costs only ~0.1dB, whereas a 22.5MHz antenna detune takes SWR from 1.06 to 1.72
> (0.55dB mismatch loss) and moves off 1090MHz, which is not a negotiable frequency.

---

## 6. For Open-Source Users: Fabbing This at Another Vendor

This project is open source and can be manufactured anywhere.
**You do not need to source one of the exact stackups listed here.**

**Required** (otherwise the board cannot be built):

    6 layers, 1.6mm finished thickness, 1oz outer copper, 0.5oz inner copper
    Minimum trace/space 5/5 mil, minimum hole 0.30mm
    Layer order: F.Cu / In1(GND) / In2(signal) / In3(3V3) / In4(GND) / B.Cu

**Optional — impedance control** (affects 1090MHz sensitivity, not whether the board works):

    50Ω single-ended on the top layer, L1 referenced to L2. This design derives a
    0.15mm trace width from an L1–L2 dielectric of 0.0994mm (single-ply 3313 prepreg).

    · Your fab offers an L1–L2 of 0.085~0.115mm
      -> Send it as-is, no changes needed.
         Verified: JLCPCB JLC06161H-3313, JiePei JP06161H-3313B2
    · Your fab only offers a thicker dielectric (e.g. single-ply 7628 ~= 0.19mm)
      -> Still manufacturable. RF traces land around 70Ω, mismatch loss ~0.12dB,
         1090 sensitivity drops slightly, everything else is unaffected
    · 🔴 **The on-board IFA antenna must be retuned for your laminate** (see §5.2).
      The 50.0mm copper length is tied to JLCPCB's 3313 laminate, not a universal constant.
      Measured: the same Gerber on JiePei's 7628A1 resonated 22.5MHz lower, SWR 1.06 -> 1.72.
      Procedure: trim 0.1-0.2mm at a time, estimate with 11.5MHz/mm, judge by VNA
      measurement in the enclosure. Ignore this if you only use the external U.FL/SMA antenna.

**Quick guide for users outside China** (details in §8): **no overseas fab's default stackup
lands inside the window**, and that turns out not to matter — **PCBWay (0.1195 / 55Ω / 0.011dB)
and OSH Park (0.1107 / ~57Ω / 0.022dB) can simply be ordered as-is**. The two mainstream European
fabs (Aisler / Multi-CB) both press 0.230mm → 77Ω → 0.20dB: usable but clearly off, and only
Multi-CB can correct it via a custom buildup. 🔴 **ALLPCB's website is suspended — do not use.**

**What will actually ruin the board when changing vendors is not impedance:**

    (1) Layer order: inner layers are identified only by the In1..In4 filenames.
        If a CAM operator reverses them, In2 (signal) and In3 (3V3 plane) swap
        and the board is scrap. In1/In4 are both GND, so swapping those is harmless.
        **Only the In2/In3 pair is fatal.**
    (2) Surface finish: ENIG is mandatory, not HASL. The criterion is pitch,
        not "it's an RF board": U8 is a QFN-56 at 0.4mm pitch with 0.200mm
        pads, plus three LGAs (U4 BNO085, U5 BMP388, U6). HASL coplanarity
        is ±5–15µm and is widely held to be unusable below 0.5mm pitch;
        ENIG sits in the 0.5µm–±2µm range. QFNs have an extra failure mode:
        the solder dome on the large exposed pad lifts the perimeter leads
        off the board.
        ⚠️ Lead-free HASL is *worse* than leaded (SAC305 has higher surface
        tension) — don't pick it just because it's free.
    (3) Slots: J4's four SH holes are **plated slots** 0.60mm wide ×
        1.70/1.20mm long (Excellon G85), not round holes. Exclude them from
        any resin plugging.
    (4) Via-in-pad: 28 thermal vias sit inside paste apertures. Unplugged, they
        are 28 funnels that drain paste during reflow. Either pay for plugging,
        or re-issue an F.Paste that avoids the vias.

Each of these fails far more often than a 0.1dB mismatch ever will. Don't cut them to save money.

**On "via tenting / via opening"**: JLCPCB, HQPCB and JiePei all print the same sentence on
their order pages — **"if the file is Gerber format, we process strictly per the file; this
option has no effect"** — because Gerber cannot distinguish vias from component through-holes.
So via solder-mask state is **entirely determined by the file**. In this project: 437 vias,
401 tented and 36 exposed (all 36 fall inside pad mask openings; 28 of them are thermal
via-in-pad under exposed pads).

---

## 7. Glass Style Is the Cross-Vendor Common Language (surveyed 2026-09-04)

The four prepreg glass styles are IPC standards that **every vendor carries**; only the
pressed thickness differs. Measured from three vendors' official data (6-layer / 1.6mm):

| Glass style | Measured h per vendor (mm) | Source |
|---|---|---|
| **1080** | 0.077 / 0.070 (raw) | HQPCB HA3 / HA4; Eurocircuits PR1080 |
| **3313** | 0.091 / **0.0994** | JiePei 3313B2 / **JLCPCB (free tier)** |
| **2116** | 0.1055 / 0.1088 / 0.1195 / 0.120 (raw) / 0.125 / 0.230 (×2 plies) | PCBWay 30% residual / JLCPCB 4-layer / PCBWay 70% residual / Eurocircuits PR2116 / HQPCB HA5 / **Aisler and Multi-CB** |
| **7628** | 0.1785 / **0.186** / 0.180 (raw) / 0.195 (raw) / 0.2104 / 0.230 | PCBWay 50% residual / **JiePei 7628A1 (free tier)** / Eurocircuits PR7628 / JiePei / JLCPCB 4-layer / HQPCB 7628H |
| FR408HR | 0.1107 | OSH Park (**not FR4**, Dk≈3.66) |

**The same glass style varies 8–24% between vendors** — and 2116 spans 118%, because some
vendors press one ply and others press two. Searching by glass style works, but you still have
to check each vendor's actual pressed thickness **and ask how many plies** — that last part is
the easiest way to get burned.

> Note the two free tiers sit at opposite ends:
> JLCPCB free = 3313 (0.0994), JiePei free = 7628 (0.186).

### 7.1 Tolerance window for each glass style used as the design basis

Computed with `tools/microstrip_z0.py`; criterion is impedance landing in 45–55Ω:

| Basis | Typical h | 50Ω width | h tolerance window | Window width | Covers |
|---|---|---|---|---|---|
| 1080 | 0.077 | 0.108mm | 0.064–0.090 | 0.027 | HQPCB |
| **3313** | 0.095 | **0.142mm** | 0.081–0.111 | 0.030 | JLCPCB + JiePei |
| 2116 | 0.118 | 0.189mm | 0.101–0.136 | 0.035 | JLCPCB 4L + HQPCB |
| **7628** | 0.190 | **0.331mm** | 0.160–0.225 | **0.065** | JiePei free + JLCPCB 4L + HQPCB |

**Relative windows are all ±15–17%, but the absolute window grows with h.**
The 7628 basis gives a 0.065mm window — more than twice the 3313 basis.
**A thicker-dielectric basis is more vendor-portable.**

### 7.2 The current board has no widening headroom (measured)

"One layout, switch trace width per vendor" requires the layout to accommodate wider traces.
**Measured answer: it cannot.**

Method: on a copy of the board, actually widen all 106 RF50 segments, **refill the zones**,
then run `kicad-cli pcb drc` (real DRC, not an estimate):

| RF trace width | Copper violations |
|---|---|
| **0.15mm (current)** | **0** |
| 0.166mm | 0 (theoretical limit) |
| 0.17mm | 3 |
| 0.19mm | 4 |
| 0.25mm | 21 |
| 0.32mm | 24 |
| 0.331mm (true 50Ω on 7628) | 28 |

**Adding 0.02mm already produces violations.** Three blocking spots (measured with KiCad's
collision geometry):

    U11 pad6 (GND) <-> LNA1_OUT   two segments, current gap 0.1580mm (rule 0.15)
    C41 pad2 (GND) <-> SUBG_N3    one segment, current gap 0.1569mm

Total headroom is only 0.008mm, and widening consumes half of it on each side, so the
**width ceiling is ≈0.166mm** — consistent with the table above.

> 🔴 **Correction (2026-09-04):** these two gaps were previously recorded as 0.1430 / 0.1419.
> That was **wrong**, and self-contradictory: a gap below the 0.15mm clearance rule would make
> the current board report violations, yet the current board is clean. Re-measured by bisecting
> `SHAPE::Collide()` for the minimum clearance: 0.1580 / 0.1569. The old values were
> systematically 0.0150mm low. **The conclusion is unchanged** (headroom is still tiny), but the
> width ceiling moves from "0.164" to 0.166mm.

The implementation itself is trivial — width is stored per segment as field 7 of
`internal/tools/ROUTES.json` (`[net, layer, x1,y1, x2,y2, width]`), so a script can rewrite it.
**The bottleneck is the layout, not the tooling.**

At 0.331mm the failure is no longer three spots but **board-wide**: 28 violations spread across
ten components — FL1 / FL2 / U11 / U16 / U14 / U7 / C41 / L1 / D3 / L10 — hitting every stage of
all three chains (1090, 978, GNSS), with the tightest gap squeezed to 0.060mm. The root cause is
that 0.5mm-pitch packages only have 0.22–0.25mm between adjacent pads, while a 0.331mm trace
needs a 0.331 + 0.15 = **0.481mm channel to escape a pin — wider than the pin gap itself**.

> ⚠️ **After changing trace widths or clearances, refill the zones (`ZONE_FILLER.Fill()`)
> before running DRC.** Otherwise the traces are new but the copper pours are stale, and dozens
> of phantom violations appear. Telltale sign: many identical values, each exactly
> `original gap − half the width increase`. Measured: 0.17mm gives 82 violations without a
> refill vs 3 with; 0.331mm gives 112 vs 28.

### 7.3 Proposal for the next revision: dual-basis design

To make this board "one design, any fab", route the next revision against both:

    Basis A (thin)  3313  h~0.095  width 0.142mm  -> JLCPCB free tier, JiePei paid tier
    Basis B (thick) 7628  h~0.190  width 0.331mm  -> JiePei free tier, HQPCB, most overseas fabs

**Draw the RF corridors and keepouts to Basis B's 0.331mm**, and both widths become legal;
switching is then a single parameter at Gerber-export time. Cost: the RF area grows to roughly
twice its current footprint.

---

## 8. Overseas Fabs (surveyed 2026-09-04)

The reality of an open-source project: users abroad will take these Gerbers to their local fab.
Everything below is **first-party data from vendor documentation**, with sources cited. Anything
not obtained is marked "not obtained" rather than filled in from industry experience.

| Fab | Stackup | Glass | **h (mm)** | This board @0.15mm | Mismatch loss | Verdict |
|---|---|---|---|---|---|---|
| **PCBWay** | 6L/1.6mm, **30% residual copper** | 2116 | **0.1055** | **51.8Ω** ✅ | 0.001dB | in window |
| **OSH Park** | 6-Layer (only one) | **FR408HR** | **0.1107** | 53.2 – **57.6Ω** | 0.004–0.022dB | just order it |
| **PCBWay** | 6L/1.6mm, **70% residual** (this board's default) | 2116 | **0.1195** | **55.3Ω** | 0.011dB | just order it |
| **PCBWay** | 6L/1.6mm, 50% residual | 7628 | 0.1785 | 68.5Ω | 0.107dB | usable |
| **Aisler** | 6 Layer 1.6mm 35µm (only one) | **2116 ×2** | **0.230** | **76.8Ω** | 0.198dB | usable, not changeable |
| **Multi-CB** | Defined buildup `6L-01` | **2116 ×2** | **0.230** | **76.8Ω** | 0.198dB | usable, custom available |
| **Multi-CB** | Variable buildup (default) | varies | **not guaranteed** | unpredictable | — | 🔴 not for RF boards |
| **Eurocircuits** | 6L pooled (single predefined) | not obtained | **not obtained** | — | — | ask first |
| **Seeed Fusion** | 6L/1.6mm | not published | **not obtained** | — | — | ask first |
| **NextPCB** | 6L impedance-controlled | stocks 7628/1080/**2313**/2116 | **not obtained** | — | — | ask first |
| **ALLPCB** | — | — | — | — | — | 🔴 site suspended |

Impedances are computed the same way as §1.1 (`tools/microstrip_z0.py`, Dk back-solved from an
official data point, then forward-calculated) — **not** interpolated from h. The Aisler /
Multi-CB rows use **their own published "50Ω needs 395µm"** to back-solve an equivalent Dk of
4.512; HQPCB's independently back-solved Dk at the same h is 4.467, within 1%. Two unrelated
paths corroborating each other.

### 8.1 Three things that need calling out separately

**① PCBWay's h tracks the inner-layer residual copper ratio, and even switches glass style.**
Three stackups exist for the same 6L/1.6mm spec, with L1–L2 from 0.1055 to 0.1785 — nearly
double — and the glass jumping from 2116 to 7628. Layer 2 on this board is a solid ground plane,
so the residual ratio is high and it lands on the **70% tier (0.1195 / 55.3Ω)**. Just order it.
For precision, name the 30% tier in the order notes. Note also that PCBWay's 6-layer boards have
**1oz minimum inner copper — no 0.5oz option**, so the "0.5oz inner" listed under §6 mandatory
items does not hold at PCBWay.

**② OSH Park is not FR4; don't apply this document's Dk values.** The laminate is ISOLA FR408-HR
(Dk≈3.66, versus 4.47–4.73 equivalent Dk for every other vendor here). The thin dielectric pushes
impedance down while the low Dk pushes it up, partially cancelling: 53.2Ω using an FR4 equivalent
Dk, 57.6Ω after scaling by √(4.3/3.66) = 1.084. **The true value lies between, and both ends are
acceptable.** OSH Park offers no impedance control, no custom stackups, and **no assembly** —
bare boards only.

**③ Aisler and Multi-CB have identical 6-layer stackups** (2×2116 = 230µm, εr 4.25, 395µm for
50Ω), per two mutually independent vendor sites. European small-batch fabs evidently share one
FR4 stackup convention — so European users ordering as-is will land around 77Ω / 0.2dB. Only
Multi-CB lets you correct it via a Custom buildup.

### 8.2 Impedance control / custom stackup / assembly

| Fab | Impedance control | Custom stackup | SMT | Notes |
|---|---|---|---|---|
| PCBWay | Yes (±10%, ±5Ω at 50Ω and below) | **Yes** | Yes | 74 standard 6-layer stackups |
| OSH Park | **No** | **No** | **No** | $15/in², 3 copies, 12–21 calendar days |
| Aisler | No formal service, but publishes stackup and 50Ω width | No | Yes | 6L/100×100mm, 12 pcs $360, 13 working days |
| Multi-CB | Yes, **surcharge** | **Yes** (Custom) | Not offered | Defined buildup is **free**; ⚠️ sells to companies/institutions only |
| Eurocircuits | **Dedicated DEFINED IMPEDANCE pool** (from 1 pc, ±10%) | Yes (Special, not priced online) | Yes | The only European option with a proper impedance service |
| Seeed Fusion | Checkbox in the quoting tool | Not on the order page | Yes | 6L/100×100mm, 10 pcs $336.51 |
| NextPCB | Yes (4–20 layers) | **Yes** | Not obtained | Stocks 2313 (note: not 3313) |

### 8.3 What wasn't obtained, and what to ask

Eurocircuits' stackup data lives in their Buildup Editor, which **requires registration**. Their
published material list does bound it, though: only **PR1080 (70µm) / PR2116 (120µm) /
PR7628 (180µm)**, and their own documentation states they "almost always use two prepreg sheets".
**They stock no 3313** and cannot produce the 0.0994mm tier; the thinnest combination,
2×PR1080, is about 0.140mm. (That is derived from the material list, not measured — confirm
before ordering.)

The question to send, valid for all three:

> For 6 layers / 1.6mm / 1oz outer: **what is the pressed dielectric thickness between L1 and L2,
> and which prepreg construction is used (how many plies)?** We need 50Ω single-ended microstrip
> on the outer layer at 0.15mm track width.

---

## 9. Recalculation Tool

```bash
python3 hardware/tools/microstrip_z0.py                    # official data points + back-solved Dk
python3 hardware/tools/microstrip_z0.py 0.15 0.0994 4.725  # W h Dk -> Z0
```

Correct procedure when changing vendors or stackups:

1. **Look up that vendor's L1–L2 thickness first** and check it against the §1.2 window
   (0.085–0.115mm). Inside the window → stop here, no files change.
2. Outside the window → **compute the mismatch cost per §5 before deciding**. A 0.1–0.2dB loss
   is rarely worth re-routing an entire RF section.
3. If you really are changing trace width: **forward-calculate** the 50Ω width once in that
   vendor's own calculator and use that value directly. Don't extrapolate another vendor's Dk.
   To derive other impedances within the same stackup, back-solve Dk from an official data point
   with `solve_er()` and interpolate.
4. After changing trace widths, **grep the entire repo for the old value** — hardcoded copies
   have burned us five times already (see the v3/v4 internal layout-constraint notes).

> 🔴 **The nastiest class of hazard when changing stackups: constants whose provenance is h, but
> which are written down as a number.** They don't fail loudly — they pass silently, because the
> assertion compares against the hardcoded number rather than against h.
>
> One known instance: `NO_CU_FLOOR = 0.30` in `route_ifa_feed.py`, whose comment states its
> origin as **3 × 0.0994 (three dielectric thicknesses on 3313, the microstrip fringing-field
> decay distance)**. On 7628 (h=0.186) the correct floor is **0.558mm**, and the current
> `NO_CU = 0.50` is already insufficient — yet the assertion still passes. The effect is that the
> IFA feedline becomes a semi-coplanar waveguide rather than pure microstrip (ground copper at
> 2.7h instead of 5h).
>
> The direction of that effect is **benign** — coplanar ground pulls Z₀ down, i.e. closer to 50Ω
> — so it is not a reason to avoid ordering. But when spot-checking, don't be suspicious of a
> reading that beats the pure-microstrip model.
>
> **When changing stackups, find every constant whose provenance is a multiple of h and redo it.**

Official calculators:

| Fab | Entry point |
|---|---|
| JLCPCB | <https://tools.jlc.com/#/impedanceCalculation> |
| HQPCB | <https://www.hqpcb.com/quote/impedance> (same tool as the order page's stackup calculator) |
| JiePei | Quote page → "Stackup" → "More laminated structures", open each tier for pressed thickness |

> ⚠️ HQPCB's calculator shows, in the "result" column, the impedance achieved **after** it adjusts
> the trace width — not the impedance of the width you typed. Read the **"trace width"** column
> (the width it says you need for 50Ω). That column is easy to misread.
