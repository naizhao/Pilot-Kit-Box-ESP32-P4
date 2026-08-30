# V4.2 Onboard 1090MHz IFA Tuning Spares Purchase List
Chinese version: [`BOM_IFA_TUNING-zh_CN.md`](BOM_IFA_TUNING-zh_CN.md)

> Purpose: solely for trimming the v4 onboard 1090MHz IFA, in-enclosure VNA tuning, and matching-network component swaps.
> This list is an independent tuning spares list and does not replace the full-board [`BOM_PURCHASE.md`](BOM_PURCHASE.md).
>
> Current engineering facts: ZP1/ZP2 are 0603 shunt positions, ZS1 is a 0603 series position, J7 is the U.FL tuning port,
> C53 is a 0402 100pF. J7 sits after the π network and before C53; when connecting the VNA, C53 must be left unpopulated
> so the downstream receive chain is kept out of the measurement.

## 1. First, clarify the assembly states

| State | ZP1 (antenna-side shunt) | ZS1 (series) | ZP2 (radio-side shunt) | J7 | C53 | Purpose |
|---|---:|---:|---:|---:|---:|---|
| **Current CAD nominal assembly** | DNP | 0R | DNP | Populated | **100pF** | Antenna feeds the receive chain directly; the antenna cannot be measured independently from J7 |
| **Calibration-board first build / bare-antenna baseline** | DNP | 0R | DNP | Populated | **DNP** | Isolates the receive chain; first trim the antenna open end so that, once in the enclosure, the series resonance lands on 1090MHz, and capture the unmatched impedance |
| **HFSS first-round matching starting point** | DNP | **3.6nH** | **3.3pF** | Populated | **DNP** | A 9-point parameter sweep estimates about 21.15Ω at the current D≈4.988, H=6 and target length of about 50.230mm; the ideal values are about 3.607nH/3.411pF, and this combination serves only as the first component-swap starting point |
| **Final receive state** | Decided by measurement | Decided by measurement | Decided by measurement | May keep | **100pF** | The π network gets the final measured values; after removing the VNA, re-populate C53 to reconnect the receive chain |

> **DNP = not populated.** 3.6nH/3.3pF are not verified production values. The finished enclosure, battery, screws, laminate batch and
> component parasitics all change the final result; the in-enclosure measured result is the final authority.

## 2. Default parts for the first build

| Purchase | Category | Value/Spec | Footprint | Suggested Qty | Ref Des | Supplier/Part No. | Notes |
|---|---|---|---|---:|---|---|---|
| 🔴Must-buy | Default pass-through | 0R jumper resistor | 0603 | 20 | ZS1 | Generic 0603 0R | Used in the baseline state; any generic thick-film 0R with low parasitics and ordinary ratings is fine |
| 🔴Must-buy | HFSS starting point | 3.6nH, RF high-Q inductor | 0603 | 20 | ZS1 | **Murata LQW18AN3N6C00D**; or same-spec Coilcraft 0603HP-3N6XJRW | For this value Murata gives SRF 6GHz, Q(min) 25; ample margin at 1090MHz. When swapping values across the whole set, keep the same series as much as possible |
| 🔴Must-buy | HFSS starting point | 3.3pF, C0G/NP0 | 0603 | 20 | ZP2 | **Murata GRM1885C1H3R3BA01D** | 50V, C0G, ±0.1pF; keep ZP1 DNP in the first round |
| 🔴Must-buy | Tuning interface | 50Ω U.FL board-end receptacle | Footprint compatible with U.FL_Hirose_U.FL-R-SMT-1_Vertical | 3 | J7 | 品赞 (Pinzan) GT-RF1125A-01G **C5299419** (¥0.24); the original Hirose U.FL-R-SMT-1(80) C88374 (¥0.63) also works | 1 for first build, 2 spares for damage; 50Ω, 6GHz (original 8GHz), with 1090/978/1575MHz far inside band |
| 🟢Parts bin | Restoring the receive chain | 100pF | 0402 | 20 | C53 | Any 100pF, **X7R is fine** | DNP during the VNA measurement phase; populate after the matching values are fixed and the measurement cable is removed. **No need to buy C0G**; reasoning below |

> **Why C53 does not need C0G** (aligned with [`BOM_PURCHASE.md`](BOM_PURCHASE.md) on 2026-08-28):
> This list originally marked C53 as "🔴Must-buy C0G", which conflicted with the full-board purchase list's "🟢General, X7R is fine".
> After review, X7R stands — **C53 is a DC-blocking capacitor, not a matching component**; it was swept in by §5's "all matching
> capacitors uniformly C0G" rule. Its impedance at 1090MHz is only
> `1/(2π·1090MHz·100pF) = 1.46Ω`, and even with X7R capacitance drifting ±15% it only reaches 1.7Ω;
> the `BOM_PURCHASE.md` full-path calculation shows a difference of just **0.13dB**.
> Moreover, **C53 is DNP throughout the entire tuning process** (the receive chain must be isolated to measure the antenna alone from J7),
> so it never participates in the matching measurement at all. ZP1/ZP2/ZS1 are the matching positions, and those three must still be C0G/NP0.

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
| 🔴Must-buy | Cutting blades | Pointed hobby knife/scalpel replacement blades | 1 box | To be selected | Trim only 0.1–0.2mm at a time from the IFA open end; after each cut, deburr, clean, reassemble to the fixed position, then re-measure |
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

1. First build: leave ZP1/ZP2/C53 unpopulated, populate ZS1 with 0R and J7 with U.FL; assemble the unit into the enclosure in the production state.
2. Connect the VNA at J7 and observe the series resonance first; trim only the IFA open end so the `Im(Z)` zero crossing lands near 1090MHz.
3. Once the frequency is in place, record the complex impedance at 1090MHz; then start swapping from ZS1=3.6nH, ZP2=3.3pF, ZP1=DNP.
4. Use "close to 50Ω at 1090MHz, stable S11/SWR, and insensitivity to assembly position" as the screening criteria — do not just chase the deepest single dip.
5. After fixing the values, remove the VNA cable, re-populate C53=100pF, re-test in the enclosure, and run an A/B on 1090 message counts, range and noise floor.
6. Write the final antenna length and the measured ZP1/ZS1/ZP2 values back into the next PCB/BOM revision; later batches populate the fixed values directly, but the π positions stay on the board for re-verification after laminate or enclosure changes.
