# Baseboard Mechanical Reference (Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3)

Chinese version: [`BASEBOARD_REF-zh_CN.md`](BASEBOARD_REF-zh_CN.md)

> Source: official dimension drawing + mechanical drawing + assembly silkscreen drawing (provided by the user on 2026-08-01).
> **This file is the single source of truth for the expansion board's board outline/connectors/mounting holes**, and predates the PCB layout.

## 1. Board outline and mounting holes

| Item | Value | Source |
|---|---|---|
| Baseboard dimensions | **102.50 × 60.00 mm** | dimension drawing annotation |
| **Expansion board is 100 × 62 mm** | X stays in the free fabrication tier (≤100mm); Y gets +1mm top and bottom to give the J3 pin header trace routing room (see §2a) | this project's decision |
| Expansion-to-baseboard alignment | mounting holes keep the 92×50 absolute coordinates → expansion board inset 1.25 mm on each side, left/right (symmetric) | 100−92=8, 4 per side; base 5.25−4=1.25 |
| Mounting holes | **4×M2.5**, hole spacing **92.0 (X) × 50.0 (Y)** | mechanical drawing annotation |
| Top-left hole center | **5.25** from the left edge, **5.00** from the top edge | dimension drawing annotation |
| Derived four-hole coordinates (**baseboard coordinate system**, origin top-left, +X right / +Y down) | (5.25, 5.00) (97.25, 5.00) (5.25, 55.00) (97.25, 55.00) | 5.25+92=97.25 symmetric with 102.5−97.25=5.25 ✓; 5+50=55 symmetric with 60−55=5 ✓ |

**The expansion board reuses the same board outline and the same set of mounting holes** (stacked alignment).

## 2. J3 expansion female header (the connector we mate with)

| Item | Value | Status |
|---|---|---|
| Specification | 2×20, **2.54 mm pitch**, **SMD female header (socket)** | confirmed against the physical part; ⚠️ contradicts the "pin header" written in `docs/hardware/board_pinout-zh_CN.md:48` — the physical part wins |
| Position | **bottom edge of the board** | confirmed by dimension/silkscreen drawings |
| Rightmost column pin center X | 102.50 − 19.70 = **82.80 mm** | dimension drawing "19.70" annotation |
| Leftmost column pin center X | 82.80 − 19×2.54 = **34.54 mm** | derived from 20 columns × 2.54 |
| Row-to-row Y spacing | 2.54 mm | standard |
| Bottom clearance | **pads ≤0.5 mm from the board's bottom edge** (nearly flush with the edge) | user-measured |
| **Absolute Y of the two pin rows** | **54.06 / 56.60** (from the top edge) | ✅ confirmed by the user with calipers |
| Socket center | **4.67** from the **bottom** edge → 55.33 from the top edge | user-measured (measured at the socket center, not individual pin centers) |
| Plastic body edge | 1.55 from the bottom edge | user-measured |
| **Mating board spacing** | **7.0 mm** (HAT female 3.5 + HAT male 3.5 butted together) | measured note in `docs/jlc/lcd-4.3in/3d-case/pilot-kit-box-43-base.scad:80` |

> Cross-check of the derivation: working backwards from "pads ≤0.5mm from the edge" gave 55.4 / pin rows 54.13·56.67,
> versus the measured 55.33 / 54.06·56.60 — **only 0.07mm apart**, so the method is trustworthy. **The measured values take precedence.**

## 2a. Board outline +1mm in Y (adopted)

The expansion board is **100 × 62** (1mm more than the display board on each of top and bottom); rationale and safety margin:

| Item | Value | Source |
|---|---|---|
| The case's inner cavity is built to the **glass outline**, not the display-board outline | glass 66.88 vs display board 60, **3.44mm** extra per side in Y | scad:62-68 `glass_ov_y=(glass_w-scr_pcb_w)/2` |
| Conclusion | +1mm per side is well within the 3.44mm margin, **no interference with the case** | — |
| Benefit | the pin header pads' outer edge is at y≈59.43, originally only 0.57mm from the board edge; after +1mm there is **1.57mm**, enough to route 1-2 traces | pads ±4.1mm (KiCad SMD pin header geometry) |
| Free prototyping | 100×62 is still ≤100mm, the JLCPCB free tier is unchanged | — |

⚠️ But be clear: **this 1mm only fixes "board edge too close", it does not fix fine-pitch IC fan-out congestion** (measured in practice: enlarging the board outline has no effect on the latter).

## 2b. Mating pin header (used on our board)

The user measured the pin header at hand: overall length 51 mm, plastic body 5 mm, SMD leg length 2.85 mm, tip-to-tip across the two rows 7.7 mm.
Checked against the official KiCad `Connector_PinHeader_2.54mm:PinHeader_2x20_P2.54mm_Vertical_SMD`:
40 pads, span along the long edge 48.26 mm (=19×2.54; including the plastic body it is exactly ≈51 mm ✓), pads 3.15×1.0, pad center offset ±2.525.

- **The mating interface is the pins (2.54 pitch / 2.54 row spacing), a standard part, no risk in mating**; the outward splay of the SMD legs varies slightly between manufacturers
  (user measured 7.7 vs KiCad 8.2), which is within the solderable range for 1mm-wide pads.
