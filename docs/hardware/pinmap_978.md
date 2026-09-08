# Pin map — RP2040 ↔ CC1312R Sub-GHz link (978 MHz)

Evidence-based pin fact sheet for the SUBG_* nets between the RP2040 (U8) and the
CC1312R1F3RGZR Sub-GHz radio (U10) on the expansion board. This sheet is the
hardware authority for the SPI protocol spec and for clock-rate decisions.

Evidence sources:

- Net connectivity extracted from
  `hardware/expansion-board-v4/kicad/expansion-board-v4.kicad_sch` (sheets
  `SubGHz_978` = `subghz.kicad_sch` and `MCU_RP2040` = `mcu.kicad_sch`) using
  KiCad 10.0.4 `kicad-cli sch export netlist`, cross-checked against the pad→net
  assignments in `expansion-board-v4.kicad_pcb` and against v3
  (`hardware/expansion-board-v3/kicad/`). Schematic netlist, PCB netlist and v3
  agree on every row below.
- Pin identities/functions verified against the datasheets in
  `hardware/datasheets/`: `CC1312R.pdf` (TI SWRS210H, revised November 2020) and
  `RP2040.pdf` (Raspberry Pi). Page numbers below are **PDF page numbers of the
  files as stored** (RP2040 printed page = PDF page − 1; CC1312R printed page =
  PDF page).
- Nothing on this sheet is inferred from memory: every pin claim has a citation
  or is listed under "Unresolved".

## Digital interface nets

| Net | RP2040 U8 (pad → GPIO) | RP2040 cite | CC1312R U10 (pad → pin) | CC1312R cite | Notes |
|---|---|---|---|---|---|
| `SUBG_SCK` | 13 → GPIO10 | RP2040.pdf p.613, Table 615 | 16 → DIO_10 | CC1312R.pdf p.7 Fig. 7-1, p.8 Table 7-1 | SPI1 SCK (see below) |
| `SUBG_MOSI` | 14 → GPIO11 | RP2040.pdf p.613, Table 615 | 14 → DIO_8 | CC1312R.pdf p.7 Fig. 7-1, p.8 Table 7-1 | SPI1 TX (controller output) |
| `SUBG_MISO` | 15 → GPIO12 | RP2040.pdf p.613, Table 615 | 15 → DIO_9 | CC1312R.pdf p.7 Fig. 7-1, p.8 Table 7-1 | SPI1 RX (controller input) |
| `SUBG_CSN` | 16 → GPIO13 | RP2040.pdf p.613, Table 615 | 17 → DIO_11 | CC1312R.pdf p.7 Fig. 7-1, p.8 Table 7-1 | SPI1 CSn; R56 10 kΩ pull-up to `3V3_DIG` (pull, **not** series) |
| `SUBG_IRQ` | 17 → GPIO14 | RP2040.pdf p.613, Table 615 | 18 → DIO_12 | CC1312R.pdf p.7 Fig. 7-1, p.8 Table 7-1 | CC1312R → RP2040 interrupt (direction is the firmware contract; both ends are plain GPIO) |
| `SUBG_SYNC` | 18 → GPIO15 | RP2040.pdf p.613, Table 615 | 19 → DIO_13 | CC1312R.pdf p.7 Fig. 7-1, p.8 Table 7-1 | Generic GPIO at both ends — no fixed hardware function (see rulings) |
| `SUBG_RESET` | 29 → GPIO18 | RP2040.pdf p.613–614, Table 615 | 35 → RESET_N | CC1312R.pdf p.8 Table 7-1 | RP2040 drives; R47 10 kΩ pull-up to `3V3_DIG`; RESET_N has **no internal pull-up** (CC1312R.pdf p.8); **also used by cJTAG flash proxy for reset pulse** |
| `SUBG_TCKC` | 28 → GPIO17 | RP2040.pdf p.614, Table 615 | 25 → JTAG_TCKC | CC1312R.pdf p.8 Table 7-1 | cJTAG clock; **RP2040 代刷输出**（`cjtag.c` 位脉冲） |
| `SUBG_TMSC` | 27 → GPIO16 | RP2040.pdf p.614, Table 615 | 24 → JTAG_TMSC | CC1312R.pdf p.8 Table 7-1 | cJTAG data (bidirectional); **RP2040 代刷双向**（`cjtag.c` 位脉冲）; high-drive pin on CC1312R (p.7) |
| `BIAS_EN_978` | 5 → GPIO3 | RP2040.pdf p.613, Table 615 | — (Q2 gate, not a CC1312R pin) | netlist | Antenna DC-bias enable, default off (see rulings) |

Direction contract (design-level, consistent with the pins above): the RP2040 is
the SPI master and owns CSN; the CC1312R is the SPI slave and the interrupt
source; the RP2040 drives `SUBG_RESET` (RESET_N is an input on CC1312R,
CC1312R.pdf p.8).

## SPI instance verdict: SPI1, unique

