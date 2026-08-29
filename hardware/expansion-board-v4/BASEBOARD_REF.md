# Reference Board Mechanical Parameters (Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3)

Chinese version: [`BASEBOARD_REF-zh_CN.md`](BASEBOARD_REF-zh_CN.md)

> Sources: official dimension drawing + mechanical drawing + assembly silkscreen drawing (provided by the user, 2026-08-01).
> **This file is the sole reference for the expansion board outline/connectors/mounting holes**; it predates the PCB layout.

## 1. Board outline and mounting holes

| Item | Value | Source |
|---|---|---|
| Reference board size | **102.50 × 60.00 mm** | dimension drawing annotation |
| **Expansion board takes 100 × 62 mm** | X stays within the free fab tier (≤100mm); +1mm at top and bottom in Y gives the J3 pin header routing room (see §2a) | this project's decision |
| Expansion-to-reference board alignment | mounting holes reuse the 92×50 absolute coordinates → the expansion board is inset 1.25 mm from left and right (symmetric) | 100−92=8, 4 per side; reference 5.25−4=1.25 |
| Mounting holes | **4×M2.5**, hole pitch **92.0 (X) × 50.0 (Y)** | mechanical drawing annotation |
| Top-left hole center | **5.25** from the left edge, **5.00** from the top edge | dimension drawing annotation |
| Derived four-hole coordinates (**reference board coordinate system**, origin top-left, +X right / +Y down) | (5.25, 5.00) (97.25, 5.00) (5.25, 55.00) (97.25, 55.00) | 5.25+92=97.25 and 102.5−97.25=5.25 symmetric ✓; 5+50=55 and 60−55=5 symmetric ✓ |

**The expansion board reuses the same board outline and the same set of mounting holes** (stacked alignment).

## 2. J3 expansion female header (the connector we need to mate with)

| Item | Value | Status |
|---|---|---|
| Specification | 2×20, **2.54 mm pitch**, **SMD female header (socket)** | confirmed on the user's physical part; ⚠️ contradicts the "pin header" wording in `docs/hardware/board_pinout-zh_CN.md:48`; the physical part wins |
| Location | **bottom edge of the board** | confirmed by the dimension/silkscreen drawings |
| Rightmost column pin center X | 102.50 − 19.70 = **82.80 mm** | dimension drawing "19.70" annotation |
| Leftmost column pin center X | 82.80 − 19×2.54 = **34.54 mm** | derived from 20 columns × 2.54 |
| Row-to-row Y pitch | 2.54 mm | standard |
| Bottom margin | **pads ≤0.5 mm from the bottom board edge** (nearly flush with the edge) | user's physical measurement |
| **Absolute Y positions of the two pin rows** | **54.06 / 56.60** (from the top edge) | ✅ confirmed by the user's caliper measurement |
| Socket center | **4.67** from the **bottom** edge → 55.33 from the top edge | user's physical measurement (measured the socket center, not individual pin centers) |
| Plastic body edge | 1.55 from the bottom edge | user's physical measurement |
| **Mated board spacing** | **7.0 mm** (HAT female 3.5 + HAT male 3.5, butted together) | measured comment in `docs/jlc/lcd-4.3in/3d-case/pilot-kit-box-43-base.scad:80` |

> Cross-check of the derivation: back-calculating from "pads ≤0.5mm from the edge" gave 55.4 / pin rows 54.13·56.67,
> which differs from the measured 55.33 / 54.06·56.60 by **only 0.07mm** — the method is credible. **The measured values govern.**

## 2a. +1mm in the board outline's Y direction (adopted)

The expansion board takes **100 × 62** (1mm more than the display board at top and bottom); rationale and safety margin:

| Item | Value | Source |
|---|---|---|
| The case's inner cavity is built from the **glass outline**, not the display-board outline | glass 66.88 vs display board 60, **3.44mm** extra per side in Y | scad:62-68 `glass_ov_y=(glass_w-scr_pcb_w)/2` |
| Conclusion | +1mm per side is far inside the 3.44mm margin, **no interference with the case** | — |
| Benefit | the pin-header pad outer edge is y≈59.43, originally only 0.57mm from the board edge; with +1mm there is **1.57mm**, enough to route 1-2 traces | pads ±4.1mm (KiCad SMD pin-header geometry) |
| Free prototyping | 100×62 is still ≤100mm, the JLCPCB free tier is unchanged | — |

⚠️ But be clear: **this 1mm only fixes "board edge too close"; it does not fix fan-out congestion of fine-pitch ICs** (measured: enlarging the board outline has no effect on the latter).

## 2b. Mating pin header (used on our board)

The user measured the pin header on hand: overall length 51 mm, plastic body 5 mm, SMD leg length 2.85 mm, tip-to-tip across the two rows 7.7 mm.
Cross-checked against the official KiCad `Connector_PinHeader_2.54mm:PinHeader_2x20_P2.54mm_Vertical_SMD`:
40 pads, span along the long edge 48.26 mm (=19×2.54; with the plastic body that is exactly ≈51 mm ✓), pads 3.15×1.0, pad centers at ±2.525.

