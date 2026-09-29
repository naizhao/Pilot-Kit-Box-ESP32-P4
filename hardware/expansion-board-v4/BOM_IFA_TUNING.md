# V4.11 Onboard 1090MHz IFA Verification and Tuning Spares List
Chinese version: [`BOM_IFA_TUNING-zh_CN.md`](BOM_IFA_TUNING-zh_CN.md)

> Purpose: verify the measured 50.0mm v4 onboard 1090MHz IFA, and keep optional matching-network spares for enclosure or laminate changes.
> This list is an independent tuning spares list and does not replace the full-board [`BOM_PURCHASE-zh_CN.md`](BOM_PURCHASE-zh_CN.md).
> Antenna-path placement rules: [`SELECTIVE_PLACEMENT.md`](SELECTIVE_PLACEMENT.md) §②/§④/§⑤.
> Laminate-vs-IFA interaction: [`../PCB_STACKUP_IMPEDANCE.md`](../PCB_STACKUP_IMPEDANCE.md) §5.2.
>
> Current engineering facts: ZP1/ZP2 are 0603 shunt positions, ZS1 is a 0603 series position, J7 is the U.FL tuning port,
> C53 is a 0402 100pF. J7 sits after the π network and before C53. The VNA requirement is that the **downstream path be open**,
> not that C53 itself must always be DNP. On the measured V4.0 board C53 was populated while U16 (the 1090 RF switch) and the
> other downstream parts were unpopulated, so no load path existed and C53 did not affect the result.

## 1. First, clarify the assembly states

| State | ZP1 (antenna-side shunt) | ZS1 (series) | ZP2 (radio-side shunt) | J7 | C53 | Purpose |
|---|---:|---:|---:|---:|---:|---|
| **V4.4 nominal / V4.0 measured state** | DNP | **0R** | DNP | Populated | **100pF** | 50.0mm outer envelope; valid J7 isolation on the measured board because U16 and the remaining downstream path were unpopulated/open |
| **Calibration board with populated downstream chain** | DNP | **0R** | DNP | Populated | **DNP or otherwise isolated** | Open the downstream path before measuring the antenna independently; DNP C53 is one isolation method, not an unconditional requirement |
| **Historical HFSS first-round starting point (superseded)** | DNP | 3.6nH | 3.3pF | Populated | Open downstream | Pre-board prediction for a roughly 50.230mm centerline length (HFSS predicted R≈21.15Ω at X=0); retained only as a contingency sweep point, not the V4.4 default |
| **Alternative enclosure/material retune** | Decided by measurement | Decided by measurement | Decided by measurement | May keep | **100pF** | Use only if a new production stack systematically moves away from the V4.0 measured baseline |

> **DNP = not populated.** The production baseline is straight-through, not 3.6nH/3.3pF. The V4.0 antenna trimmed to a
> **50.0mm copper outer envelope / 48.5mm centerline span**, trimmed cut-by-cut from
> 53.5mm and stopped at 50.0mm. In the case with the battery fitted it measured
> **1082.5MHz, SWR 1.09, 45+j0.5Ω**; with the case open (battery fitted over the ground plane) the reading
> drifts over **1080–1090MHz**.
>
> The 50.0mm geometry was re-measured on later boards. Both sessions are recorded as reported:
>
> - **2026-09-04**, first V4.4 board (JLCPCB, out of case, only ZS1+J7+J6): **1090.0MHz**, SWR 1.09,
>   53.52+j2.68Ω after de-embedding the U.FL pigtail (raw 54.09−j0.82Ω).
> - **2026-09-11**, cross-fab batch (JLCPCB, same state): SWR valley **1085.0MHz**, SWR 1.055,
>   51.47+j2.28Ω de-embedded (see the cross-fab section below).
>
> The two sessions differ by ~5MHz; the valley position shifts between measurements and is affected by
> interference, so both are kept as recorded rather than declaring a single true value.

### Measured dF/dL is not one number — read the segment, not the average

The trim series was measured on the same V4.0 board, cut 0.5mm at a time. **The slope is not constant: it is
steep at the start and flattens as the antenna approaches resonance**, so any single "MHz/mm" figure is only
valid over the segment it was measured on.

