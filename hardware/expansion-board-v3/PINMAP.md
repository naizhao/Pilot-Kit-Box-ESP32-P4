# Expansion Board V3.9 — Authoritative Pin/Net Mapping (single source of truth over the schematic)

Chinese version: [`PINMAP-zh_CN.md`](PINMAP-zh_CN.md)

> Rule: this table exists before the schematic; wherever the schematic/PCB conflicts with this table, this table wins and the schematic must be revised to match.
> Specific device pin numbers (QFN48 etc.) are filled in from datasheets when symbols are built in stage 2, and re-verified pin by pin per the PAD_NET lesson.
> Source tags: 〔A〕= topology from a similar reference-design schematic (factual reference only); 〔F〕= existing definitions in this repo's firmware; 〔D〕= this project's decision.

## 1. Interface to the main board: 2×20 female header, plugs straight into the Waveshare carrier board J3 (HAT-style stacking) 〔D〕

Only pins actually used by the expansion board are listed; the rest are left as empty vias:

| J3 Pin | P4 GPIO | Net | Direction (from P4's perspective) | Basis |
|---|---|---|---|---|
| 1, 3 (end of bottom row) | — | VCC_5V | power input | verified against the full bottom-row table of the carrier board pinout 〔F〕 |
| 5,10,13,19,26,29,33,40 | — | GND | — | top row 40/26/10 + bottom row 33/29/19/13/5 〔F〕 |
| 4 | GPIO7 | I2C_SDA | bidirectional | imu_task.c:49〔F〕 |
| 6 | GPIO8 | I2C_SCL | output | imu_task.c:50〔F〕 |
| 32 | GPIO49 | GNSS_RXD (P4 TX → GNSS) | output | gps_task.c:20〔F〕 |
| 36 | GPIO51 | GNSS_TXD (GNSS → P4 RX) | input | gps_task.c:21〔F〕 |
| 34 | GPIO50 | GNSS_PPS | input | gps_task.c:21 comment 〔F〕 |
| 28 | GPIO34 | IMU_INT | input | imu_task.c:56〔F〕 |
| 16 | GPIO28 | IMU_RST | output | imu_task.c:57〔F〕 |
| 24 | GPIO31 | BARO_INT (optional) | input | baro_task.c:32〔F〕 |
| 35 | GPIO46 | ADSB_TXD (RP2040 → P4 RX) **new** | input | carrier board marks as free 〔F〕+〔D〕 |
| 31 | GPIO32 | ADSB_RXD (P4 TX → RP2040, config/control) **new** | output | carrier board marks as free 〔F〕+〔D〕 |

Firmware-side workload: only one new UART driver (GPIO46/32); zero changes anywhere else.

## 2. RP2040 pin assignment 〔D; PIO grouping aligned with A's architecture〕

| RP2040 GPIO | Net | Function |
|---|---|---|
| GPIO0 / GPIO1 | ADSB_TXD / ADSB_RXD | UART0 → P4 (GDL90/raw messages + control) |
| GPIO2 / GPIO3 | BIAS_EN_1090 / BIAS_EN_978 | antenna bias Tee PMOS gates (active-low, 10k pull-up defaults to off) 〔D〕 |
| GPIO10 / 11 / 12 / 13 | SUBG_SCK / SUBG_MOSI / SUBG_MISO / SUBG_CSN | SPI1 master → CC1312R slave 〔A〕 |
| GPIO14 / 15 | SUBG_IRQ / SUBG_SYNC | CC1312R interrupt + sync/bootloader trigger 〔A〕 |
| GPIO16 / 17 / 18 | SUBG_TMSC / SUBG_TCKC / SUBG_RESET | cJTAG for reflashing CC1312R firmware 〔A〕 |
| GPIO19 | PULSES | comparator output → PIO0 preamble detection 〔A〕 |
| GPIO20–23 | DEMOD0–3 | PIO demod state-machine handshake/debug 〔A〕 |
| GPIO24 | RECOVERED_CLK | recovered clock for debug 〔A〕 |
| GPIO25 | TL_PWM | comparator threshold PWM (RC-filtered into LEVEL_BIAS) 〔A〕 |
| GPIO26 / ADC0 | LEVEL_BIAS_SENSE | DC readback of the threshold 〔A〕 |
| GPIO27 / ADC1 | RSSI | AD8313 VOUT readback (adaptive sensitivity) 〔A〕 |
| GPIO28 / ADC2 | Spare ADC | reserved (e.g. 978 RSSI) |
| QSPI dedicated pins | W25Q128JVSIQ | 16MB firmware + config |
| XIN | 12MHz crystal (C9002) | |
| USB_DP/DM | USB-C receptacle | UF2 drag-and-drop flashing + CDC debug 〔D〕 |
| RUN + BOOTSEL | tactile switches ×2 | essential for a dev board 〔D〕 |
| SWD (SWCLK/SWDIO) | 3-pin test points | debug fallback 〔D〕 |

## 3. CC1312R (QFN48) logical assignment 〔D; DIO→pin numbers to be filled from the datasheet when building the library part〕

| Logical signal | CC1312R side | Counterpart |
|---|---|---|
| SPI slave: SCLK/MOSI/MISO/CSN | DIO10 / DIO8 / DIO9 / DIO11 | RP2040 SPI1 |
| IRQ / SYNC | DIO12 / DIO13 | RP2040 GPIO14/15 |
| cJTAG TMSC / TCKC | dedicated JTAG_TMSC / JTAG_TCKC pins | RP2040 GPIO16/17 |
| RESET_N | dedicated pin | RP2040 GPIO18 |
| RF_P / RF_N | differential RF | LC matching network → J3 (U.FL 978) |
| X48M | 48MHz crystal (Abracon ABM8W 7pF preferred; KDS 12pF pending verification of the on-chip cap array range) | |
| X32K | 32.768kHz (EPSON FC-135) | |

978 differential matching starting values 〔A〕: L 7.5nH / 27nH / 6.8nH×2; C 3.6pF×2 / 2.7pF / 6.2pF. With the 915→978 frequency shift, V1 leaves all positions tunable.

## 4. Power tree 〔D; one extra split RF LDO compared to A〕

```
J3 VCC_5V ──┬── ME6211C33 (500mA) ──→ 3V3_DIG: RP2040, Flash, CC1312R (via ferrite bead), BNO085, BMP388, QMC5883P
            ├── TPS7A2033 #1 ──────→ 3V3_RF: QPL9547, BGA2817, AD8313, MCP comparator domain
            └── TPS7A2033 #2 ──────→ 3V3_GNSS: ATGM336H-6N-74 + both antenna bias Tees
Bias Tee (×2: 1090 / 978) 〔A〕: PMOS high-side switch + 6V/200mA fuse + 100nH feed inductor + ESD (0.6pF class)
```

## 5. 1090 RX chain, net by net 〔A topology + our component choices〕

```
ANT1090(U.FL) → bias Tee → C 100pF DC block → L 27nH series + 82nH bias feed → QPL9547(LNA①)
→ C 12pF → TA0970A(SAW①) → matching as above → BGA2817(LNA②) → C 12pF → TA0970A(SAW②)
→ MM8930-2620RJ4 (production test socket, in series) → C 3pF coupling → AD8313 (sole detector; the AD8319 branch was removed in V3.9)
→ RSSI/VOUT ─┬→ RP2040 ADC1
             └→ TLV3501 IN+; IN- = LEVEL_BIAS (TL_PWM via 100k+0.1µF RC) + 10k hysteresis 〔A〕
→ TLV3501 OUT = PULSES → RP2040 GPIO19
Test points: W.FL ×2 (after LNA② / detector input) 〔A〕
```

All LC values are 〔A〕 starting points; V1 keeps a 0402 tunable position at every designator. After the SAW part change (TFS1090F→TA0970A), the matching values must be tuned on the actual board.

## 6. Sensors / GNSS (wired direct to P4, not through the RP2040) 〔F — architecture unchanged〕

| Device | Bus/pins | Notes |
|---|---|---|
| BNO085 | I2C address 0x4A + IMU_INT(GPIO34) + IMU_RST(GPIO28) | PS0/PS1 and SA0 are tied low; CLKSEL0 and H_CSN are tied high; V3.9 footprint rotation is 0° |
| BMP388 | I2C addr 0x76 (baro_task.c:29) + BARO_INT(GPIO31) | case keeps a vent hole |
| QMC5883P | I2C address 0x2C | keep away from inductors / high-current traces; pins and support network verified against the datasheet |
| ATGM336H-6N-74 | UART0(RXD0/TXD0) → J3; 1PPS → GPIO50; VCC_RF feeds the active antenna | 18-pin LCC, pinout verified (manual §2.3) |
| GNSS antenna | third U.FL | V1 all-external antennas |

## 7. Known conflicts / items to verify

1. ~~BNO085 I2C address and mode straps~~ **verified in the exported netlist: SA0/PS0/PS1=GND, address 0x4A, I2C mode.**
2. CC1312R on-chip 48M load-capacitance array range → decides Abracon 7pF or KDS 12pF.
3. TA0970A input/output impedance and matching values (recalculate once the datasheet is in hand; current values are placeholders only).
4. After stacking the J3 header, interference check between the USB-C (RP2040 flashing port) opening direction and the 4.3-inch case (docs/jlc/lcd-4.3in/3d-case) — done at the PCB stage.
5. Distance constraint between QMC5883P and RP2040/DCDC (magnetometer clean zone) — to be defined in the PCB-stage placement constraint doc.
6. ~~QPL9547 EN pin polarity~~ **verified (Qorvo DS Rev.D): SD pin <0.63V = LNA ON, ground-enable is correct**.
7. (added at schematic stage) TLV3501 SHDN pulled high to enable (per TI, low = shutdown); cross-check polarity consistency on the TOKMAS version.
8. (added at schematic stage) the official KiCad CC1312R symbol has no DIO_0 — before PCB layout, re-verify every QFN48 pin against TI SWRS210.
9. ~~AD8319 CLPF left floating~~ — the AD8319 branch was removed in V3.9; the AD8313 has no equivalent pin, so this note is void.
10. ~~no ESD device on the 978 antenna port~~ **handled: D3 added to the schematic (aligned with 1090; exposed ports on an aviation product always get ESD)**.
11. (added at schematic stage) the asymmetric 12/15pF 32k crystal load capacitors were copied as-is from the reference design; symmetric would be normal — item for review discussion.

## 8. Component version verification checklist (user requirement: ensure datasheet version = actually purchased version; placement allowed only after every item passes incoming inspection)

| Device | Datasheet version the symbol was built from | Purchase SKU | Incoming inspection points |
|---|---|---|---|
| BNO085 | CEVA 1000-3927 **v1.16** (BNO080/085/086 share the same pinout) | Taobao ¥56 tier, loose new | silkscreen "BNO085" (beware "BN0085" counterfeit markings); measure the actual LGA-28 3.8×5.2mm package |

| AD8313 | ADI Rev.F | LCSC C578690 (only this channel) | silkscreen "J1A"; MSOP-8 |
| BMP388 | Bosch DS001-07 Rev 1.7 | LCSC C779278 (factory) | LGA-10 2×2mm; sample-test CHIP_ID=0x50 on arrival |
| QMC5883P | QST 13-52-19 RevA (**the same document linked on the LCSC C2847467 product page**) | Taobao ¥5.5 tier or LCSC | LGA-16 **3×3mm** (do not mix up with the L-version small package); responds on I2C 0x2C |
| ATGM336H-6N-74 | ICOMSpass 6N user manual (pin table §2.3) + 5N manual land pattern (§2.2; both generations share the same package, pin-compatible) | Taobao ¥11 tier (look for the "full-constellation/GALILEO" wording) | silkscreen contains "6N-74"; on power-up the NMEA GSV sentences should include GA (Galileo) — the hardest evidence of single-BD/full-constellation firmware |
| MM8930-2620 | Murata O30E catalog, 2025-12 edition (RJ4/RK15 differ only in tape-and-reel packaging, same part) | LCSC C6227587 (RJ4) | 1.6×1.6mm; straight-through continuity test (R–C conducts with no probe inserted) |
| TA0970A | TST Rev 3.0 (**6 pads**) | LCSC C7115531 (listed as "8P"; conflict has been filed) | **First thing on arrival: count the pads.** If the physical part has 8 pads, the footprint is voided and redrawn; hold the PCB order until the physical part is confirmed |
| CC1312R1F3RGZR | official KiCad symbol (verify pin by pin against TI SWRS210 when drawing the schematic) | Taobao ¥11 original tier | silkscreen CC1312R1F3; QFN48 7×7 |
| W25Q128JVSIQ | Winbond JV series (official symbol W25Q128JVS) | LCSC C97521 | SOIC-8 208mil wide body (a footprint-selection point; do not draw it as 150mil) |

**Rule**: any part that fails verification never goes on the board; until TA0970A is physically confirmed, the PCB does not go to production.