- **The mating interface is the pins (2.54 pitch / 2.54 between the two rows) — a standard part, mating is risk-free**; the splayed SMD leg dimensions vary slightly
  between manufacturers (user measured 7.7 vs KiCad 8.2), still within the solderable range for the 1mm-wide pads.
- Conclusion: **use the official KiCad SMD pin-header footprint directly**; just re-verify the SMD leg span against the actual part model when ordering.

## 3. Reference board component zones (affects our layout and RF planning)

```
┌──────────────────────────────────────────────────────────┐
│ MIC1  SPK      [SD card]  RESET BOOT POWER   C6-UART     │  top edge
│                                          ┌──────────────┐│
│ USB-C(UART)                              │ ESP32-C6-MINI││ ← 2.4GHz WiFi
│ USB-C(USB)      [DISPLAY FPC]            │ + onboard ant││   module at top-right
│                  ┌──────────────┐        └──────────────┘│
│  Power/DCDC zone │  ESP32-P4     │  [CAMERA FPC]         │
│  (left side)     │  core module  │                       │
│                  └──────────────┘                        │
│ MIC2   BAT   ├─── J3 2×20 SMD female header ───┤  RTC    │  bottom edge
└──────────────────────────────────────────────────────────┘
```

**Three constraints on our expansion board**:
1. **J3 is at the bottom** → our SMD pin header must also be at the bottom (I previously placed it at the top — the whole thing was mirrored by mistake).
2. **The ESP32-C6 2.4GHz WiFi antenna is at the top-right corner** → our 1090 onboard IFA antenna and GNSS antenna **must stay away from the top right**, otherwise the strong near-field emission will desense the 1090 LNA. Suggested: put the IFA on the left edge or at the bottom left.
3. **The P4 core module is in the center** → a digital noise source sits in the middle; our sensitive analog stages (detector, slicer) should avoid sitting directly above it.

## 4. Existing Pilot Kit carrier board (`docs/jlc/lcd-4.3in/`) assets

| Item | Description |
|---|---|
| `1090_MHz_IFA_ANT` | historical onboard IFA shape; the 41mm version measured about 1408MHz, **never tuned to 1090MHz** |
| `IFA_ANT_IPEX` | IPEX receptacle (fallback path for an external antenna) |
| `Z_SER` / `Z_SH1` / `Z_SH2` | the IFA's π matching network (one series, two shunt) — the key to frequency tuning |
| `GPS_GT-U8` / `IMU_BNO085` / `PRES_BMP388` | sensors |
| `RTL-SDR` | external SDR connected over USB (the new board is meant to replace it) |

## 5. Onboard IFA antenna (user confirmed: coordinates don't matter, it can be redesigned)

- The existing design sits at the **top of the board**; the new board keeps the top but **offsets to the left** (x ≈ 8–55),
  avoiding the ESP32-C6 2.4GHz antenna at the reference board's top-right corner (see §3 constraint 2).
- The antenna needs a **copper-free window clear through all four layers**; the +1mm in Y conveniently also gives the antenna keep-out a bit more margin.
- The π matching network (Z_SER series + Z_SH1/Z_SH2 dual shunt) copies the topology as-is; component values get tuned on the real board.

⚠️ The LCSC JSON in the repo is a **stripped-down export** (no pad coordinates / reference-designator geometry); to reuse the IFA's exact trace shape, the user must re-export the full project or provide the antenna shape.

## 6. Antenna plan finalized (2026-08-01, user confirmed the case can be modified)

| Link | Internal | External | RF switch | Notes |
|---|---|---|---|---|
| **1090** | onboard IFA (reuse only the standard inverted-F topology, not the 41mm size), top edge **offset left** | SMA | ✅ XA17-G4K | dimensions set by the v3 HFSS antenna study (internal engineering notes, 52.0mm drawn length / 53.5mm outer envelope); kept away from the C6 WiFi antenna |
| **978** | ❌ not implemented | SMA (or U.FL pigtail) | ❌ not fitted | only 10% away from 1090 in frequency — strong coupling would pull it off; and only US GA uses it. **The entire channel is done as a select-population variant**, saving ¥30 on the domestic version |
| **GNSS** | 25×25×6.3 active ceramic patch, mounted in the case's **top-edge bump-out**, facing the sky, X≈70–95 | SMA | ✅ XA17-G4K | the user has verified this patch gets a good signal; the top-edge bump-out gets implemented when the case design is finalized |

**Rationale for the antenna zoning**: both the IFA and the patch are on the top edge, separated ≥15mm in X to reduce mutual coupling (1090/1575 differ by 45%, an easier case than 1090/978).
The IFA is given the left side because the 1090 LNA is broadband and most vulnerable to the C6's 2.4G near field; the GNSS module has its own SAW filtering and tolerates interference better.

**Both GNSS paths are active antennas** → each branch gets its own bias tee (the switch's RF port cannot pass DC);
the two PMOS are driven complementarily by the same GPIO pair, so power switches together with the RF. Increment ≈¥1.

⚠️ 5mm behind the patch sits the expansion board's solid ground plane, which will affect the radiation pattern and the resonant point → **the V1 case gets an adjustable mounting position; V2 freezes it after measurement**.
