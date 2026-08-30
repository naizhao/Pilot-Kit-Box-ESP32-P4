# V4.1 Two Variants: Powered / Unpowered

Chinese version: [`VARIANTS-zh_CN.md`](VARIANTS-zh_CN.md)

> 2026-08-23. This file is the **single source of truth for the variant design**; the selective placement
> rules in ASSEMBLY-zh_CN.md also defer to this file.

## 1. Division principle

Not "leave out a few parts", but **two complete, self-contained power systems**:

| | F side | B side |
|---|---|---|
| **Unpowered variant** | All sensors + RP2040 + CC1312R + RF chains + three LDOs | **Zero placed parts** |
| **Powered variant** | Same as above | J4 (USB-C PD) + CH224K + SY6970 + SY7069 + L16/L17 + J9 battery holder + passives |

The unpowered variant **doesn't even fit the battery holder** — power and battery-level info all go through the Waveshare mainboard (`VCC_5V` enters from J1 pin1/pin3,
feeding the three LDOs U1/U2/U3). The `VCC_5V` net is bidirectional: on the powered variant SY7069 outputs and back-feeds J1;
on the unpowered variant it is an input from J1. Same trace, two directions — no jumper or board change needed.

## 2. Why split: the cost is in SMT side count, not component fees

Measured with JLC SMT online quoting (board 10.00×6.20 cm, 5 pcs, 2026-08-23).
The table below uses **relative multiples**; absolute quotes change with time and batch size, so get your own quote before ordering:

| Option | Placement sides | Available tier | Relative cost |
|---|---|---|---|
| Powered variant (697 solder joints / 38 part types) | Double-sided | **Standard only** | **2.43×** (2.43 times the baseline) |
| Unpowered variant (598 solder joints / 32 part types), **still double-sided** | Double-sided | Standard only | 2.30× |
| Unpowered variant (602 solder joints / 33 part types), **single-sided** | Single-sided | **Economy** | **1.00× (baseline)** |
| Same as above on Standard | Single-sided | Standard | 1.57× |

Cost structure (this is the key — the difference is almost entirely fixed fees, not solder-joint count):

```
                 Double-sided Standard   Single-sided Economy
Engineering fee      High        Low      ← main source of the difference
Stencil fee          Yes         No       ← single-sided is stencil-free
Placement joint fee  ≈           ≈        ← essentially identical between the two
Reel change fee (after full reels) ≈  ≈
```

**Two conclusions**:

1. **The savings hinge entirely on unlocking the Economy tier** — JLC's "double-sided placement does not yet support Economy" is a hard gate.
   Single-sided saves only half even on the Standard tier; only the Economy tier captures the full difference.
2. **Without getting to single-sided, splitting into variants is almost pointless** — the two double-sided variants differ by only 5%; the saved placement fees
   are eaten by the fixed engineering fee + stencil fee.

## 3. What counts as "one placement side"

The criterion is **whether that side has parts that need machine placement**, not "whether it has copper":

| | Occupies a side | Note |
|---|---|---|
| SW2 (SolderJumper solder-pad jumper) | **No** | Nothing placed, no process step, no stencil, no oven run |
| Through-hole parts (if J9 switches to bent-pin) | **No** | Counted in the "through-hole" column (hand-soldering fee ¥2 + hand-assembly engineering fee ¥20, fixed) |
| J4 (USB-C, 16 SMD contacts) | **Yes** | As long as it sits on the B side, that side is a placement side |
| The 28 power SMDs | Yes | The powered variant is necessarily double-sided |

So leaving SW2 on the B side has zero cost impact; no adjustment needs to be made for it.

## 4. 【Implemented 2026-08-23】How single-sided usability was achieved

Product principle: **single-sided usability is the floor; double-sided is better**. So the unpowered variant's B side must have **zero placed parts**.

Three things were done:

| Change | Reason |
|---|---|
| **J4 (USB-C) → F-side right board edge** (144.63, 73.00) | If left on the B side, the unpowered variant would still be double-sided, saving ¥35 — a waste of effort. The courtyard's right edge sits flush with the board edge at 150, and the opening still faces +X (same side as the Waveshare H1). `USB_DP/DM` goes through R9/R10, which were already on the F side at x117; after the move the routing is actually shorter |
| **Added R7/R8 (5.1k CC pull-downs) → F side**, 2.2mm right next to J4 | The unpowered variant has no CH224K; with CC floating, the host simply won't recognize the device (the USB-C spec requires Rd on the device side). **Either-or with CH224K**: not placed on the powered variant |
| R37 moved from (138.80,69.84) to (137.75,70.75) | J4's through-hole mounting legs poke through to the B side and landed inside R37's courtyard. This also shrank its distance to the CFG1 pin from 2.97mm to 2.56mm |

**Acceptance criterion: the unpowered variant's B-side placed parts = 0** (only the SW2 solder-pad jumper and non-side-counting TP items remain).

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
