# Expansion Board V4.11 — Authoritative Pin/Net Mapping (Single Source of Truth for the Schematic)

Chinese version: [`PINMAP-zh_CN.md`](PINMAP-zh_CN.md)

> Rule: this table predates the schematic; when the schematic/PCB conflicts with this table, this table wins and the schematic/PCB must be revised in sync.
> Device-specific pin numbers (QFN48 etc.) are filled in from the datasheet when creating symbols in Phase 2, and re-verified pin by pin per the PAD_NET lesson.
> Source tags: 〔A〕=schematic topology of a similar reference design (factual reference only); 〔F〕=existing definitions in this repo's firmware; 〔D〕=this project's decision.

## 1. Interface to the main board: expansion-board J1 (2×20 female) plugs straight into carrier-board J3 (HAT-style stacking)〔D〕

Only pins actually used by the expansion board are listed; the remaining pins are left as unconnected through-holes:

| Expansion J1 / carrier J3 pin | P4 GPIO | Net | Direction (from P4's perspective) | Basis |
|---|---|---|---|---|
| 1, 3 (bottom-row ends) | — | VCC_5V | power in | carrier board pinout, full bottom-row cross-check〔F〕 |
| 5,10,13,19,26,29,33,40 | — | GND | — | top row 40/26/10 + bottom row 33/29/19/13/5〔F〕 |
| 14 | GPIO4 | P4_GPIO4_SW3_SENSE | input (active-low) | V4.11 isolated SW3 sense path; `docs/hardware/board_pinout.md`, `firmware/main/sw3_button.c`〔F〕+〔D〕 |
| 4 | GPIO7 | I2C_SDA | bidirectional | imu_task.c:49〔F〕 |
| 6 | GPIO8 | I2C_SCL | output | imu_task.c:50〔F〕 |
| 32 | GPIO49 | GNSS_RXD (P4 TX → GNSS) | output | gps_task.c:20〔F〕 |
| 36 | GPIO51 | GNSS_TXD (GNSS → P4 RX) | input | gps_task.c:21〔F〕 |
| 34 | GPIO50 | GNSS_PPS | input | gps_task.c:21 comment〔F〕 |
| 28 | GPIO34 | IMU_INT | input | imu_task.c:56〔F〕 |
| 16 | GPIO28 | IMU_RST | output | imu_task.c:57〔F〕 |
| 24 | GPIO31 | BARO_INT (optional) | input | baro_task.c:32〔F〕 |
| 35 | GPIO46 | ADSB_TXD (RP2040 → P4 RX) **new** | input | marked free on the carrier board〔F〕+〔D〕 |
| 31 | GPIO32 | ADSB_RXD (P4 TX → RP2040, config/control) **new** | output | marked free on the carrier board〔F〕+〔D〕 |

### V4.11 SW3 sensing on J1.14 / carrier J3-14

The complete one-way sensing path is `GPIO4 → J3-14/J1.14 → R67 (1 kΩ) → D10 → CHG_QON`.
`SW3.1` still directly pulls `CHG_QON` / SY6970 `/QON` low and `SW3.2` still goes to GND,
so ship-mode wake remains a hardware function independent of the P4. D10's **anode is on the
GPIO4/R67 side** and D10's **cathode is on the CHG_QON side**. This copies an asserted low to
GPIO4 while isolating the measured approximately 3.57 V `/QON` released level; the MCU must
never drive this input or add a 3.3 V pull-up directly to `/QON`.

The reuse/occupancy decision is evidence-backed:

- `docs/hardware/board_pinout.md` maps carrier J3 pin 14 to ESP32-P4 GPIO4; the expansion
  connector is J1, hence the mated point is expansion J1.14.
- A source search found no production GPIO4 owner before this ECO. The current sole owner is
  `firmware/main/sw3_button.c`, which configures `GPIO_NUM_4` as a pulled-up input and never
  drives it.
- GPIO4 is not an ESP32-P4 strapping pin, but it can be PAD-JTAG `MTMS`. The 2026-09-29
  read-only device audit recorded `DIS_PAD_JTAG=0`, `DIS_USB_JTAG=0`,
  `JTAG_SEL_ENABLE=0`, and `SOFT_DIS_JTAG=0`, selecting USB Serial/JTAG rather than the
  GPIO2–5 PAD-JTAG group. If `DIS_USB_JTAG` or `JTAG_SEL_ENABLE` is changed later, this pin
  assignment must be reviewed again.

Electrical design and the completed V4.10 flywire closed-loop evidence are recorded in
`docs/internal/2026-09-29-sw3-gpio4-flywire.md`. V4.11 was frozen only after that bench
closed-loop; the schematic/netlist/PCB remain the authority for its final connectivity.

### V4.6 AIRBAND additions (BK4819 SPI + PCM1808 I2S on the second P4 I2S)

| J3 pin | P4 GPIO | Net | Direction (from P4) | Basis |
|---|---|---|---|---|
| 20 | GPIO29 | VHF_SCN (3-wire SPI chip-select) | output | free J3 pin〔F〕+〔D〕 |
| 22 | GPIO30 | VHF_SCK (3-wire SPI clock) | output | free J3 pin〔F〕+〔D〕 |
| 37 | GPIO47 | VHF_SDATA (3-wire SPI data) | bidirectional | free J3 pin〔F〕+〔D〕 |
| 11 | GPIO5 | VHF_MCLK (PCM1808 system clock) | output | free J3 pin〔F〕+〔D〕 |
| 17 | GPIO22 | VHF_BCK (I2S bit clock) | output | free J3 pin〔F〕+〔D〕 |
| 38 | GPIO52 | VHF_LRCK (I2S word clock) | output | free J3 pin〔F〕+〔D〕 |
| 39 | GPIO48 | VHF_DOUT (PCM1808 → P4) | input | free J3 pin〔F〕+〔D〕 |

The P4 drives the BK4819 3-wire SPI by bit-banging (SDATA is bidirectional; no hardware SPI pin is consumed).
The PCM1808 uses a **second** P4 I2S controller (the carrier's ES8311/ES7210 use I2S0); its signals are
routed through these J3 pins. Airband audio playback is over BLE to the app; the carrier's ES8311 +
NS4150B + H4 speaker are optional (firmware only).

Firmware-side change volume: one added UART driver (GPIO46/32) plus the VHF driver set (BK4819 SPI +
second-I2S capture); zero changes elsewhere.

## 2. RP2040 pin assignment〔D, PIO grouping aligned with A's architecture〕

| RP2040 GPIO | Net | Function |
|---|---|---|
| GPIO0 / GPIO1 | ADSB_TXD / ADSB_RXD | UART0 → P4 (GDL90/raw messages + control) |
| GPIO2 / GPIO3 | BIAS_EN_1090 / BIAS_EN_978 | antenna bias-tee PMOS gates (active-low, 10k pull-up keeps them off by default)〔D〕 |
| GPIO10 / 11 / 12 / 13 | SUBG_SCK / SUBG_MOSI / SUBG_MISO / SUBG_CSN | SPI1 master → CC1312R slave〔A〕 |
| GPIO14 / 15 | SUBG_IRQ / SUBG_SYNC | CC1312R interrupt + sync/bootloader trigger〔A〕 |
| GPIO16 / 17 / 18 | SUBG_TMSC / SUBG_TCKC / SUBG_RESET | cJTAG for reflashing CC1312R firmware〔A〕— **proxy-flash implemented** (RP2040 bit-bang, `cjtag.c`; see `docs/firmware_update.md` CC1312R section) |
| GPIO19 | PULSES | comparator output → PIO0 preamble detection〔A〕 |
| GPIO20–23 | DEMOD0–3 | PIO demod state-machine handshake/debug〔A〕. On J11 since V4.9 (V4.9: pins 10/12/14/13; **V4.10 shifted every pin up by 4 — now 14/16/18/17**, see below) |
| GPIO24 | RECOVERED_CLK | recovered clock for debugging〔A〕. On J11 since V4.9 (V4.9: pin 15, **V4.10: pin 19** — pin 15 is GND now); the firmware borrows it as the self-test pulse output |
| GPIO25 | TL_PWM | comparator threshold PWM (RC-filtered into LEVEL_BIAS)〔A〕 |
| GPIO26 / ADC0 | LEVEL_BIAS_SENSE | DC readback of the threshold〔A〕 |
| GPIO27 / ADC1 | RSSI | AD8313 VOUT readback (adaptive sensitivity)〔A〕 |
| GPIO28 / ADC2 | spare ADC | reserved (e.g. 978 RSSI) |
| QSPI dedicated pins | W25Q128JVSIQ | 16MB firmware+config |
| XIN | 12MHz crystal (C9002) | |
| USB_DP/DM | USB-C receptacle | UF2 drag-and-drop flashing + CDC debug〔D〕 |
| RUN + BOOTSEL | J11 bridge pairs 5-6 / 7-8 (bare pads, tweezers/solder) | dedicated pads SW1/SW2 removed in V4.9, folded into J11 (V4.10: 2x10 bare-pad array)〔D〕 |
| SWD (SWCLK/SWDIO) | **J11 pins 10 / 12** | broken out with J11 since V4.9 (formerly TP1/TP2; V4.9 pins 6/8, shifted up by 4 in V4.10) |

### USB path for the RP2040 (verified 2026-08-23)

| USB peripheral (ESP32-P4) | Default pins | Can act as host? | Exposed on the Waveshare J3? |
|---|---|---|---|
| USB Serial/JTAG | GPIO24 / 25 | ❌ device only | ✅ pin23 / pin21 |
| USB 2.0 Full-Speed OTG | GPIO26 / 27 | ✅ | ❌ **not exposed** |
| USB 2.0 High-Speed OTG | dedicated pins 49/50 | ✅ | ✅ pin27 / pin25 (same net as H2) |

⚠️ **The GPIO24/25 exposed on J3-23/21 are USB Serial/JTAG, not OTG FS**, and cannot act as host.
Switching requires burning the eFuse `USB_PHY_SEL` (irreversible, and it kills USB-JTAG). If the P4 is to reflash the RP2040,
only the HS pair (pin27/25) can be used, at the cost of permanently occupying H2 and the carrier board's USB-A. See
[VARIANTS.md](VARIANTS.md) §4–§5.

## J11 debug pad array (new in V4.9; changed to a 2x10 bare-pad array in V4.10, bottom side)

Before this revision there was **no way to reach the CC1312R's cJTAG pins**:
`SUBG_TMSC`/`SUBG_TCKC`/`SUBG_RESET` ran point-to-point between `U8` (RP2040)
and `U10`, with nowhere to break the link or land a probe. J11 breaks all three
out and replaces the former `TP1`–`TP7` and `SW1`/`SW2` (nine parts removed).

🔴 **Two V4.10 changes will make you misread a V4.9-era diagram:**

1. **It is not a header, it is a bare-pad array** (in-house footprint
   `TestPad_2x10_P2.54mm`: twenty φ1.0 mm bare copper pads on a 2.54 mm grid,
   no part fitted). It mates with a 2x10p PCB test clip whose probes press
   straight onto the pads. **With no header there are no jumper caps** — the
   three bridges are now made with tweezers or a solder blob.
2. **Every pin number shifted by four**: V4.9 pins 1–16 are now **pins 5–20**,
   and the new pins 1–4 carry the airband signals.

⚠️ **The silkscreen uses abbreviations** (0.8 mm text cannot fit a name like
`RECOVERED_CLK`). Use this table when the board and the docs disagree:

| Pin | Silkscreen | Net | V4.9 pin | Notes |
|---|---|---|---|---|
| 1 | `TCXO` | `VHF_TCXO_TP` | — | BK4819 26 MHz reference (new in V4.10). **Reaches `VHF_TCXO` only through `R66` (0 R, not fitted by default)** |
| 2 | `AUD` | `VHF_AUDIO` | — | BK4819 demodulated analogue audio (new in V4.10) |
| 3 | `VSCK` | `VHF_SCK` | — | BK4819 SPI clock (new in V4.10) |
| 4 | `VSDA` | `VHF_SDATA` | — | BK4819 SPI data (new in V4.10) |
| 5 | `GND` | GND | 1 | bridge 5-6 = reset |
| 6 | `RUN` | `RP_RUN` | 2 (former **SW1** RESET solder jumper) | RP2040 reset |
| 7 | `GND` | GND | 3 | bridge 7-8 = BOOTSEL |
| 8 | `BOOT` | `QSPI_SS` | 4 (former **SW2** BOOTSEL solder jumper) | pull low to boot into UF2 mode |
| 9 | `TMS` | `SUBG_TMSC` | 5 | **CC1312R cJTAG**, first broken out in V4.9 |
| 10 | `SWCLK` | `SWCLK` | 6 (former TP1) | RP2040 SWD clock |
| 11 | `TCK` | `SUBG_TCKC` | 7 | **CC1312R cJTAG** |
| 12 | `SWDIO` | `SWDIO` | 8 (former TP2) | RP2040 SWD data |
| 13 | `RST` | `SUBG_RESET` | 9 | **CC1312R reset** |
| 14 | `D0` | `DEMOD0` | 10 (former TP3) | PIO demod state output |
| 15 | `GND` | GND | 11 | ground reference for the cJTAG side |
| 16 | `D1` | `DEMOD1` | 12 (former TP4) | same |
| 17 | `D3` | `DEMOD3` | 13 (former TP6) | same |
| 18 | `D2` | `DEMOD2` | 14 (former TP5) | same |
| 19 | `RCLK` | `RECOVERED_CLK` | 15 (former TP7) | GPIO24, borrowed by firmware as the self-test pulse output |
| 20 | `PLS` | `PULSES` | 16 | GPIO19, TLV3501 comparator → PIO 1090 pulse input |

🔴 **Pin 1 (the 26 MHz reference) is open by default.** `R66` (0 R, 0603, top
side at (138.10, 103.80), designator at (138.10, 106.13)) sits in series with it.
Hand-solder a 0 R link when you want to look at the 26 MHz reference with the
clip, and remove it afterwards.

Why it cannot be a direct connection: the branch to J11 runs **119.9 mm on In2**,
a stripline sandwiched between the In1 ground and In3 3V3 planes — the highest
capacitance-per-length layer on the board — worth roughly **17 pF**. The
`TG-5006CG` datasheet specifies `Output load` as **10 kΩ // 10 pF**, and the
`fo-Load` row conditions that on **±10 %**. A direct connection would double or
triple the load. Frequency is not the main risk (the crystal is inside the TCXO
and the output is buffered, and AM airband tolerates large frequency error);
**amplitude is**: the 0.8 Vpp minimum is specified into 10 pF, and 27 pF is only
227 Ω at 26 MHz. The BK4819 XI pin is specified as a parallel-resonant crystal
port with no external-injection threshold given, so a reference that fails to
start means airband receives nothing at all.

R66 **must sit at the TCXO end** (next to U21/Y4). Placing it at the J11 end
would leave the 120 mm branch hanging on the oscillator node — no benefit at all.
With R66 unfitted the oscillator node carries 11.0 mm plus two vias, about
**1.8 pF**, for a total near 11.8 pF: 7 % above the 11 pF edge of the datasheet's
±10 % window. Extrapolating linearly that is about ±0.35 ppm of load pulling
(48 Hz at 130 MHz, inaudible on AM) and a load impedance of 612 Ω → 520 Ω, so
roughly 10 % less amplitude — a different order of magnitude from the 218 Ω and
−65 % of a direct connection.

Pins 2–4 are directly connected: `VHF_AUDIO` feeds a 10 µF coupling cap and does
not care about 10 pF at audio frequencies, and the SPI lines are slow.

**Three bridge positions** (jumper caps in V4.9; on V4.10's bare pads use
tweezers or a solder blob):

| Bridge | Equivalent to | Purpose |
|---|---|---|
| 5-6 | former SW1 | reset the RP2040 |
| 7-8 | former SW2 | enter BOOTSEL (UF2 flashing) |
| 19-20 | previously a flying wire | self-test loopback `GPIO24 → GPIO19` |

All three are the left/right pair of one row: 2.54 mm centre-to-centre,
1.54 mm edge-to-edge.

🔴 Before using the 19-20 loopback you **must remove `R57`** (33 Ω, the
`PULSES_RAW`→`PULSES` series resistor). Otherwise the TLV3501 push-pull output
fights the GPIO — see the audit round 5 correction in
`firmware/rp2040/selftest_gen.h`.

The pin order follows **the incoming routing order, not a functional grouping**:
the eight former TP/SW nets arrive as a parallel bundle from the upper right,
already sorted by y, so the pads must be ordered the same way for them to drop
straight down without crossing. See `internal/tools/sheet_mcu.py` before
rearranging pins.

Growing to 2x10 in V4.10 **moved the centre up by exactly 2.54 mm**
(y 88.44 → 85.90), and that number is not arbitrary: pad 5 of a 2x10 sits at
-6.35 from centre while pad 1 of a 2x8 sits at -8.89, so the shift lands the new
pins 5–20 exactly on the old pins 1–16, and none of the sixteen already-routed
nets had to be redone. Growing downwards instead would have hit RT1 on the
bottom side (y 99.41–102.46) and the through-hole H3 mounting hole.

---

## 3. CC1312R (QFN48) logical assignment〔D; DIO→pin numbers get filled in from the datasheet when building the library〕

| Logical signal | CC1312R side | Counterpart |
|---|---|---|
| SPI slave: SCLK/MOSI/MISO/CSN | DIO10 / DIO8 / DIO9 / DIO11 | RP2040 SPI1 |
| IRQ / SYNC | DIO12 / DIO13 | RP2040 GPIO14/15 |
| cJTAG TMSC / TCKC | dedicated JTAG_TMSC / JTAG_TCKC pins | RP2040 GPIO16/17 |
| RESET_N | dedicated pin | RP2040 GPIO18 |
| RF_P / RF_N | differential RF | LC matching network → J3 (U.FL 978) |
| X48M | 48MHz crystal (Abracon ABM8W 7pF preferred; KDS 12pF pending a check of the on-chip capacitor array range) | |
| X32K | 32.768kHz (EPSON FC-135) | |

978 differential matching starting values〔A〕: L 7.5nH / 27nH / 6.8nH×2; C 3.6pF×2 / 2.7pF / 6.2pF. The 915→978 frequency shift means V4 leaves tunable slots everywhere.

## 4. Power tree〔D, one extra split RF LDO compared to A〕

```
J3 VCC_5V ──┬── ME6211C33 (500mA) ──→ 3V3_DIG: RP2040, Flash, CC1312R (via ferrite bead), BNO085, BMP388, QMC5883P
            ├── TPS7A2033 #1 ──────→ 3V3_RF: QPL9547, BGA2817, AD8313, TLV3501 comparator domain
            └── TPS7A2033 #2 ──────→ 3V3_GNSS: ATGM336H-6N-74
GNSS active-antenna bias (×2: internal patch / external SMA): ATGM336H VCC_RF (U7.14) → PMOS high-side switch (Q4/Q5) → 6V/200mA fuse → 33nH feed inductor → antenna. The module senses the VCC_RF current to report ANTENNA OK/OPEN/SHORT.
Bias tees (×2: 1090 / 978)〔A〕: PMOS high-side switch + 6V/200mA fuse + 100nH feed inductor + ESD (0.6pF class)
```

## 5. 1090 receive chain, net by net〔A topology + our component choices〕

```
Onboard IFA → ZP1 shunt → ZS1 series → ZP2 shunt → C53 100pF ─┐
                                                              ├→ U16 RF switch → C54 → QPL9547 (LNA①)
J6 external U.FL → bias tee → C30 100pF ─────────────────────┘
→ C 100pF → TA0970A (SAW①) → matching as above → BGA2817 (LNA②) → C 100pF → TA0970A (SAW②)
→ C 100pF C0G coupling (C34/C35) → AD8313 (U14, set on 2026-08-26 as the sole detector scheme; AD8319 removed)
→ RSSI/VOUT ─┬→ RP2040 ADC1
             └→ TLV3501 IN+; IN- = LEVEL_BIAS (TL_PWM via 10k+100nF RC) + 10k hysteresis〔A〕
→ TLV3501 OUT = PULSES → RP2040 GPIO19
Test points: V1 removed the MM8930 production-test socket and the RF-section W.FL (1090MHz stub); the onboard-IFA side keeps the J7 debug port
```

All LC values are〔A〕starting points; V1 leaves an 0402 tunable slot at every reference designator. After the SAW swap (TFS1090F→TA0970A) the matching values must be tuned on the real board.

The onboard IFA's J7 hangs after the π network and before C53. A VNA measurement at J7 is isolated whenever the downstream receive path is open.
On the measured V4.0 board C53 was populated, but U16 and the other downstream parts were not, so C53 did not create a load path and did not affect
the result. If the downstream path is populated on another board, isolate it before measuring.
The current π network defaults to ZS1=0R, ZP1/ZP2=DNP, i.e. straight-through. The V4.0 antenna was physically trimmed to a **50.0mm copper outer
envelope / 48.5mm radiating-arm centerline span**, reached by cutting from 53.5mm and
stopping at 50.0mm. In the case with the battery fitted it measured **1082.5MHz, SWR 1.09,
45+j0.5Ω**; with the case open the reading drifts over 1080–1090MHz. V4.4 transfers this geometry to the generator, footprint library and embedded ANT1; the taper and 5.103mm feed path remain unchanged.

## 6. Sensors / GNSS (wired straight to P4, not through the RP2040)〔F architecture unchanged〕

| Device | Bus/pins | Notes |
|---|---|---|
| BNO085 | I2C address 0x4A + IMU_INT(GPIO34) + IMU_RST(GPIO28) | PS0/PS1 and SA0 tied low; CLKSEL0 and H_CSN tied high; V4.4 footprint rotation is 90° |
| BMP388 | I2C addr 0x76 (baro_task.c:29) + BARO_INT(GPIO31) | case keeps a vent hole |
| QMC5883P | I2C address 0x2C | keep away from inductors / high-current traces; pins and support network verified against the datasheet |
| ATGM336H-6N-74 | UART0(RXD0/TXD0) → J3; 1PPS → GPIO50; VCC_RF (pin 14) powers the two bias-tee PMOS sources (Q4/Q5) so the module can detect the active antenna | 18-pin LCC, pin diagram verified (manual §2.3, §2.7) |
| GNSS antenna | third U.FL | V1 uses fully external antennas |

## 7. Known conflicts / to-verify list

1. ~~BNO085 I2C address and mode straps~~ **verified in the exported netlist: SA0/PS0/PS1=GND, address 0x4A, I2C mode.**
2. Range of the CC1312R on-chip 48M load-capacitor array → decides Abracon 7pF or KDS 12pF.
3. TA0970A input/output impedance and matching values (recompute once the datasheet arrives; current values are placeholders only).
4. Interference check between the USB-C (RP2040 flashing port) opening orientation once stacked on the J3 female header and the 4.3-inch case (docs/jlc/lcd-4.3in/3d-case) — done in the PCB phase.
5. Distance constraint between QMC5883P and the RP2040/DCDC (magnetometer clean zone) — set in the PCB-phase layout-constraints document.
6. ~~QPL9547 EN pin polarity~~ **verified (Qorvo DS Rev.D): SD pin <0.63V = LNA ON, ground-enable is correct**.
7. (Added in the schematic phase) TLV3501 SHDN is pulled high to enable (per TI, low level shuts down); re-check polarity consistency against the TOKMAS version.
8. (Added in the schematic phase) The official KiCad CC1312R symbol has no DIO_0 — before PCB layout, re-check every QFN48 pin against TI SWRS210.
9. ~~AD8319 CLPF left floating~~ — 2026-08-26 switched to AD8313 (no CLPF pin); this item is void.
10. ~~No ESD diode on the 978 antenna port~~ **handled: D3 is in the schematic (aligned with 1090; exposed ports on an aviation product always get ESD)**.
11. (Added in the schematic phase) The asymmetric 12/15pF 32k-crystal load capacitors were copied verbatim from the original schematic; normally they should be symmetric — discussion item for review.
12. ~~Onboard-IFA length bring-up~~ **resolved on V4.0 on 2026-09-03:** trim from about 54mm to a 50.0mm outer envelope moved the center to about
    1090MHz; the straight-through π state measured the values above. Radiation efficiency and over-the-air message/range A/B remain to be verified.

## 8. Device version verification checklist (user requirement: datasheet version = actually purchased version; SMT placement allowed only after item-by-item arrival check)

| Device | Datasheet version the library was built from | Purchase SKU | Arrival check points |
|---|---|---|---|
| BNO085 | CEVA 1000-3927 **v1.16** (BNO080/085/086 share the same pinout) | Taobao ¥56 grade, loose new parts | silkscreen "BNO085" (beware of "BN0085" counterfeits); LGA-28 3.8×5.2mm physical measurement |
| ~~AD8319~~ | — | — | **dropped 2026-08-26**: replaced by the single AD8313 scheme (70dB vs 40dB dynamic range, and cheaper to actually buy) |
| AD8313 | ADI Rev.F | LCSC C578690 (this channel only) | silkscreen "J1A"; MSOP-8 |
| BMP388 | Bosch DS001-07 Rev 1.7 | LCSC C779278, original manufacturer | LGA-10 2×2mm; spot-check CHIP_ID=0x50 on arrival |
| QMC5883P | QST 13-52-19 RevA (**the very same document linked on the LCSC C2847467 product page**) | Taobao ¥5.5 grade or LCSC | LGA-16 **3×3mm** (do not mix up with the L-version small package); ACKs on I2C 0x2C |
| ATGM336H-6N-74 | Unicore 6N user manual (pin table §2.3) + 5N manual land pattern (§2.2; the two generations are pin-compatible in the same package) | Taobao ¥11 grade (look for the "full-mode/GALILEO" wording) | silkscreen contains "6N-74"; on power-up the NMEA GSV should show GA (Galileo) sentences — the hardest evidence of the single-BDS/full-mode distinction |
| MM8930-2620 | Murata O30E catalog, 2025-12 edition (RJ4/RK15 differ only in packaging/reel, same part) | LCSC C6227587 (RJ4) | 1.6×1.6mm; through-path continuity test (R-C conducts with no probe inserted) |
| TA0970A | TST Rev 3.0 (**6 pads**) | LCSC C7115531 (listed as "8P", conflict has been filed) | **first thing on arrival: count the pads**. If the physical part has 8 pads the footprint is scrapped and redrawn, and the PCB release waits until the physical part is confirmed |
| CC1312R1F3RGZR | official KiCad symbol (cross-check every pin against TI SWRS210 when drawing the schematic) | Taobao ¥11 genuine grade | silkscreen CC1312R1F3; QFN48 7×7 |
| W25Q128JVSIQ | Winbond JV series (official symbol W25Q128JVS) | LCSC C97521 | SOIC-8 208mil wide body (footprint selection point; do not draw it as 150mil) |

**Rule**: any part that fails verification never goes on the board; until TA0970A is confirmed physically, the PCB does not go to production.
