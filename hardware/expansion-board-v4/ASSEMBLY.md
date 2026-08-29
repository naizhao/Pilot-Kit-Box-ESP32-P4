# V4.0 Manual SMT Placement List

Chinese version: [`ASSEMBLY-zh_CN.md`](ASSEMBLY-zh_CN.md)

> Auto-generated from schematic sheets + PCB coordinates by an internal tool — **do not hand-edit**; it is regenerated after any board revision.

Companion views: board positions in `render/top.png`; silkscreen and pin1 in `render/silk.svg`.
For the color-coded full-board assembly maps see
[`ASSEMBLY_MAP.pdf`](ASSEMBLY_MAP.pdf) (Chinese: [`ASSEMBLY_MAP-zh_CN.pdf`](ASSEMBLY_MAP-zh_CN.pdf));
placement groups and mutex rules are defined in
[`SELECTIVE_PLACEMENT.md`](SELECTIVE_PLACEMENT.md).

## ⚠️ Confirm Which Version You Are Building Before Assembly

The same PCB yields two versions, and **one part group follows reverse logic: populated only when the power section is left unpopulated**. This is the easiest thing to get wrong — verify before starting:

| | Power section (U18/U19/U20/L16/L17/J9/RT1/C70-C80/R37-R46) | F-side `R7`/`R8` (5.1k CC pull-downs) |
|---|---|---|
| **Powered version** | ✅ Populate all | ❌ **Do not populate** |
| **Non-powered version** | ❌ Populate none | ✅ **Must populate** |

**Why the logic is inverted**: for USB-C to be recognized by the host, the CC pins must have Rd (5.1k). The powered version provides it internally via CH224K; adding external R7/R8 in that case scrambles the CC resistance and interferes with PD negotiation. The non-powered version has no CH224K, and without R7/R8 the CC pins float — **plugging into a computer gets no response at all, and you cannot flash RP2040 firmware via UF2**.

A silkscreen box is printed around `R7`/`R8` on the board, labeled `NO-PWR VER` — **parts inside the box are populated only on the non-powered version**.

⚠️ This part group **was once entirely on the B side**. After "no paste on the B side" was set as a principle, the 26 parts moved to the F side; only `J9` (battery connector) and `RT1` (NTC — must sit against the battery to sense its temperature) remain on the B side. The "B-side power section" wording in older documents is obsolete — do not look for parts by board side; look them up by designator.

**Side count when placing the SMT order** (only 3 positions remain on the B side; both versions can go single-sided):

| Version | B-side SMT parts | How to order |
|---|---|---|
| **Non-powered version** | 0 (`J9`/`RT1` belong to the power section and stay unpopulated; `SW2` is a pad jumper with no part) | Single-sided SMT, economical |
| **Powered version** | 2 (`J9` connector + `RT1` 0603) | Single-sided SMT + hand-solder these 2, cheaper than paying for a B-side stencil; for convenience you can also order double-sided |

See [VARIANTS.md](VARIANTS.md) for details.

## ⚠️ IFA Pre-Study Is Integrated into the PCB but Not Yet Verified on a Physical Board

The generator, footprint library, `ifa_geom.py`, and the ANT1 embedded in the PCB have been unified to a 52.0mm PCB drawn length / 53.5mm outer envelope; the taper, the 5.103mm feed path, the π network, and J7 have all landed on the board — but this is not a tuned 1090MHz antenna. The π network defaults of ZS1=0R and ZP1/ZP2=DNP merely represent a straight pass-through. When measuring the antenna through J7, C53 must be left unpopulated; the first-round HFSS match on the full six-layer rev2 can be swept starting from ZS1=3.6nH, ZP2=3.3pF, ZP1=DNP, with 3.6/3.9pF on hand, and final values are set by in-enclosure VNA. Integration steps are in the archived v3 HFSS antenna research report §8 (internal engineering record; the key conclusions are already in this document: 52.0mm drawn length / 53.5mm outer envelope, with 50.2mm as the trim-short target on the physical board).

---

## Designator Silkscreen Self-Check: 0 Swapped Pairs (No Issues ✅)