| Outer envelope | Bare board (no battery) | Battery over ground plane (case off) | In case |
|---|---|---|---|
| 53.5mm | — | — | 995MHz |
| 51.5mm | 1042.5–1045 | 1057.5–1060 | 1047.5MHz |
| 51.0mm | 1056 | 1070–1072.5 | — |
| **50.0mm** | — | 1080–1090 | **1082.5MHz, SWR 1.09, 45+j0.5Ω** |

- **Same-state 51.5→51.0mm: 24.5MHz/mm** (bare, +12.25MHz/0.5mm) and **25.0MHz/mm** (battery over the ground plane, +12.50MHz/0.5mm).
- **51.0→50.0mm spans two different assembly states** (the 51.0mm point was measured bare / battery-on-ground-plane with the case off; the 50.0mm point is in case), so this segment cannot be read as a single-state slope. The previously quoted 11.5MHz/mm came from mixing those two states and is kept only as a rough figure.
- The 53.5→50.0mm full-sequence average is ≈25MHz/mm.

**For a small retune from the 50.0mm operating point, treat the estimate as uncertain**: 11.5MHz/mm is a
rough mixed-state figure, while the same-state measured slope is 24–25MHz/mm. Because the slope dominates
the answer, always confirm with a VNA in the production enclosure, 0.1–0.2mm per cut.

Three other measured effects from the same batch — do not mistake them for a length error:

- **The battery is part of the antenna ground: +15MHz**, consistent across two lengths. Every reading must state whether the battery is fitted, and where.
- **Battery position alone is worth +9MHz** (moved to mid-board lower) — a tuning knob that needs no cutting, and larger than the last trim step.
- **The screen splits the resonance into two peaks ~87MHz apart** (1037.5/1125MHz at 50.0mm) whose midpoint (1081.25MHz) matches the no-screen 1082.5MHz. This is **modal splitting, not detuning**; cutting copper cannot fix it (an SD-socket absorber is the structural fix).

### Cross-fab measurement, 2026-09-11 — the 50.0mm value is tied to JLCPCB's laminate

Two boards, **same Gerber, same U.FL pigtail, same calibration, only the fab changed** (both with only
ZS1(0R) + J7 + J6 populated, downstream open, probed at J7; IFA geometry unchanged V4.4→V4.5, so the data
applies to both):

| | **JLCPCB `JLC06161H-3313`** | **JiePei `JP06161H-7628A1`** |
|---|---|---|
| L1–L2 dielectric | 3313 ×1, h≈0.0994mm | 7628 ×1, h≈0.186mm |
| **SWR-minimum frequency** | **1085.0 MHz** | **1062.5 MHz** |
| **SWR at minimum** | **1.055** | **1.717** |
| Impedance at minimum (with pigtail) | 52.54 + j0.43 Ω | 59.90 + j26.18 Ω |
| Impedance at minimum (**de-embedded**) | **51.47 + j2.28 Ω** | **39.28 + j21.75 Ω** |

**50.0mm reproduces on JLCPCB** — valley 1085.0MHz, SWR 1.055, consistent with the V4.0 in-case
1082.5MHz/SWR 1.09, closing the 53.5→50.0mm trim experiment.

> 🔴 **On JiePei it detunes: 22.5MHz low and SWR degrades from 1.055 to 1.717.** The cause is the laminate's
> εr (back-solving gives εeff **4.28% higher**), **not a fab defect**. See
> [`../PCB_STACKUP_IMPEDANCE.md`](../PCB_STACKUP_IMPEDANCE.md) §5.2 — that section previously claimed "the IFA
> is unaffected by the stackup"; that was wrong, **what is cleared is the copper, not the dielectric**.
>
> To pull it back to 1085MHz on JiePei: the rough 11.5MHz/mm figure gives about **2.0mm**, while the
> same-state measured 24–25MHz/mm gives about **0.9mm**. The slope choice dominates the answer, so trim
> **0.1–0.2mm at a time and confirm with a VNA in the enclosure**.
> **This correction is specific to JiePei's laminate; another fab needs its own measurement.**

**Four traps this batch hit — read before measuring:**

