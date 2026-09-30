# V4.11 Selective Placement / Mutually Exclusive Parts Reference

Chinese version: [`SELECTIVE_PLACEMENT-zh_CN.md`](SELECTIVE_PLACEMENT-zh_CN.md)

> A number of designators on this board are **not simply "populate everything in the BOM"**: some depend on
> the variant, some are pick-one-of-three, some depend on measurement, and one is normally fitted but must come
> off while measuring the antenna. These rules used to be scattered across `VARIANTS.md`, `ASSEMBLY.md` and the
> schematic; this document consolidates them.
>
> These rules are backed by assertions in the build that produces the fabrication
> files; getting placement wrong fails the build. Docs go stale, assertions don't.

This board's current configuration: **powered variant**.

The airband group (stage G — 22 designators: `U21` `U22` `Y4` `J10` `D6` `L18` `R58`–`R61` `C88` `C89` `C91`–`C100`; `C99`/`R61` are the V4.7 reference-clock DC-block and load, `C100` is the V4.8 VHF VREF decoupling)
is **not** in any selective or mutex group: it is placed unconditionally, on both variants. It is listed here
only so that its absence from the tables below is not read as an omission.

Designator locations on the board: [ASSEMBLY_MAP-V4.11.pdf](ASSEMBLY_MAP-V4.11.pdf) (public version),
`internal/装配图-本轮打板-V4.11-zh_CN.pdf` (parts each board of this batch must hand-place itself).
Both are generated from the criteria in this table, so they cannot disagree with it.

---

## ① Power Variant — Two Groups with Opposite Logic 🔴 Wrong Placement Burns the Board

The same PCB is built in two versions, and **this is the only group in the whole table where a wrong placement burns the board**:

| | Power section (39 designators) | `R7` `R8` (5.1kΩ CC pulldowns) |
|---|---|---|
| **Powered variant** (this board) | ✅ place all | ❌ **must never be placed** |
| Unpowered variant | ❌ place none | ✅ must be placed |

**Power section (the `board_meta.PWR_REFS` set):** `BT1` `C70` `C71` `C72` `C73` `C74`
`C75` `C76` `C77` `C78` `C79` `C80` `C101` `C102` `D7` `D9` `D10` `J9` `L16`
`L17` `R37` `R38` `R39` `R40` `R41` `R42` `R43` `R44` `R45` `R46` `R63` `R65`
`R67` `RT1` `SW3` `U18` `U19` `U20` `U23`.

(`U23`/`C101`/`C102`/`D7`/`R63`/`BT1` are the V4.8 GNSS VBAT backup rail. They all
hang off `BAT_P`, which is produced by `U19` + `J9` — the unpowered variant omits
both, so this group is omitted with them. **Omitting them from this list is not a
"few extra parts" problem**: `U23`'s input would float, the whole backup rail would
be meaningless, and the board would still power up without any error.)

