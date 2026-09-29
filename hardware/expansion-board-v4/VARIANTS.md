# V4.11 Two Variants: Powered / Unpowered

Chinese version: [`VARIANTS-zh_CN.md`](VARIANTS-zh_CN.md)

> Updated for the frozen V4.11 board on 2026-09-29. This file is the **single source of truth for the variant design**; the selective placement
> rules in ASSEMBLY-zh_CN.md also defer to this file.

## 1. Division principle

Not "leave out a few parts", but **two complete, self-contained power systems**:

| | F side | B side |
|---|---|---|
| **Unpowered variant** | All sensors + RP2040 + CC1312R + RF chains + three LDOs | **Zero placed parts** |
| **Powered variant** | Same as above + the power main section (all except `BT1`/`J9`/`RT1`) | Only `BT1`, `J9` and `RT1`, all hand-soldered after F-side SMT |

The unpowered variant **doesn't even fit the battery holder** — power and battery-level info all go through the Waveshare mainboard (`VCC_5V` enters from J1 pin1/pin3,
feeding the three LDOs U1/U2/U3). The `VCC_5V` net is bidirectional: on the powered variant SY7069 outputs and back-feeds J1;
on the unpowered variant it is an input from J1. Same trace, two directions — no jumper or board change needed.

> **V4.6 airband and the variants**: the airband group (stage G — `U21` `U22` `Y4` `J10` and passives) is
> placed on **both** variants; it is not part of the variant split. Its analog 5 V (`VHF_AVCC`, via `R60`)
> comes from `VCC_5V`, so on the unpowered variant airband depends on the carrier feeding 5 V into J1 pin1/pin3.
> Everything else in the group runs off `3V3_RF`.

## 2. Current SMT-side result

The **F side carries the power section** on V4.11. The released V4.11 CPL files contain
only `T` (top/F-side) rows. `BT1`, `J9` and `RT1` are the only power-section references on
the B side; none is in the current CPL, and all three are hand-soldered. Therefore both variants
can use single-sided SMT. The powered variant is not a double-sided SMT build.

| Option | Machine placement | B-side follow-up |
|---|---|---|
| Powered variant | **F side only** | Hand-solder `BT1`, `J9` and `RT1` |
| Unpowered variant | **F side only** | None |

The relative-price table previously kept here was a 2026-08-23 quote for an older placement
state in which the power block was described as B-side SMT. It does not describe the frozen
V4.11 board and must not be used for ordering. Request a fresh quote from the current CPL.

The variant switch itself is the **39-designator** `board_meta.PWR_REFS` set:

`BT1` `C70` `C71` `C72` `C73` `C74` `C75` `C76` `C77` `C78` `C79` `C80`
`C101` `C102` `D7` `D9` `D10` `J9` `L16` `L17` `R37` `R38` `R39` `R40`
`R41` `R42` `R43` `R44` `R45` `R46` `R63` `R65` `R67` `RT1` `SW3` `U18`
`U19` `U20` `U23`.

The powered variant places all 39 and omits `R7`/`R8`; the unpowered variant omits all
39 and places `R7`/`R8`. This remains an electrical rule even though both SMT outputs are
single-sided.

## 3. What counts as "one placement side"

The criterion is **whether that side has parts that need machine placement**, not "whether it has copper":

| | Occupies a side | Note |
|---|---|---|
| Pad-only items: the J11 debug pads (V4.10 2x10 bare pads; they replaced the SW2 solder jumper, removed in V4.9) | **No** | Nothing placed, no process step, no stencil, no oven run |
| `BT1` / `J9` / `RT1` on the B side | **No machine-placement side** | Deliberately excluded from CPL; hand-solder after F-side SMT |
| `J4` (USB-C, 16 SMD contacts) | **Yes, F side** | Present on both variants |
| Power section (39-designator variant set) | **F side SMT only** | 36 references are on F; only `BT1`/`J9`/`RT1` are on B and hand-soldered |

So leaving pad-only J11 on the B side has zero cost impact. The same is true of the two
explicit hand-solder exceptions `BT1`/`J9`/`RT1`; none creates a B-side SMT run.

## 4. 【Implemented 2026-08-23】How single-sided usability was achieved

Product principle: **single-sided SMT is the floor**. The unpowered variant's B side has
zero parts; the powered variant's B side has only the three hand-solder parts `BT1`/`J9`/`RT1`.

Three things were done:

| Change | Reason |
|---|---|
| **J4 (USB-C) → F-side right board edge** (144.63, 73.00) | If left on the B side, the unpowered variant would still be double-sided, saving ¥35 — a waste of effort. The courtyard's right edge sits flush with the board edge at 150, and the opening still faces +X (same side as the Waveshare H1). `USB_DP/DM` goes through R9/R10, which were already on the F side at x117; after the move the routing is actually shorter |
| **Added R7/R8 (5.1k CC pull-downs) → F side**, 2.2mm right next to J4 | The unpowered variant has no CH224K; with CC floating, the host simply won't recognize the device (the USB-C spec requires Rd on the device side). **Either-or with CH224K**: not placed on the powered variant |
| R37 moved from (138.80,69.84) to (137.75,70.75) | J4's through-hole mounting legs poke through to the B side and landed inside R37's courtyard. This also shrank its distance to the CFG1 pin from 2.97mm to 2.56mm |

