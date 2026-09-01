# Expansion Board V4.3 — Authoritative Pin/Net Mapping (Single Source of Truth for the Schematic)

Chinese version: [`PINMAP-zh_CN.md`](PINMAP-zh_CN.md)

> Rule: this table predates the schematic; when the schematic/PCB conflicts with this table, this table wins and the schematic/PCB must be revised in sync.
> Device-specific pin numbers (QFN48 etc.) are filled in from the datasheet when creating symbols in Phase 2, and re-verified pin by pin per the PAD_NET lesson.
> Source tags: 〔A〕=schematic topology of a similar reference design (factual reference only); 〔F〕=existing definitions in this repo's firmware; 〔D〕=this project's decision.

## 1. Interface to the main board: 2×20 female header, plugs straight into the Waveshare carrier board J3 (HAT-style stacking)〔D〕

Only pins actually used by the expansion board are listed; the remaining pins are left as unconnected through-holes:

| J3 pin | P4 GPIO | Net | Direction (from P4's perspective) | Basis |
|---|---|---|---|---|
| 1, 3 (bottom-row ends) | — | VCC_5V | power in | carrier board pinout, full bottom-row cross-check〔F〕 |
| 5,10,13,19,26,29,33,40 | — | GND | — | top row 40/26/10 + bottom row 33/29/19/13/5〔F〕 |
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

Firmware-side change volume: only one added UART driver (GPIO46/32); zero changes elsewhere.

## 2. RP2040 pin assignment〔D, PIO grouping aligned with A's architecture〕

| RP2040 GPIO | Net | Function |
|---|---|---|
| GPIO0 / GPIO1 | ADSB_TXD / ADSB_RXD | UART0 → P4 (GDL90/raw messages + control) |
| GPIO2 / GPIO3 | BIAS_EN_1090 / BIAS_EN_978 | antenna bias-tee PMOS gates (active-low, 10k pull-up keeps them off by default)〔D〕 |
| GPIO10 / 11 / 12 / 13 | SUBG_SCK / SUBG_MOSI / SUBG_MISO / SUBG_CSN | SPI1 master → CC1312R slave〔A〕 |
| GPIO14 / 15 | SUBG_IRQ / SUBG_SYNC | CC1312R interrupt + sync/bootloader trigger〔A〕 |
| GPIO16 / 17 / 18 | SUBG_TMSC / SUBG_TCKC / SUBG_RESET | cJTAG for reflashing CC1312R firmware〔A〕 |
| GPIO19 | PULSES | comparator output → PIO0 preamble detection〔A〕 |
| GPIO20–23 | DEMOD0–3 | PIO demod state-machine handshake/debug〔A〕. **No landing point in V4**: the original TP3–TP6 were removed, leaving the net with only the RP2040 end |
| GPIO24 | RECOVERED_CLK | recovered clock for debugging〔A〕. **No landing point in V4**: the original TP7 was removed, same as above |
| GPIO25 | TL_PWM | comparator threshold PWM (RC-filtered into LEVEL_BIAS)〔A〕 |
| GPIO26 / ADC0 | LEVEL_BIAS_SENSE | DC readback of the threshold〔A〕 |
| GPIO27 / ADC1 | RSSI | AD8313 VOUT readback (adaptive sensitivity)〔A〕 |
| GPIO28 / ADC2 | spare ADC | reserved (e.g. 978 RSSI) |
| QSPI dedicated pins | W25Q128JVSIQ | 16MB firmware+config |
| XIN | 12MHz crystal (C9002) | |
| USB_DP/DM | USB-C receptacle | UF2 drag-and-drop flashing + CDC debug〔D〕 |
| RUN + BOOTSEL | tactile switches ×2 | dev-board essentials〔D〕 |
| SWD (SWCLK/SWDIO) | ~~3-pin test points~~ **removed in V4** | test points TP1–7 are gone in V4; firmware debug happens on the already-prototyped v3.2 |

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
            └── TPS7A2033 #2 ──────→ 3V3_GNSS: ATGM336H-6N-74 + two antenna bias tees
Bias tees (×2: 1090 / 978)〔A〕: PMOS high-side switch + 6V/200mA fuse + 100nH feed inductor + ESD (0.6pF class)
```

## 5. 1090 receive chain, net by net〔A topology + our component choices〕

```
Onboard IFA → ZP1 shunt → ZS1 series → ZP2 shunt → C53 100pF ─┐
                                                              ├→ U16 RF switch → C54 → QPL9547 (LNA①)
J6 external U.FL → bias tee → C30 100pF ─────────────────────┘
→ C 12pF → TA0970A (SAW①) → matching as above → BGA2817 (LNA②) → C 12pF → TA0970A (SAW②)
→ C 3pF coupling (C34) → AD8313 (U14, set on 2026-08-26 as the sole detector scheme; AD8319 removed)
→ RSSI/VOUT ─┬→ RP2040 ADC1
             └→ TLV3501 IN+; IN- = LEVEL_BIAS (TL_PWM via 100k+0.1µF RC) + 10k hysteresis〔A〕
→ TLV3501 OUT = PULSES → RP2040 GPIO19
Test points: V1 removed the MM8930 production-test socket and the RF-section W.FL (1090MHz stub); the onboard-IFA side keeps the J7 debug port
```

All LC values are〔A〕starting points; V1 leaves an 0402 tunable slot at every reference designator. After the SAW swap (TFS1090F→TA0970A) the matching values must be tuned on the real board.

The onboard IFA's J7 hangs after the π network and before C53. When measuring via J7 with a VNA, leave C53 unpopulated so the downstream stages don't show up in parallel in the reading.
The current π network defaults to ZS1=0R, ZP1/ZP2=DNP, i.e. straight-through; the full six-layer rev2 HFSS first sweep can start from ZS1=3.6nH, ZP2=3.3pF,
ZP1=DNP, with final values settled by measurement in the case. The ANT1 embedded in the generator/footprint library/PCB has been unified to
52.0mm drawn length / 53.5mm outer envelope; the taper and the 5.103mm feed path are already on the board (see the v3 HFSS antenna study
report §8, internal engineering notes).

## 6. Sensors / GNSS (wired straight to P4, not through the RP2040)〔F architecture unchanged〕

| Device | Bus/pins | Notes |
|---|---|---|
| BNO085 | I2C address 0x4A + IMU_INT(GPIO34) + IMU_RST(GPIO28) | PS0/PS1 and SA0 tied low; CLKSEL0 and H_CSN tied high; V4.3 footprint rotation is 90° |
| BMP388 | I2C addr 0x76 (baro_task.c:29) + BARO_INT(GPIO31) | case keeps a vent hole |
| QMC5883P | I2C address 0x2C | keep away from inductors / high-current traces; pins and support network verified against the datasheet |
| ATGM336H-6N-74 | UART0(RXD0/TXD0) → J3; 1PPS → GPIO50; VCC_RF feeds the active antenna | 18-pin LCC, pin diagram verified (manual §2.3) |
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
12. (Onboard-IFA physical-board to-do) Frozen inputs are synced to the footprint/PCB/feedline; the physical board's dF/dL, in-case shift,
    π-network measured values, and receive efficiency still await board bring-up verification.

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