(`D9`/`R65` are the V4.8 **charge indicator**. `D9`'s cathode lands on `CHG_STAT`
through `R65`, and `CHG_STAT` is the `STAT` pin of `U19` (SY6970). The unpowered
variant has no `U19` and therefore no charging at all, so this LED **can never
light up** — populating it is pure waste.
⚠️ **`D8`/`R64` (power indicator) are NOT in this list** and must be populated on
both variants: they sit between `3V3_DIG` and `GND`, and `3V3_DIG` comes from `U1`
stepping down `VCC_5V`, which the unpowered variant still has.)
(`SW3` is the V4.7 power button on SY6970's `/QON`; without `U19` it connects to nothing.)
(`R67`/`D10` are the V4.11 isolated SW3-sense branch to P4 GPIO4. `D10` blocks the
approximately 3.57 V `/QON` pull-up from feeding the 3.3 V GPIO; without `U19`/`SW3`
the branch has no useful source and therefore belongs to the same power-section variant.)

**Why it burns**: for USB-C to be recognized by the host, the CC pin must have Rd. In the powered variant this is provided internally by CH224K(`U18`);
if an external 5.1k is then added in parallel on top of that, **the CC resistance is pulled down → PD negotiation misjudges → it may request
9V/12V into a board designed for 5V**. Conversely, the unpowered variant has no CH224K;
if `R7`/`R8` are also not placed, CC is left floating — **the computer won't recognize it at all, and the RP2040 firmware cannot be flashed**.

> A silkscreen box is printed around `R7`/`R8` on the board, labeled **`NO-PWR VER`** —
> **parts inside the box are placed only on the unpowered variant**.

This group carries two selection/assembly constraints worth calling out:

| Designator | Constraint |
|---|---|
| `L16` `L17` | Power inductors — **sourcing by inductance value alone gets you parts with insufficient Isat**: `L16` needs ≥5A, `L17` needs ≥4A |
| `J9` `RT1` | On the **B side** (`RT1` is an NTC and must sit where it can sense battery temperature) |

---

## ② 1090 Antenna Path — Pick One of Three

The signal from the antenna to the receive chain passes through a two-way switching point; there are three implementations, and **at most one may be placed**:

| Designator | Function | When placed |
|---|---|---|
| **`U16`** | RF switch SPDT (XA17-G4K) | Software switching between external/onboard, **default scheme** |
| `R24` | 0R bypass → fixed to the **external** antenna | Shorts `SW1_J2`↔`SW1_J1`, bypassing the switch |
| `R25` | 0R bypass → fixed to the **onboard IFA** | Shorts `SW1_J3`↔`SW1_J1`, bypassing the switch |

```
                    ┌── SW1_J2 ──[C30]── ANT1090_EXT ── J6 / D2 / L14   external
Receive chain ──[C54]── SW1_J1 ──(U16)
                    └── SW1_J3 ──[C53]── IFA_MATCH ── ZS1/ZP2/J7 ── ANT1  onboard
                         ↑R25 bypass        ↑R24 bypass connects to SW1_J2
```

**Consequences of placing more than one**:
- `R24` + `R25` → **the external antenna and the onboard IFA end up shorted directly in parallel**, impedance completely thrown off
- `U16` + either bypass → one port of the switch is shorted to the common port; antenna switchover fails and the switch may be damaged

**Default scheme: place `U16`; keep `R24`/`R25` as DNP.**
The two bypass positions exist for the "drop the switch, hard-wire one path" variation; normally they stay unpopulated.

---

## ③ GNSS Antenna Path — Switch Only, No Bypass

| Designator | Function |
|---|---|
| **`U17`** | RF switch SPDT, switching between external / built-in patch |
| `J2` | External connector (pigtail to SMA) |
| `J8` | Built-in patch connector |

```
GNSS front end ──[C57]── SW2_J1 ──(U17)──┬── SW2_J2 ──[C58]── J2  external
                                         └── SW2_J3 ──[C59]── J8  built-in patch
```

**Difference from the 1090 side: the GNSS side has no bypass resistor** — without `U17` there is no path at all.
**`U17` must be placed at assembly**; otherwise GNSS receives no signal at all.

**Active-antenna bias:** both branches are active antennas, so each has its own bias tee (the switch's RF port cannot pass DC). The PMOS source is the module's `VCC_RF` (U7.14), not the 3V3_GNSS LDO rail — that is what lets the module sense the antenna current and report `ANTENNA OK/OPEN/SHORT`.

---

## ④ IFA π Matching Network — For Tuning, Not Mutually Exclusive

| Designator | Position | Default | Notes |
|---|---|---|---|
| `ZS1` | Series | **0R** (straight-through) | `ANT1090_IFA` ↔ `IFA_MATCH` |
| `ZP1` | Shunt · antenna side | **DNP** | `ANT1090_IFA` ↔ GND |
| `ZP2` | Shunt · radio side | **DNP** | `IFA_MATCH` ↔ GND |

The three can be combined freely into L / π networks; **values are determined by measurement**, and there is no mutual exclusivity.
The default `ZS1=0R` + `ZP1/ZP2=DNP` is just a straight-through line, **it does not mean the network is tuned**.

Starting point for the first sweep (from HFSS of the six-layer rev2): `ZS1=3.6nH`, `ZP2=3.3pF`, `ZP1=DNP`;
keep 3.6/3.9pF on hand — the final values are set by VNA after enclosure assembly.

---

## ⑤ Conditional downstream isolation when measuring at `J7`

`C53` (100pF) sits in series between `IFA_MATCH` ↔ `SW1_J3`; it is the coupling capacitor from the onboard IFA to the RF switch.

The requirement is that the path after `C53` be electrically open. On the measured V4.0 board `C53` was populated,
but U16 and the other downstream parts were unpopulated, so it did not establish a load path and did not affect the reading.
Only boards with a populated downstream chain need `C53` removed or that path isolated by another method.

---

## ⑥ 0R Jumpers — Placed by Default, Kept as Debug Positions

| Designator | Connects | Purpose |
|---|---|---|
| `R21` | `AD8313_VOUT` ↔ `RF_DET_OUT` | Detector output → divider network; disconnect to test the AD8313 alone |
| `R22` | `ANT_SEL_1090_A` ↔ `V1` | 1090 switch control line; disconnect to set it manually |
| `R23` | `ANT_SEL_1090_B` ↔ `V2` | Same as above |

These three are **placed as 0R by default**; they are not DNP and not mutually exclusive.

---

## ⑦ Designators That Never Had Parts

| Designator | Notes |
|---|---|
| `ANT1` | Onboard IFA — it is just the PCB copper itself |
| `H1`~`H4` | M2.5 mounting holes (⌀2.7mm NPTH) |

All MARK/fiducials (`FID1`~`FID3`) and their keepouts were removed at the user's
request on 2026-09-30. Do not add MARKs to the PCB or generation/rebuild flow
without a new explicit requirement.

---

## These Rules Are Machine-Enforced

Every rule above is backed by an assertion in the build that produces the
fabrication files: populating the wrong variant, fitting mutually exclusive
parts together, or leaving a designator unclassified fails the build outright
rather than quietly emitting a plausible-looking BOM. The BOM and CPL under
`release/` only exist because those checks passed.

So check the physical board against this table and trust it: the table and the
fabrication files come from the same source and cannot disagree.