> ⚠️ **Judge the centre frequency by the SWR valley, not by the reading at 1090MHz.** This batch was 5MHz off:
> parking M1 at 1090 read SWR 1.083, while the true valley was at 1085, SWR 1.051.
>
> ⚠️ **Do not use "where the Smith chart crosses the real axis" to judge frequency** — that criterion is
> polluted by the pigtail. The SWR-valley position is not, and is reliable.
>
> ⚠️ **The U.FL pigtail must be de-embedded** (`hardware/tools/vna_deembed.py`). **Measure the open-circuit
> pigtail reading first** as a reference; re-measure after changing cable or re-calibrating. This batch's
> pigtail: 9.984 − j115.2Ω, 0.55dB round-trip. The pigtail not only rotates the impedance; **for a badly
> mismatched load it also makes the SWR look optimistic** (same batch, J6 open: reading SWR 10.8, actual 16.4).
> A proper U.FL SOL calibration board (`hardware/cal-board/`) removes this entirely.
>
> ⚠️ **Out of case ≠ in case.** The table above is a bare-board value. The enclosure pulls the resonance down;
> landing near 1080 in case is normal. **Acceptance is judged in the enclosure.**
>
> 📏 **Measurement repeatability is about 2%** (two readings of the same JLC board's J6 reactance with the
> same pigtail: −j38.50 vs −j39.27). Differences smaller than that are not conclusions.

## 2. Default parts for the first build

| Purchase | Category | Value/Spec | Footprint | Suggested Qty | Ref Des | Supplier/Part No. | Notes |
|---|---|---|---|---:|---|---|---|
| 🔴Must-buy | Default pass-through | 0R jumper resistor | 0603 | 20 | ZS1 | Generic 0603 0R | Used in the baseline state; any generic thick-film 0R with low parasitics and ordinary ratings is fine |
| 🔴Must-buy | HFSS starting point | 3.6nH, RF high-Q inductor | 0603 | 20 | ZS1 | **Murata LQW18AN3N6C00D**; or same-spec Coilcraft 0603HP-3N6XJRW | For this value Murata gives SRF 6GHz, Q(min) 25; ample margin at 1090MHz. When swapping values across the whole set, keep the same series as much as possible |
| 🔴Must-buy | HFSS starting point | 3.3pF, C0G/NP0 | 0603 | 20 | ZP2 | **Murata GRM1885C1H3R3BA01D** | 50V, C0G, ±0.1pF; keep ZP1 DNP in the first round |
| 🔴Must-buy | Tuning interface | 50Ω U.FL board-end receptacle | Footprint compatible with U.FL_Hirose_U.FL-R-SMT-1_Vertical | 3 | J7 | Juxingtai AIPEX-1 **C41432122** (three-pad); original Hirose U.FL-R-SMT-1(80) C88374 also works | 1 for first build and 2 spares. Use a three-pad part; Pinzan C5299419 has four pads and does not match this PCB footprint |
| 🟢Parts bin | Receive-chain DC block | 100pF | 0402 | 20 | C53 | Any 100pF, **X7R is fine** | May remain populated during VNA measurement if the downstream path is already open; otherwise isolate the path. **No need to buy C0G**; reasoning below |

> **Why C53 does not need C0G** (aligned with [`BOM_PURCHASE-zh_CN.md`](BOM_PURCHASE-zh_CN.md) on 2026-08-28):
> This list originally marked C53 as "🔴Must-buy C0G", which conflicted with the full-board purchase list's "🟢General, X7R is fine".
> After review, X7R stands — **C53 is a DC-blocking capacitor, not a matching component**; it was swept in by §5's "all matching
> capacitors uniformly C0G" rule. Its impedance at 1090MHz is only
> `1/(2π·1090MHz·100pF) = 1.46Ω`, and even with X7R capacitance drifting ±15% it only reaches 1.7Ω;
> the `BOM_PURCHASE-zh_CN.md` full-path calculation shows a difference of just **0.13dB**.
> C53 also does not create a termination by itself: with U16 and the remaining downstream parts unpopulated, the line after C53 is open.
> That was the actual V4.0 measurement state. ZP1/ZP2/ZS1 are the matching positions; any capacitor used at ZP1/ZP2 must still be C0G/NP0.

