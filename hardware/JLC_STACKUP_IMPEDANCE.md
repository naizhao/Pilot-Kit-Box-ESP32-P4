# JLC Stackup & Impedance Quick Reference (Board-Level General Reference)

Chinese version: [`JLC_STACKUP_IMPEDANCE-zh_CN.md`](JLC_STACKUP_IMPEDANCE-zh_CN.md)

> Data source: official screenshots of the **JLC impedance calculator**, provided by the project maintainer on 2026-08-23.
> This document records only **official first-hand data** and the quantities back-solved from it; any formula-derived value is always labeled with its source.
> Applies to: `expansion-board-v3` / `v4` and all subsequent boards. For antenna-specific discussion, see
> §3 of the archived v3 antenna study (internal engineering notes).

---

## 0. Three Rules (Read This First)

1. **Changing the stackup requires recalculating every impedance trace width.** The outer-layer microstrip impedance is determined only by the **L1–L2 dielectric thickness h** and the trace width,
   total board thickness and layer count are irrelevant. The same 0.34mm trace is 51.5Ω on a 4-layer board but only 31.8Ω on a 6-layer board.
2. **Take trace widths from the official calculator; never settle a value with the bare microstrip formula.** Hand-calc formulas omit the solder mask, and solder mask raises the
   effective Dk, so formula-derived traces always come out too wide (4.6% too wide at this board's scale).
3. **Always tick "Impedance Control" when ordering.** On a 6-layer board, 50Ω needs only 0.15mm — a fine trace; without that option, trace width tolerance eats the
   impedance precision directly.

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

## 4. Recalculation Tool

```bash
python3 hardware/tools/microstrip_z0.py                    # 官方数据点 + 反标定 Dk
python3 hardware/tools/microstrip_z0.py 0.15 0.0994 4.725  # W h Dk -> Z0
```

Correct procedure when changing stackups:

1. In the [JLC impedance calculator](https://tools.jlc.com/#/impedanceCalculation), select the new stackup and
   **forward-calculate** the 50Ω trace width once — use that value directly.
2. To derive other impedances within the same stackup (differential pairs, other trace widths), first back-solve the Dk from the step-1 data point
   with `solve_er()`, then interpolate using that Dk.
3. After changing trace widths, **grep the entire repo for the old value** — hardcoded copies have burned us four times already
   (see the archived v3/v4 layout constraints, internal engineering notes).