- Conclusion: **use the official KiCad SMD pin header footprint directly**; when ordering, just re-verify the SMD leg span against the actual part.

## 3. Baseboard component zones (affects our layout and RF planning)

```
┌──────────────────────────────────────────────────────────┐
│ MIC1  SPK      [SD card]   RESET BOOT POWER   C6-UART    │  top edge
│                                          ┌──────────────┐│
│ USB-C(UART)                              │ ESP32-C6-MINI││ ← 2.4GHz WiFi
│ USB-C(USB)      [DISPLAY FPC]            │ + on-board   ││   module at
│                  ┌──────────────┐        │   antenna    ││   top-right
│  Power/DCDC zone │  ESP32-P4    │  [CAMERA FPC]         │
│  (left side)     │  core module │                       │
│                  └──────────────┘                        │
│ MIC2   BAT   ├─── J3 2×20 SMD female header ───┤   RTC   │  bottom edge
└──────────────────────────────────────────────────────────┘
```

**Three constraints on our expansion board**:
1. **J3 is on the bottom edge** → our SMD pin header must also be on the bottom edge (I had placed it on the top edge earlier — an overall mirrored placement, wrong).
2. **The ESP32-C6's 2.4GHz WiFi antenna is at the top-right corner** → our 1090 on-board IFA antenna and GNSS antenna **must stay far away from the top-right**, otherwise the strong near-field transmission will desense the 1090 LNA. Recommend placing the IFA on the left edge or bottom-left.
3. **The P4 core module is in the center** → a digital noise source sits in the middle; keep our sensitive analog sections (detector, slicer) away from directly above it as much as possible.

## 4. Existing assets of the current Pilot Kit carrier board (`docs/jlc/lcd-4.3in/`)

| Item | Description |
|---|---|
| `1090_MHz_IFA_ANT` | historical on-board IFA geometry; the 41mm version measured ≈1408MHz, **it was never tuned to 1090MHz** |
| `IFA_ANT_IPEX` | IPEX receptacle (alternative path for an external antenna) |
| `Z_SER` / `Z_SH1` / `Z_SH2` | the IFA's π matching network (one series, two shunts), the key to tuning the frequency |
| `GPS_GT-U8` / `IMU_BNO085` / `PRES_BMP388` | sensors |
| `RTL-SDR` | external SDR connected via USB (the new board will replace it) |

## 5. On-board IFA antenna (user confirmed: exact coordinates don't matter, it can be redesigned)

- The existing design sits at the **top of the board**; the new board keeps it at the top but **offset to the left** (x ≈ 8–55),
  away from the ESP32-C6 2.4GHz antenna at the baseboard's top-right corner (see §3 constraint 2).
- The antenna needs a **copper-free window clear through all four layers**; the +1mm in Y also conveniently gives the antenna keep-out a little more margin.
- The π matching network (Z_SER series + Z_SH1/Z_SH2 dual shunt) copies the topology as-is; component values are tuned on the actual board.

⚠️ The JLC/LCEDA JSON in the repo is a **stripped-down export** (no pad coordinates / designator geometry). To reuse the IFA's exact trace shape, the user needs to re-export the full project or provide the antenna geometry.

## 6. Antenna plan finalized (2026-08-01, user confirmed the enclosure can be changed)

| Link | Internal | External | RF switch | Notes |
|---|---|---|---|---|
| **1090** | on-board IFA (reuses only the standard inverted-F topology, not the 41mm dimensions), top edge **offset left** | SMA | ✅ XA17-G4K | preliminary dimension study in the archived v3 HFSS antenna study (internal engineering notes); keep away from the C6 WiFi antenna |
| **978** | ❌ not done | SMA (or U.FL pigtail) | ❌ not fitted | only 10% from 1090 in frequency, would strongly couple and pull it off; and only US GA uses it. **Make the entire channel a populate-optional variant**, saving ¥30 on the domestic version |
| **GNSS** | 25×25×6.3 active ceramic patch, mounted in the enclosure's **top-edge bump**, facing skyward, X≈70–95 | SMA | ✅ XA17-G4K | the user has verified this patch gets a good signal; bump details in the archived v3 enclosure study (internal engineering notes), TODO-1 |

**Rationale for the antenna zones**: both the IFA and the patch sit on the top edge, separated by ≥15mm in X to reduce mutual coupling (1090 vs 1575 differ by 45%, an easier pairing than 1090 vs 978).
The IFA is given the left side because the 1090 LNA is wideband and most vulnerable to the C6's 2.4G near field; the GNSS module has its own SAW filtering and tolerates interference better.

**Both GNSS paths are active antennas** → each branch gets its own bias Tee (the switch's RF port cannot pass DC),
the two PMOS are controlled complementarily by the same GPIO pair, so power switches together with the RF. Added cost ≈¥1.

⚠️ 5mm behind the patch is the expansion board's solid ground plane, which will affect the radiation pattern and the resonant point → **make the V1 enclosure's mounting position adjustable, measure it, then freeze it in V2**.