The four data nets land on GPIO10/11/12/13 (RP2040 pads 13/14/15/16). Per the
GPIO function select table (RP2040.pdf p.238, Table 279, §2.19.2):

| GPIO | pad | SPI function (F1) |
|---|---|---|
| GPIO10 | 13 | **SPI1 SCK** |
| GPIO11 | 14 | **SPI1 TX** (MOSI) |
| GPIO12 | 15 | **SPI1 RX** (MISO) |
| GPIO13 | 16 | **SPI1 CSn** |

- Each of GPIO10–13 carries exactly one SPI function in the whole mux table, and
  it is always SPI1; SPI0 has no function on these pins. The wiring therefore
  admits **SPI1 only** — there is no alternative SPI0 instance on this link.
- This is the *direct/standard* arrangement (SCK/TX/RX/CSn on four consecutive
  GPIOs 10–13). The RP2040 datasheet itself does not use the word "flipped";
  the "flipped" wording known from SDK docs refers to alternative RX/TX splits
  which are not what Table 279 offers on these pins. No RX/TX swap is involved.
- §4.4 SPI (RP2040.pdf p.502) confirms both controllers are ARM PL022 SSPs and
  defers pin assignment to Table 279 (§2.19.2). The summary table on p.13
  (Table 2, §1.4.3) shows the same F1 assignments.
- GPIO13's F1 is the hardware CSn function, so hardware chip-select is wired;
  firmware may still drive CS as plain GPIO (PL022 supports either usage).
- CC1312R side: DIO_8…DIO_13 are generic digital GPIOs (CC1312R.pdf p.8,
  Table 7-1). Which SSI instance and which SSI signal each DIO carries is a
  firmware IOC (I/O controller) routing decision — see Unresolved #1.

## Semantics rulings (naming traps)

- **`SUBG_SYNC` = CC1312R DIO_13 (pad 19), a generic GPIO** (CC1312R.pdf p.8,
  Table 7-1 "GPIO"). It has no dedicated hardware function on the CC1312R — no
  frame-sync, no RF-synchro pin. Any "sync" meaning is a firmware-level protocol
  decision (protocol spec, not hardware). Do not treat it as an SPI signal.
- **`SUBG_SW` is not a signal at all.** It lands on CC1312R pad 33 =
  DCDC_SW, "Output from internal DC/DC converter" (CC1312R.pdf p.8,
  Table 7-1), and runs through L7 (6.8 µH) to `SUBG_VDDR` (VDDR pad 45 /
  VDDR_RF pad 48, CC1312R.pdf p.8–9). C60 (22 µF) + C61/C62 (100 nF) decouple
  `SUBG_VDDR`, matching the datasheet footnote "the inductor between DCDC_SW
  and VDDR… the 22 uF DCDC capacitor must be kept on the VDDR net"
  (CC1312R.pdf p.9, Table 7-2 note 2). The `SUBG_` prefix is misleading; this
  is the DC/DC switch node, an analog power net, and it is **not connected to
  the RP2040**.
- **`SUBG_N3` / `SUBG_N4` / `SUBG_N5` are RF matching-network internal nodes**
  (net class RF50 in the schematic), not digital signals. Topology from the
  netlist: RF_P (pad 1) and RF_N (pad 2) are tied by L10 (27 nH); the
  differential port is converted through L9 (7.5 nH) + C40 (3.6 pF) into N3,
  shunt C41 (2.7 pF), series L11 (6.8 nH) to N4, shunt C42 (6.2 pF), series
  L12 (6.8 nH) to N5, shunt C43 (3 pF), then DC-blocked by C39 (100 pF) onto
  `ANT_978` (U.FL connector J5, ESD shunt D3 TPESD8L3.3). None of N3/N4/N5
  touch the RP2040.
- **`SUBG_RXTX` = CC1312R pad 3 = RX_TX**, "Optional bias pin for the RF LNA"
  (CC1312R.pdf p.8, Table 7-1). It is biased from the RF_N node through
  L13 (7.5 nH) with C45 (100 pF) to ground. Analog RF, not connected to the
  RP2040.
- **`BIAS_EN_978`** (RP2040 pad 5 = GPIO3) gates Q2 (AO3401A P-channel MOSFET,
  SOT-23): source = `3V3_GNSS`, drain → `SUBG_FUSE` → F2 (6 V / 200 mA fuse) →
  `SUBG_FEED` → L8 (100 nH) → `ANT_978`. R17 (10 kΩ) pulls the gate up to
  `3V3_DIG`, so the bias path is **off by default** (P-FET needs a low gate);
  driving GPIO3 low injects DC bias onto the antenna feed line. This is an
  external-antenna bias switch, not a CC1312R signal.

## Power domain and signal integrity

- Both chips' I/O rails sit on the same net: RP2040 IOVDD (U8 pads
  1/10/22/33/42/49) = CC1312R VDDS (U10 pads 13/22/34/44) = `3V3_DIG`.
  CC1312R VDDS accepts 1.8–3.8 V (CC1312R.pdf p.9). **No level shifting is
  present or needed.**