The criterion: "standing beside a component, does the nearest designator text belong to it?"
Checking the other way — "find the nearest component for each text" — misses some; that is exactly how the ones hidden by large chips got missed.

Swapped designators **cannot be caught by visual inspection, DRC, or Gerber preview**: every component has a designator next to it and the layout looks completely normal — only the assignments are crossed. That is why this check exists.

---

## Before Starting Work

**Before starting, run bare-board checks: power-to-ground insulation + inner-layer continuity.** Once parts are on, these two can no longer be measured — decoupling caps and chips sit in parallel between power and ground, so the reading is no longer open-circuit.

### Placement Order Principle: Large Before Small, Center Before Periphery

Whether you reflow the whole board at once or reflow part by part with hot air, the conclusion is the same:

- **Full-board reflow** — large parts need repeated alignment adjustments; 0402s placed around them first get knocked crooked by the tweezers

- **Hot air part by part** — blowing the periphery can flip small parts already placed; large parts have high thermal mass and are unharmed in the reverse order

So within each stage below, parts are **sorted by footprint area, largest first** — just follow the table top to bottom.

### Common Pitfalls

- **Plastic-encapsulated parts such as BNO085 / BMP388 / CC1312R are moisture-sensitive**. If a package has been out of its sealed bag for more than a week, bake it at 125°C for 4 hours before placement; otherwise internal moisture vaporizes during reflow and cracks the package (popcorn effect) — invisible from the outside, the symptom is a chip that simply does not work
- **Parts with bottom pads (★★★) are out of reach of an iron**; if blown askew, the only option is removal and redo, so confirm alignment repeatedly before heating — do not count on fixing it afterwards
- **Always turn the hot-air flow down for 0402s**; at nominal airflow, 0402s get blown right off, and the flying part often lands on other pads and shorts them
- **pin1 orientation follows the board silkscreen** (notch/dot); the "Rotation" column below is only a reference. Component orientation on this board had errors early on — 25 two-terminal parts once had pin1/pin2 reversed

---

## Stage A: Power (37 Parts)
3 LDOs plus the input/output capacitors on each rail. **These 9 parts form a self-contained loop** — once placed you can apply power and measure voltages, without touching any expensive part.