## 3. Matching replacement parts

When purchasing, keep each class of component from **the same vendor and the same RF series** as much as possible. Even with identical nominal values, different series can differ in Q,
SRF and pad parasitics; after changing vendor or series, re-measurement is mandatory.

### 3.1 ZS1 series inductor set

| Purchase | Value Range | Footprint | Suggested Qty per Step | Supplier/Part No. | Mandatory Spec | Notes |
|---|---|---|---:|---|---|---|
| 🔴Core tier | 2.7 / 3.0 / 3.3 / **3.6** / 3.9 / 4.3 / 4.7 / 5.1 / 5.6 / 6.2nH | 0603 | 10 | Prefer the same Murata **LQW18AN_00** series; for 3.6nH see the table above | RF high-Q; SRF≥2.0GHz, ≥2.5GHz preferred; tolerance ≤±5% | Covers the current 3.6nH prediction and adjacent swaps; before ordering each value, still verify SRF/Q against the specific part number |
| 🟡Extended low tier | 1.0 / 1.2 / 1.5 / 1.8 / 2.2nH | 0603 | 10 | To be selected | Same as above | Used when measurements call for only a smaller series reactance |
| 🟡Extended high tier | 6.8 / 7.5 / 8.2 / 9.1 / 10nH | 0603 | 10 | To be selected | Same as above; verify SRF value by value — listings that say only "RF inductor" without curves/parameters are not accepted | Expand the search when the measured impedance deviates significantly from HFSS |

> Blind-buying up to 30nH is not advised: the larger the inductance, the lower the SRF usually is. Near or past SRF around 1090MHz, the device no longer
> behaves as an inductor. If values above 10nH are truly needed, first verify that the specific part number is still inductive at 1090MHz with sufficient Q, then buy it separately.

### 3.2 ZP1/ZP2 shunt capacitor set

| Purchase | Value Range | Footprint | Suggested Qty per Step | Supplier/Part No. | Mandatory Spec | Notes |
|---|---|---|---:|---|---|---|
| 🔴Core tier | 2.2 / 2.4 / 2.7 / 3.0 / **3.3** / 3.6 / 3.9 / 4.3 / 4.7 / 5.1pF | 0603 | 20 | Prefer the same Murata **GRM1885C1H…BA01D** series; for 3.3pF see the table above | **C0G/NP0**; tolerance ±0.1pF or ±5% preferred; rated voltage ≥25V | Covers the 3.411pF ideal value and adjacent swaps; ZP1 and ZP2 can share; the full part number differs per capacitance value and must be confirmed item by item against the official selection table |
| 🟡Extended low tier | 0.5 / 0.6 / 0.7 / 0.8 / 1.0 / 1.2 / 1.5 / 1.8pF | 0603 | 20 | To be selected | **C0G/NP0**; ±0.1pF preferred | Parasitics dominate in the small-capacitance tiers, so keeping the series consistent is essential |
| 🟡Extended high tier | 5.6 / 6.2 / 6.8 / 7.5 / 8.2 / 9.1 / 10 / 12pF | 0603 | 20 | To be selected | **C0G/NP0**; tolerance ≤±5%; rated voltage ≥25V | Expand the search when measurements deviate significantly from HFSS |

## 4. Test connectors and consumables