- `SUBG_SCK` / `SUBG_MOSI` / `SUBG_MISO` are direct copper between U8 and U10 —
  **no series resistors, no buffers, no filters** on the SPI data/clock nets.
  The only component on `SUBG_CSN` besides the two chips is the R56 10 kΩ
  pull-up. Relevant to WP-E clock-rate choices: there is no board-level
  attenuator on this link (trace geometry was not extracted; see Unresolved #4).
- Boot-state behaviour: RP2040 GPIOs come out of reset in pull-down state
  (RP2040.pdf p.613 Table 615 "Reset State", pull resistance 50–80 kΩ on
  p.617). The external 10 kΩ pull-ups (R56 on CSN, R47 on RESET_N) dominate
  that weak pull-down (divider result 2.75–2.93 V, computed from
  3.3 V × R_pulldown/(R_pulldown + 10 kΩ) over the 50–80 kΩ reset-state range;
  audit 2026-09-08 reviewer note), keeping CSN deasserted and
  the CC1312R out of reset while the RP2040 boots or its firmware is absent.

## cJTAG boundary

cJTAG debug is 2-wire only on this board: `SUBG_TCKC` (U10 pad 25, JTAG_TCKC)
and `SUBG_TMSC` (U10 pad 24, JTAG_TMSC), driven by RP2040 GPIO17/GPIO16
(pads 28/27). These nets share **no pin** with the SPI1 quad (GPIO10–13), and
the 4-wire JTAG pins DIO_16/JTAG_TDO (pad 26) and DIO_17/JTAG_TDI (pad 27) are
left unconnected (netlist `unconnected-` entries; unused DIO preferred NC per
CC1312R.pdf p.9, Table 7-2). First-flash/debug via the RP2040 therefore has no
pin overlap and no protocol impact on the SPI link.

## v3 ↔ v4 differences: none on this interface

The v3 and v4 schematics carry identical SUBG_* labels (9 on `mcu.kicad_sch`:
SCK/MOSI/MISO/CSN/IRQ/RESET/SYNC/TCKC/TMSC; same label set on the subghz sheet
plus RF/power nets), and the extracted netlists match pad-for-pad:

| Net | v3 | v4 | Match |
|---|---|---|---|
| SUBG_SCK / MOSI / MISO / CSN | U8.13/14/15/16 ↔ U10.16/14/15/17, R56.2 | same | ✓ |
| SUBG_IRQ / SYNC / RESET | U8.17/18/29 ↔ U10.18/19/35, R47.2 | same | ✓ |
| SUBG_TCKC / TMSC | U8.28/27 ↔ U10.25/24 | same | ✓ |
| SUBG_SW / RXTX / N3 / N4 / N5 | U10.33 / 3 / RF network | same | ✓ |
| SUBG_VDDR / DCOUPL / X48P/N / X32K1/2 | U10.45+48 / 23 / 47+46 / 4+5 | same | ✓ |
| BIAS_EN_978 / ANT_978 / RF_N / RF_P | U8.5, Q2, R17 / J5, D3 / U10.2 / U10.1 | same | ✓ |
| Components (U8, U10, R47, R56, R17, Q2, F2, L7–L13, C39–C45) | identical refs + values | identical | ✓ |

Both versions use U8 = RP2040 (QFN-56) and U10 = CC1312R1F3RGZR (QFN-48).
Naming note: `JTAG_TCKC` / `JTAG_TMSC` exist **only as CC1312R symbol pin
names** — there are no nets or labels by that name on either version; the
cJTAG wires are named `SUBG_TCKC` / `SUBG_TMSC` on both sheets.

## Unresolved

1. **CC1312R-side SSI instance and DIO mux.** `CC1312R.pdf` states the device
   has "2× SSI (SPI, MICROWIRE, TI)" (p.1 features, p.3 block diagram) but does
   not document which SSI instance or signal maps to a given DIO; that is the
   IOC pin-mux territory of the CC13xx/CC26xx TRM (SWCU020, not in
   `hardware/datasheets/`). The wiring fixes the *pins*, not the peripheral
   instance — firmware decides. Unresolved: TRM citation for DIO↔SSI routing.
2. **`SUBG_SYNC` protocol semantics.** Hardware evidence supports only
   "generic GPIO on DIO_13 / GPIO15". Any frame- or RF-synchronisation meaning
   must be defined by the protocol spec (WP-P0b Task 2), not derived from the
   schematic.
3. **`SUBG_IRQ` electrical behaviour** (level vs pulse, polarity) — firmware
   domain; the schematic fixes only the pin pair GPIO14 ↔ DIO_12 (pad 18).
4. **PCB trace geometry** (lengths, impedance class beyond the RF50 net-class
   labels) was not extracted from the PCB layout; only the absence of
   series/level-shift components on the SPI nets is established here.