| Designator | Part | Footprint | Board Position | Rotation | Hand-Solder Difficulty |
|---|---|---|---|---|---|
| **J9** | MX1.25WT-2P BAT | MX1.25WT-2P_1x02-1MP_P1.25 | (134.6, 108.7) | 0° | ★   Standard |
| **U18** | CH224K | CH224K_ESSOP-10 | (132.5, 68.6) | 0° | ★   Standard |
| **U19** | SY6970 | QFN-24-1EP_4x4mm_P0.5mm_EP | (134.5, 83.7) | 0° | ★★★ Bottom pads, hot air only, iron cannot reach |
| **L16** | 1uH | L_Bourns-SRN4018 | (139.7, 83.6) | -90° | ★   Standard |
| **L17** | 1.5uH | L_Bourns-SRN4018 | (144.8, 95.0) | -90° | ★   Standard |
| **U2** | TPS7A2033PDBVR | SOT-23-5 | (82.3, 97.8) | 90° | ★   Leads exposed, iron can touch up |
| **U20** | SY7069 | TSOT-23-6 | (139.2, 95.0) | 0° | ★   Leads exposed, iron can touch up |
| **U1** | ME6211C33M5 | SOT-23-5 | (74.7, 97.8) | 90° | ★   Leads exposed, iron can touch up |
| **U3** | TPS7A2033PDBVR | SOT-23-5 | (89.2, 97.8) | 90° | ★   Leads exposed, iron can touch up |
| **C72** | 6.8uF | C_1206_3216Metric | (130.9, 79.2) | 90° | ★   Standard |
| **C77** | 22uF | C_1206_3216Metric | (141.2, 99.7) | -90° | ★   Standard |
| **C76** | 22uF | C_1206_3216Metric | (134.4, 96.3) | 180° | ★   Standard |
| **RT1** | NCP18XH103F03RB 10k NTC | R_0603_1608Metric | (120.9, 82.0) | 90° | ★   Standard |
| **C1** | 10uF | C_0805_2012Metric | (70.8, 96.5) | 180° | ★   Standard |
| **C73** | 10uF | C_0805_2012Metric | (141.3, 88.1) | -90° | ★   Standard |
| **C75** | 10uF | C_0805_2012Metric | (139.1, 88.1) | -90° | ★   Standard |
| **C74** | 10uF | C_0805_2012Metric | (143.3, 84.9) | 90° | ★   Standard |
| **C6** | 1uF | C_0603_1608Metric | (92.1, 97.9) | 90° | ★   Standard |
| **R45** | 10k | R_0603_1608Metric | (129.2, 84.0) | 180° | ★   Standard |
| **R42** | 150k | R_0603_1608Metric | (137.4, 98.6) | -90° | ★   Standard |
| **C3** | 1uF | C_0603_1608Metric | (70.8, 98.9) | 180° | ★   Standard |
| **C70** | 47nF | C_0603_1608Metric | (135.7, 80.2) | 0° | ★   Standard |
| **R46** | 10k | R_0603_1608Metric | (129.2, 86.5) | 180° | ★   Standard |
| **C79** | 1uF | C_0603_1608Metric | (130.6, 65.0) | 0° | ★   Standard |
| **C2** | 1uF | C_0603_1608Metric | (85.9, 99.1) | 0° | ★   Standard |
| **R41** | 470k | R_0603_1608Metric | (139.1, 98.6) | 90° | ★   Standard |
| **R39** | 5.62k | R_0603_1608Metric | (136.2, 88.2) | 90° | ★   Standard |
| **R44** | 10k | R_0603_1608Metric | (137.0, 69.5) | 90° | ★   Standard |
| **R37** | 6.8k | R_0603_1608Metric | (137.0, 66.4) | 90° | ★   Standard |
| **C4** | 1uF | C_0603_1608Metric | (78.7, 97.8) | 0° | ★   Standard |
| **R40** | 31.6k | R_0603_1608Metric | (134.6, 88.2) | -90° | ★   Standard |
| **R38** | 180R | R_0603_1608Metric | (133.1, 88.2) | -90° | ★   Standard |
| **C71** | 4.7uF | C_0603_1608Metric | (133.4, 79.0) | 90° | ★   Standard |
| **R43** | 1k | R_0603_1608Metric | (136.3, 72.0) | 180° | ★   Standard |
| **C78** | 1uF | C_0603_1608Metric | (134.4, 94.2) | 180° | ★   Standard |
| **C80** | 1uF | C_0603_1608Metric | (136.3, 74.6) | 180° | ★   Standard |
| **C5** | 1uF | C_0402_1005Metric | (85.9, 96.7) | 0° | ★★  0402, easily blown away by hot air, turn airflow down |