| Purchase | Category | Spec | Suggested Qty | Supplier/Part No. | Purpose/Cautions |
|---|---|---|---:|---|---|
| 🔴Must-buy | VNA cable | 50Ω U.FL plug → SMA male, short flexible coax | 3 | To be selected | 1 for measuring, 2 spares for damage; once a length is chosen, stick to the same cable — repeated bending changes readings |
| 🟡Advised | SMA adapter/extension | 50Ω SMA male-female adapters or flexible test leads | 1 set | To be selected | Reduces stress on the NanoVNA port; once used, fold the calibration plane/port extension into the measurement procedure |
| 🔴Must-buy | Flux | No-clean, suitable for 0603/0402 rework | 1 pc | To be selected | Reduces pad damage from repeated component swaps; after tuning, clean residues and re-measure |
| 🔴Must-buy | Solder wire | Fine solder, about 0.3mm | 1 spool | To be selected | For hand-swapping 0603/0402 parts and copper-foil restoration experiments |
| 🔴Must-buy | Solder wick | About 0.8–1.0mm | 1 spool | To be selected | Clean the ZP1/ZP2/ZS1/C53 pads; avoid solder buildup changing RF parasitics |
| 🟡Service only | Cutting blades | Pointed hobby knife/scalpel replacement blades | 1 box | To be selected | V4.4 already contains the measured 50.0mm geometry; cut only during a controlled retune for a changed enclosure/material, 0.1–0.2mm at a time |
| 🟡Advised | Copper foil restoration stock | Adhesive-free bare copper foil, about 0.03–0.05mm thick, width ≥1.5mm | 1 sheet | To be selected | Only for experimental restoration after over-cutting and for confirming direction; the final production length must be written back into the PCB copper — attached foil is not a production solution |
| 🟡Advised | Assembly consumables | Insulating tape/foam/screws identical to production | 1 set/prototype | To be selected | In-enclosure placement and metal screws change the resonance; tuning must use the same assembly state as production |

## 5. Ordering and storage requirements

1. 0603 inductors and 0603 capacitors must keep their original tape and labels sorted by value; splitting them into unlabeled loose mixes is forbidden.
2. Before ordering each inductor, check its datasheet one by one: 0603 footprint, still inductive at 1090MHz, SRF meeting this table's requirements, plus a Q or impedance curve.
3. Capacitors for the **matching positions** (ZP1/ZP2) are purchased uniformly as C0G/NP0; do not substitute X5R/X7R.
   ⚠️ This rule covers ZP1/ZP2 only — `C53` is DC blocking, not matching, so X7R is fine (see the note under the §2 table).
4. Order the "core tiers" first; buy the extended tiers if the budget allows. For a single prototype, the core tiers already cover the main swap range around the current HFSS prediction.
5. Store parts in compartmented anti-static sample boxes labeled by "component type + nominal value + vendor series" to prevent confusing 3.3pF/3.3nH.

Verified model basis: Murata `LQW18AN3N6C00D` is 1608/0603, 3.6nH, SRF 6GHz;
Murata `GRM1885C1H3R3BA01D` is 0603, 3.3pF, 50V, C0G, ±0.1pF;
(the Murata `GRM1555C1H101JA01D` originally listed for `C53` has been changed to "any 100pF X7R", see §2);
Hirose `U.FL-R-SMT-1(80)` is a 50Ω vertical SMT receptacle rated to 8GHz. If a purchasing platform's listing title conflicts with these parameters,
defer to the manufacturer's full part number and datasheet, not the abbreviated product name.

## 6. Shortest tuning sequence

1. Verify the manufactured copper outer envelope is 50.0mm; fit ZS1=0R with ZP1/ZP2 DNP, and populate J7 for the VNA.
2. Confirm that the receive path after C53 is open. C53 may remain populated when the downstream parts (U16 etc.) are absent; otherwise isolate the path.
3. **Measure the open-circuit pigtail reading first** (nothing connected at the U.FL end) and record R/X — this is the de-embed reference; re-measure after changing cable or re-calibrating.
4. Measure at J7 first out of the case, then in the complete production enclosure including the display, TF-card socket, battery and screws.
5. **Sweep and find the SWR valley**; record the valley frequency and the complex impedance there, and compare with the baselines above. Run the impedance through `hardware/tools/vna_deembed.py` to remove the pigtail. **Do not read at 1090MHz as the centre frequency** (it can be several MHz off), and do not use "where the Smith chart crosses the real axis" (polluted by the pigtail). Do not cut copper or fit the historical HFSS values merely to deepen a single dip.
6. Only when a changed enclosure/material causes a repeatable batch-level shift, retune in controlled 0.1–0.2mm steps and use the π network as needed. **Changing fab counts as a material change** — for JiePei 7628A1 the rough 11.5MHz/mm gives ≈2.0mm and the same-state 24–25MHz/mm gives ≈0.9mm; the slope dominates, so cut 0.1–0.2mm at a time and judge by the in-enclosure VNA.
7. Run an over-the-air A/B on 1090 message count, range and noise floor; VNA matching alone does not verify radiation efficiency.