**Acceptance criterion:** every released V4.11 CPL row is F-side. The unpowered variant's
B-side part count is zero; the powered variant's B-side exceptions are exactly `BT1`, `J9`
and `RT1`, all hand-soldered (pad-only J11 does not count as a part).

The proxy-flashing scheme (former §4 Option A) was rejected; reasons in §5 — GPIO24/25 are USB Serial/JTAG and cannot do host,
and switching requires burning an irreversible eFuse. No gamble taken; J4 stays.


## 5. Verification record: why GPIO24/25 cannot be used

**The former wrong scheme**: wire the RP2040's USB to J1 pin23/21 and let the P4 act as USB host for proxy flashing.
After checking the ESP32-P4 datasheet, **rejected**:

| USB peripheral | Default pins | Can be host | Brought out on J3? |
|---|---|---|---|
| **USB Serial/JTAG** | **GPIO24 / 25** | ❌ device only | ✅ pin23/21 |
| USB 2.0 **Full-Speed OTG** | **GPIO26 / 27** | ✅ | ❌ **not brought out** |
| USB 2.0 High-Speed OTG | dedicated pins 49/50 | ✅ | ✅ pin27/25 (same nets as H2) |

- The GPIO24/25 brought out on the Waveshare J3-23/21 are **USB Serial/JTAG**, device-only, cannot act as host
- OTG FS defaults to GPIO26/27, and **J3 does not bring out those two pins**
- Switching OTG FS to GPIO24/25 requires burning the eFuse `USB_PHY_SEL` — **one-time and irreversible, and once burned the P4's USB-JTAG is dead**; besides, the Waveshare board already ships from the factory, not under our control

**Only the HS group (J3-27/25) remains**, but it shares nets with H2's USB-C port, and the carrier board's USB-A (the RTL-SDR option)
also runs on that line; of the three parties only one can be chosen.

## 6. Why the C6 needs manual UART flashing while the RP2040 doesn't

This question decides whether proxy flashing is actually dependable; the answer is **the two mechanisms are completely different**:

**The C6 (see `docs/hardware/c6_slave_firmware-zh_CN.md`)** needs an external USB-UART hooked to the P1 pin header,
IO9 shorted to ground, and esptool to write the flash. Two reasons:

1. **Hardware**: Waveshare routes the C6's UART to a separate P1 pin header, not to the P4's GPIOs — the P4 cannot reach it
2. **Chicken-and-egg**: P4↔C6 runs ESP-Hosted over SDIO, and for SDIO to come up, the hosted firmware must already be running on
   the C6. A blank C6 has no firmware → SDIO won't come up → the only path left is the ROM's UART bootloader,
   which needs an external tool for the handshake. So the **first** flash must be manual; only after that can OTA be used.

**The RP2040 doesn't have this problem**: BOOTSEL is baked into ROM; powering up a blank chip (or shorting QSPI_SS) turns it
directly into a USB MSC thumb drive, with no pre-installed firmware and no handshake protocol needed. This is an architectural difference,
not a configuration issue.

## 7. After a mainboard swap (e.g. RK3506), does proxy flashing still hold

Proxy flashing depends on exactly one thing: **the upstream has a USB host + can mount MSC**.

| Swap to | Proxy flashing still works? |
|---|---|
| RK3506 and other ARM Linux SoCs | ✅ even simpler than the P4, `mount` + `cp x.uf2` |
| Raspberry Pi CM / any Linux/Android mainboard | ✅ |
| A pure MCU with no USB host | ❌ |

The Pilot Kit has to run a Flutter UI, so the main SoC will only get stronger with each swap; this risk is low. And J1 is a 2.54mm pin header,
**so at any time you can run two jumper wires from the header to a USB adapter and connect straight to a computer**; no USB receptacle is needed on the board,
and no SMT side count is consumed — this is the fallback path that does not depend on the upstream board.

## 8. R7/R8 (5.1k CC pull-downs) are mandatory selective-placement parts

For a while it seemed unnecessary — that rested on the assumption that "the unpowered variant has no J4". But per §4, J4 is placed on both variants
(otherwise there is no way to flash the RP2040 firmware), so CC must have Rd:

| | R7/R8 | CH224K |
|---|---|---|
| Powered variant | ❌ not placed | ✅ placed; Rd built in |
| Unpowered variant | ✅ **must place** | ❌ not placed |

**The two are mutually exclusive**: placing both would scramble the CC resistance and interfere with PD communication (in the Type-C
receptacle reference circuit of the CH224 manual §6.1, CC connects straight to the chip with no external pull-down).