**Verification after placement**: bench supply current-limited to 100mA; clip the 5V positive lead to `J1.1` and the negative to `U10.49` (the CC1312R's large EP pad — nothing placed there yet, so the pad is bare and easy to clip).
  - No-load current should be only a few mA; if it hits the limit the instant power is applied, cut power and check U1/U2/U3 for solder bridges
  - Measure `U9.8`: 3V3_DIG = 3.3V ±5%
  - Measure `U14.1`: 3V3_RF = 3.3V ±5%
  - Measure `Q2.2`: 3V3_GNSS = 3.3V ±5%
  Proceed only when all three rails pass. If this step fails, everything placed afterwards is wasted.

## Stage B: MCU + Flash (21 Parts)
RP2040 + QSPI Flash + 12MHz crystal + USB-C receptacle.

| Designator | Part | Footprint | Board Position | Rotation | Hand-Solder Difficulty |
|---|---|---|---|---|---|
| **J4** | USB-C_16P | USB_C_Receptacle_HRO_TYPE- | (145.8, 73.0) | 90° | ★   Standard |
| **U8** | RP2040 | QFN-56-1EP_7x7mm_P0.4mm_EP | (86.4, 76.9) | 180° | ★★★ Bottom pads, hot air only, iron cannot reach |
| **U9** | W25Q128JVSIQ | SOIC-8_5.3x5.3mm_P1.27mm | (93.7, 85.9) | 0° | ★   Leads exposed, iron can touch up |
| **Y1** | 12MHz | Crystal_SMD_3225-4Pin_3.2x | (85.1, 69.1) | 180° | ★   Standard |
| **R6** | 10k | R_0603_1608Metric | (87.9, 86.4) | 90° | ★   Standard |
| **R5** | 10k | R_0603_1608Metric | (80.0, 73.2) | 180° | ★   Standard |
| **R4** | 1k | R_0603_1608Metric | (82.0, 69.1) | 90° | ★   Standard |
| **C29** | 1uF | C_0603_1608Metric | (80.0, 81.0) | 180° | ★   Standard |
| **C27** | 100nF | C_0603_1608Metric | (93.2, 81.8) | 0° | ★   Standard |
| **C25** | 100nF | C_0603_1608Metric | (82.8, 85.1) | -90° | ★   Standard |
| **R8** | 5.1k | R_0603_1608Metric | (143.6, 64.8) | -90° | ★   Standard |
| **C22** | 100nF | C_0603_1608Metric | (94.5, 79.5) | 0° | ★   Standard |
| **R7** | 5.1k | R_0603_1608Metric | (145.6, 64.8) | -90° | ★   Standard |
| **C28** | 1uF | C_0603_1608Metric | (81.3, 82.8) | 180° | ★   Standard |
| **C23** | 100nF | C_0603_1608Metric | (94.5, 74.9) | 0° | ★   Standard |
| **C20** | 33pF | C_0603_1608Metric | (80.0, 69.1) | 90° | ★   Standard |
| **R10** | 27R | R_0603_1608Metric | (85.3, 83.3) | -90° | ★   Standard |
| **C19** | 33pF | C_0603_1608Metric | (88.4, 69.1) | -90° | ★   Standard |
| **C26** | 100nF | C_0603_1608Metric | (80.0, 75.8) | 180° | ★   Standard |
| **R9** | 27R | R_0603_1608Metric | (87.9, 83.3) | -90° | ★   Standard |
| **C24** | 100nF | C_0603_1608Metric | (80.0, 78.4) | 180° | ★   Standard |

**Verification after placement**: plug USB-C into a computer, short `SW2` (BOOTSEL) with tweezers, then power up → the computer should show the **RPI-RP2** USB drive.
  - Drive not recognized: first measure `C28.1` for RP_1V1 at 1.1V (from the RP2040's internal LDO)
  - Drive recognized but Flash not detected: check the four QSPI lines of U9; the SOIC-8 leads are exposed, so an iron can rework them

## Stage C: Sensors + GNSS (30 Parts)
IMU / barometer / magnetometer / GNSS, all on slow I2C and UART interfaces.

| Designator | Part | Footprint | Board Position | Rotation | Hand-Solder Difficulty |
|---|---|---|---|---|---|
| **U7** | ATGM336H-6N-74 | ATGM336H_LCC-18 | (73.7, 89.1) | 0° | ★★★ Bottom pads, hot air only, iron cannot reach |
| **U4** | BNO085 | BNO085_LGA-28 | (59.7, 84.5) | 90° | ★★★ Bottom pads, hot air only, iron cannot reach |
| **J8** | U.FL→internal patch | U.FL_Hirose_U.FL-R-SMT-1_V | (54.0, 69.2) | 180° | ★   Standard |
| **J2** | U.FL_GNSS_EXT (via pigtail to SMA) | U.FL_Hirose_U.FL-R-SMT-1_V | (54.0, 75.9) | 180° | ★   Standard |
| **U6** | QMC5883P | LGA-16_3x3mm_P0.5mm | (59.7, 96.5) | -90° | ★★★ Bottom pads, hot air only, iron cannot reach |
| **Q4** | AO3401A | SOT-23 | (63.5, 75.9) | 180° | ★   Leads exposed, iron can touch up |
| **Q5** | AO3401A | SOT-23 | (63.5, 69.2) | 180° | ★   Leads exposed, iron can touch up |
| **U5** | BMP388 | BMP388_LGA-10 | (61.0, 88.9) | -90° | ★★★ Bottom pads, hot air only, iron cannot reach |
| **U17** | XA17-G4K (or AS179-92LF) | SOT-363_SC-70-6 | (68.6, 78.1) | 90° | ★★  0.65mm pitch, leads exposed but very dense |
| **C18** | 10uF | C_0805_2012Metric | (74.4, 80.8) | 90° | ★   Standard |
| **F4** | 6V/200mA | Fuse_0805_2012Metric | (60.3, 75.9) | 90° | ★   Standard |
| **F5** | 6V/200mA | Fuse_0805_2012Metric | (60.3, 69.2) | -90° | ★   Standard |
| **C16** | 100nF | C_0603_1608Metric | (64.6, 92.5) | 180° | ★   Standard |
| **R2** | 4.7k | R_0603_1608Metric | (64.1, 97.8) | 180° | ★   Standard |
| **C10** | 100nF | C_0603_1608Metric | (61.0, 92.6) | 90° | ★   Standard |
| **C15** | 4.7uF | C_0603_1608Metric | (59.9, 99.3) | 0° | ★   Standard |
| **C17** | 100nF | C_0603_1608Metric | (71.4, 81.0) | 90° | ★   Standard |
| **C11** | 100nF | C_0603_1608Metric | (64.6, 87.2) | 180° | ★   Standard |
| **C14** | 100nF | C_0603_1608Metric | (64.6, 89.9) | 180° | ★   Standard |
| **R3** | 4.7k | R_0603_1608Metric | (64.1, 95.2) | 180° | ★   Standard |
| **C13** | 100nF | C_0603_1608Metric | (59.1, 92.6) | 90° | ★   Standard |
| **C12** | 100nF | C_0603_1608Metric | (56.5, 87.7) | 180° | ★   Standard |
| **R27** | 10k | R_0603_1608Metric | (67.3, 71.1) | 0° | ★   Standard |
| **R26** | 10k | R_0603_1608Metric | (67.3, 73.7) | 0° | ★   Standard |
| **R1** | 10k | R_0603_1608Metric | (55.2, 84.5) | -90° | ★   Standard |
| **L2** | 33nH | L_0402_1005Metric | (57.7, 75.4) | 180° | ★★  0402, easily blown away by hot air, turn airflow down |
| **L15** | 33nH | L_0402_1005Metric | (57.6, 68.7) | 180° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C57** | 100pF | C_0402_1005Metric | (68.3, 81.0) | 90° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C58** | 100pF | C_0402_1005Metric | (57.7, 76.5) | 0° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C59** | 100pF | C_0402_1005Metric | (57.6, 69.8) | 0° | ★★  0402, easily blown away by hot air, turn airflow down |

**Verification after placement**: flash a scan firmware and poll each device on I2C: BNO085, BMP388, and QMC5883P should each respond.
  - For GNSS, check `U7.2` (GNSS_TXD) for NMEA output — **no satellite fix indoors is normal**; `$GPTXT` and `$GPGSV` output means the module is alive

## Stage D: 978 Transceiver (32 Parts)
CC1312R and its two crystals: 48MHz / 32.768kHz.

| Designator | Part | Footprint | Board Position | Rotation | Hand-Solder Difficulty |
|---|---|---|---|---|---|
| **U10** | CC1312R1F3RGZR | QFN-48-1EP_7x7mm_P0.5mm_EP | (104.5, 92.0) | 180° | ★★★ Bottom pads, hot air only, iron cannot reach |
| **J5** | U.FL_978 | U.FL_Hirose_U.FL-R-SMT-1_V | (118.5, 86.3) | 90° | ★   Standard |
| **Y3** | 32.768kHz FC-135 | Crystal_SMD_3215-2Pin_3.2x | (111.6, 87.0) | 0° | ★   Standard |
| **Y2** | 48MHz ABM8W-7pF | Crystal_SMD_3225-4Pin_3.2x | (108.3, 98.3) | 90° | ★   Standard |
| **Q2** | AO3401A | SOT-23 | (121.9, 95.8) | 90° | ★   Leads exposed, iron can touch up |
| **C66** | 22uF | C_0805_2012Metric | (102.9, 97.2) | 180° | ★   Standard |
| **C60** | 22uF | C_0805_2012Metric | (99.3, 98.0) | 90° | ★   Standard |
| **F2** | 6V/200mA | Fuse_0805_2012Metric | (121.9, 92.5) | 180° | ★   Standard |
| **L7** | 6.8uH | L_0805_2012Metric | (97.3, 94.7) | -90° | ★   Standard |
| **C62** | 100nF | C_0603_1608Metric | (105.4, 98.3) | -90° | ★   Standard |
| **C69** | 18pF | C_0603_1608Metric | (110.1, 85.0) | 180° | ★   Standard |
| **C61** | 100nF | C_0603_1608Metric | (102.7, 99.1) | 180° | ★   Standard |
| **C68** | 18pF | C_0603_1608Metric | (113.2, 85.0) | 0° | ★   Standard |
| **C67** | 1uF | C_0603_1608Metric | (103.6, 85.8) | 0° | ★   Standard |
| **R17** | 10k | R_0603_1608Metric | (121.8, 98.7) | 180° | ★   Standard |
| **C65** | 100nF | C_0603_1608Metric | (99.1, 94.7) | -90° | ★   Standard |
| **C63** | 100nF | C_0603_1608Metric | (107.1, 84.6) | 90° | ★   Standard |
| **C64** | 100nF | C_0603_1608Metric | (100.2, 85.8) | 180° | ★   Standard |
| **L9** | 7.5nH | L_0402_1005Metric | (112.3, 94.8) | 90° | ★★  0402, easily blown away by hot air, turn airflow down |
| **L13** | 7.5nH | L_0402_1005Metric | (110.7, 92.2) | 180° | ★★  0402, easily blown away by hot air, turn airflow down |
| **L8** | 100nH | L_0402_1005Metric | (119.3, 90.3) | 180° | ★★  0402, easily blown away by hot air, turn airflow down |
| **L12** | 6.8nH | L_0402_1005Metric | (118.2, 95.2) | 90° | ★★  0402, easily blown away by hot air, turn airflow down |
| **L11** | 6.8nH | L_0402_1005Metric | (118.2, 97.5) | 90° | ★★  0402, easily blown away by hot air, turn airflow down |
| **L10** | 27nH | L_0402_1005Metric | (110.9, 95.7) | 90° | ★★  0402, easily blown away by hot air, turn airflow down |
| **D3** | ESD 3.3V/0.6pF | D_0402_1005Metric | (117.5, 89.9) | -90° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C45** | 100pF | C_0402_1005Metric | (110.7, 90.2) | 0° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C40** | 3.6pF | C_0402_1005Metric | (110.7, 94.2) | 0° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C41** | 2.7pF | C_0402_1005Metric | (115.9, 97.5) | 90° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C42** | 6.2pF | C_0402_1005Metric | (115.9, 95.2) | 90° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C43** | 3pF | C_0402_1005Metric | (115.9, 92.8) | 90° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C39** | 100pF | C_0402_1005Metric | (118.2, 92.8) | 90° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C44** | 3.6pF | C_0402_1005Metric | (112.8, 96.5) | 0° | ★★  0402, easily blown away by hot air, turn airflow down |

**Verification after placement**: ~~connect a debugger to TP1/TP2~~ — **V4 removed the SWD test points**. Use any of the following instead:
  ① Verify the same circuit section on the already-fabricated v3.2 (this circuit is identical between the two revisions, line for line);
  ② Fly-wire a debugger to the RP2040 SWCLK/SWDIO pins (U8.24 / U8.25);
  ③ After flashing firmware, have the RP2040 read the CC1312R ID itself and print it over UART.
  - If it fails to start, measure `C60.1` for SUBG_VDDR (internal DC/DC output, approx. 1.7V) and `C67.1` for SUBG_DCOUPL (approx. 1.28V)

## Stage E: 1090 Receive Chain (43 Parts)
**The most expensive stretch of the board** (QPL9547 ¥8.5 / TA0970A ¥7.5×2 / AD8313), scheduled last — these go on the board only after the four previous stages have been verified.

| Designator | Part | Footprint | Board Position | Rotation | Hand-Solder Difficulty |
|---|---|---|---|---|---|
| **FL2** | TA0970A | TA0970A_SMD3838-6 | (118.4, 66.5) | 180° | ★★★ Bottom pads, hot air only, iron cannot reach |
| **FL1** | TA0970A | TA0970A_SMD3838-6 | (112.0, 65.5) | 0° | ★★★ Bottom pads, hot air only, iron cannot reach |
| **U14** | AD8313ARMZ | MSOP-8_3x3mm_P0.65mm | (111.1, 73.4) | 180° | ★   Leads exposed, iron can touch up |
| **J7** | U.FL_IFA_TEST (post-π tuning port) | U.FL_Hirose_U.FL-R-SMT-1_V | (90.0, 61.3) | 90° | ★   Standard |
| **J6** | U.FL_1090_EXT (via pigtail to SMA) | U.FL_Hirose_U.FL-R-SMT-1_V | (106.5, 67.3) | 90° | ★   Standard |
| **U15** | TLV3501(TOKMAS) | SOT-23-6 | (100.6, 79.5) | 180° | ★   Leads exposed, iron can touch up |
| **Q3** | AO3401A | SOT-23 | (95.3, 71.7) | 0° | ★   Leads exposed, iron can touch up |
| **U11** | QPL9547TR7 | DFN-8-1EP_2x2mm_P0.5mm_EP0 | (107.7, 61.9) | 0° | ★★★ Bottom pads, hot air only, iron cannot reach |
| **U12** | BGA2817 | SOT-363_SC-70-6 | (117.1, 62.5) | 90° | ★★  0.65mm pitch, leads exposed but very dense |
| **U16** | XA17-G4K (or AS179-92LF) | SOT-363_SC-70-6 | (98.0, 62.9) | 90° | ★★  0.65mm pitch, leads exposed but very dense |
| **F3** | 6V/200mA | Fuse_0805_2012Metric | (98.3, 71.7) | 90° | ★   Standard |
| **C49** | 100nF | C_0603_1608Metric | (77.0, 75.8) | 180° | ★   Standard |
| **R31** | 1k | R_0603_1608Metric | (101.6, 75.7) | 0° | ★   Standard |
| **R34** | 10k | R_0603_1608Metric | (77.0, 73.2) | 0° | ★   Standard |
| **R36** | 1k | R_0603_1608Metric | (96.5, 81.8) | 180° | ★   Standard |
| **R18** | 10k | R_0603_1608Metric | (91.4, 71.4) | 180° | ★   Standard |
| **C51** | 1nF | C_0603_1608Metric | (77.0, 81.0) | 180° | ★   Standard |
| **R33** | 100k | R_0603_1608Metric | (100.9, 82.7) | 90° | ★   Standard |
| **C55** | 100pF | C_0603_1608Metric | (97.6, 66.5) | 90° | ★   Standard |
| **R32** | 10k | R_0603_1608Metric | (104.9, 75.7) | 180° | ★   Standard |
| **ZS1** | 0R series | L_0603_1608Metric | (81.7, 62.9) | 0° | ★   Standard |
| **C56** | 100pF | C_0603_1608Metric | (95.0, 66.5) | -90° | ★   Standard |
| **R35** | 10k | R_0603_1608Metric | (77.0, 78.4) | 0° | ★   Standard |
| **C47** | 200pF | C_0603_1608Metric | (104.9, 80.4) | 0° | ★   Standard |
| **R23** | 0R | R_0603_1608Metric | (93.2, 77.2) | 0° | ★   Standard |
| **R22** | 0R | R_0603_1608Metric | (100.3, 66.5) | 90° | ★   Standard |
| **R21** | 0R | R_0603_1608Metric | (105.0, 72.6) | 180° | ★   Standard |
| **C46** | 3pF | C_0603_1608Metric | (104.9, 78.5) | 0° | ★   Standard |
| **D2** | ESD 3.3V/0.6pF | D_0402_1005Metric | (101.1, 72.6) | 180° | ★★  0402, easily blown away by hot air, turn airflow down |
| **L14** | 100nH | L_0402_1005Metric | (101.1, 70.6) | 0° | ★★  0402, easily blown away by hot air, turn airflow down |
| **R19** | 52.3R | R_0402_1005Metric | (115.1, 73.4) | 90° | ★★  0402, easily blown away by hot air, turn airflow down |
| **R30** | 1k | R_0402_1005Metric | (96.8, 77.2) | 180° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C54** | 100pF | C_0402_1005Metric | (103.9, 61.7) | 0° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C31** | 12pF | C_0402_1005Metric | (111.3, 62.2) | -90° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C52** | 100nF | C_0402_1005Metric | (99.6, 82.3) | -90° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C30** | 100pF | C_0402_1005Metric | (102.8, 67.5) | 90° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C53** | 100pF | C_0402_1005Metric | (93.6, 62.9) | 0° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C34** | 3pF | C_0402_1005Metric | (117.1, 73.4) | -90° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C37** | 100nF | C_0402_1005Metric | (113.9, 77.0) | -90° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C32** | 12pF | C_0402_1005Metric | (114.3, 62.2) | 90° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C33** | 12pF | C_0402_1005Metric | (120.4, 63.2) | 0° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C35** | 3pF | C_0402_1005Metric | (115.1, 71.1) | 90° | ★★  0402, easily blown away by hot air, turn airflow down |
| **C38** | 100pF | C_0402_1005Metric | (111.6, 77.0) | -90° | ★★  0402, easily blown away by hot air, turn airflow down |

**Verification after placement**: ~~TP3~TP6 / TP7 to an oscilloscope~~ — **V4 removed these test points**. Instead:
  ① Probe `U15.5` (PULSES) directly — scope probes can reach the SOT-23-6 leads;
  ② Software telemetry RSSI (ADC1) / LEVEL_BIAS (ADC0), with the firmware printing readings and decode success rate over UART;
  ③ To inspect the PIO state machine itself, do it on v3.2 (that board kept all 7 test points).
  - Mis-oriented RF parts are hard to spot; verify pin1 against `render/silk.svg` before placing

## Stage F: External Interface (1 Part)
2×20 pin header, high thermal mass.

| Designator | Part | Footprint | Board Position | Rotation | Hand-Solder Difficulty |
|---|---|---|---|---|---|
| **J1** | J3_HAT_2x20_SMD pin header | PinHeader_2x20_P2.54mm_Ver | (92.6, 106.3) | 90° | ★   Standard |

**Verification after placement**: mate with the main board and check 5V and GND continuity. **Place it last**: populate this first and the board can no longer lie flat on the heating platform during the earlier stages.

## The 10 Unpopulated Positions

| Designator | Why It Stays Unpopulated |
|---|---|
| ANT1 | On-board antenna — the PCB copper foil itself, no component. The 52.0mm drawn length / 53.5mm outer envelope is already on the PCB; pending in-enclosure VNA + message A/B testing — not yet tuned |
| H1 | M2.5 mounting hole (⌀2.7mm NPTH), not a component |
| H2 | M2.5 mounting hole (⌀2.7mm NPTH), not a component |
| H3 | M2.5 mounting hole (⌀2.7mm NPTH), not a component |
| H4 | M2.5 mounting hole (⌀2.7mm NPTH), not a component |
| R24 | DNP, unpopulated by design default (0R DNP (bypass, external)) |
| R25 | DNP, unpopulated by design default (0R DNP (bypass, onboard)) |
| SW2 | Solder jumper pads (BOOTSEL), short with tweezers, no part placed. **The pads do not count toward the SMT side count** — zero cost impact |
| ZP1 | DNP, unpopulated by design default (DNP, parallel — antenna side) |
| ZP2 | DNP, unpopulated by design default (DNP, parallel — radio side) |
