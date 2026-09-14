# Pilot Kit Avionics Board V4.8

**Integrated ADS-B / GNSS / IMU expansion board for the Pilot Kit Box**
集成 ADS-B / GNSS / IMU 扩展板

<p align="center">
  <img src="render/top.png" alt="Expansion board V4.8 top side" width="900">
</p>

A 6-layer, 100.1 × 62.1 mm HAT that stacks onto the Waveshare
ESP32-P4-WIFI6-Touch-LCD-4.3 carrier's 2×20 header. It replaces the four
discrete modules used by the current prototype (RTL-SDR dongle, BNO085,
GT-U8 GNSS, BMP388) with a single board — **including its own 1090 MHz
receive chain, so no RTL-SDR dongle is required.**

6 层、100.1 × 62.1 mm，以 HAT 方式直插微雪 ESP32-P4-WIFI6-Touch-LCD-4.3
载板的 2×20 排母，把现有原型的四个分立模块（RTL-SDR dongle、BNO085、
GT-U8 GNSS、BMP388）整合成一块板，**自带 1090 MHz 接收链，不再需要 dongle**。

---

## ⚠️ Status / 当前状态

> **V4.4 has been fabricated and operated; V4.8 is the current design under review.**
> V4.8 adds a permanently-powered backup rail for the GNSS module's VBAT pin
> (`U23` LDO off `BAT_P` -> `3V3_BKP` -> `D7` -> `GNSS_VBAT`, plus the `BT1`
> MS621FE-FL11E rechargeable cell through `R63`), so the module keeps its RTC
> and ephemeris across power-off instead of cold-starting on every boot.
> V4.8 also adds the board's first two status LEDs — `D8` power (on 3V3_DIG,
> extinguishes by itself at power-off) and `D9` charge, driven directly by the
> SY6970 open-drain `STAT` pin (lit while charging, off when done, **blinking at
> 1 Hz on any charge fault**). Neither consumes a GPIO; `R64`/`R65` limit both to
> about 1 mA. They sit on the X+ wall between the USB-C receptacle and the power
> button, so the enclosure needs a matching opening on that wall.
> V4.7 added a hardware power button (`SW3` on the SY6970 `/QON` pin;
> BATFET reset requires battery-only operation) and the airband
> reference-clock DC-block (`C99` + `R61`). V4.6 added the 118–137 MHz airband
> receive chain (BK4819 + 26 MHz TCXO + PCM1808 I²S capture, stage G of the
> assembly list). V4.5 carried forward the
> V4.3 electrical corrections and the V4.0 measured 50.0mm IFA geometry, and
> fixed GNSS active-antenna detection: the module's `VCC_RF` (U7.14) now feeds
> the two bias-tee PMOS sources instead of being left unconnected, so `$GPTXT`
> can report `ANTENNA OK/OPEN/SHORT`. V4.7 has DRC 0 violations / 0 unconnected, current zone fills,
> and ERC 0 errors / 10 warnings. Two C84 sub-assertions in the existing component
> tests fail; C84 is outside this review's agreed change scope and was not modified.
> The user confirmed a working V4.4 board; V4.7 airband and power-button behavior
> still need hardware validation. The existing 3.3 V supply to Y4 is accepted for this prototype; U22 VREF now has C100 (100 nF / 0603) in parallel with C92 (10 µF). Owners of V4.0 boards must follow the
> internal rework record before powering the 1090 MHz chain.
>
> **V4.4 已回板并运行；V4.8 是当前待审定的设计版本。** V4.8 = V4.7 + GNSS 模块 VBAT 常供电
> （`U23` LDO 取自 `BAT_P` -> `3V3_BKP` -> `D7` -> `GNSS_VBAT`，并经 `R63` 挂 `BT1`
> MS621FE-FL11E 可充电电池）：模块关机后保住 RTC 与星历，不再每次开机冷启动。
> V4.8 另有全板第一次出现的两颗指示灯：`D8` 电源（接 3V3_DIG，关机自灭）、
> `D9` 充电（直接由 SY6970 的开漏 `STAT` 脚驱动：充电中亮、充满灭、
> **充电故障时 1Hz 闪**）。两颗都不占 GPIO，`R64`/`R65` 限流到约 1mA。
> 位置在 X+ 壁的 USB-C 口与电源键之间，外壳需在该壁配套开孔。
> V4.7 = V4.6 + 硬件电源键
> （`SW3` 接 SY6970 的 `/QON`；BATFET 复位有仅电池供电的条件）
> + 航空波段参考时钟的隔直（`C99` + `R61`）。V4.6 = V4.5 + 118–137MHz 航空波段接收
> （BK4819 + 26MHz TCXO + PCM1808 I²S 采集，对应贴片清单阶段 G）。V4.5 继承 V4.3 的
> 电气修正和 V4.0 逐刀实测定型的 50.0mm IFA 几何，并修复 GNSS 有源天线检测：模块
> `VCC_RF`(U7.14) 不再悬空，改为两路偏置 Tee PMOS 的源极电源，模块据此才能输出
> `ANTENNA OK/OPEN/SHORT`。V4.7 当前 DRC 0 违例 / 0 未连通，覆铜为最新；ERC 0 error / 10 warning。
> 既有元件测试的 C84 两条子断言失败，按用户范围未修改 C84 或测试。用户已确认 V4.4 实板运行良好，
> V4.7 航空波段与电源键仍待实板验证；用户接受本轮样板保留 Y4 的3.3V供电，回板验证；U22 VREF 已增加 C100（100nF／0603），与 C92（10µF）并联。
> V4.0 实物在开启 1090 MHz 链路前必须先按内部返修记录处理。

| | Status / 状态 |
|---|---|
| Schematic / 原理图 | ✅ Complete / 完成 |
| Layout & routing / 布局布线 | ✅ Complete — 0 DRC violations, 0 unconnected nets / 零违例、未连通 0 |
| Manufacturing files / 制造文件 | ✅ Gerber + drill export cleanly / 可正常导出 |
| **Fabrication / 打样** | ⚠️ V4.0 fabricated; V4.8 not yet fabricated / V4.0 已打样，V4.8 待打样 |
| **Bring-up / 上电验证** | ⚠️ V4.0 rework procedure prepared; V4.8 pending / V4.0 返修流程已就绪，V4.8 待验证 |
| **Airband RX / 航空波段接收** | ⚠️ New in V4.6, never fabricated or bench-tested / V4.6 新增，未打样未台架验证 |
| **Power button / 电源键** | ⚠️ New in V4.7 (`SW3` → SY6970 `/QON`), never tested / V4.7 新增（`SW3` 接 SY6970 `/QON`），未验证 |
| **On-board IFA antenna / 板载 IFA 天线** | ✅ V4.0 per-cut measured 50.0mm geometry transferred / V4.0 逐刀实测 50.0mm 几何已回灌 |

**About the IFA antenna** — V4.7 keeps the V4.0 physical-board result: a
**50.0mm copper outer envelope** (48.5mm radiating-arm centerline span), with
the matching network straight-through (`ZS1`=0R, `ZP1`/`ZP2`=DNP). The V4.0
board was trimmed cut-by-cut from 53.5mm and stopped at 50.0mm, where it
measured **1082.5MHz, SWR 1.09, 45+j0.5Ω** in the case with the battery
fitted. The same series yields a slope of **24–25 MHz/mm**. These values
validate the transferred geometry, but each new enclosure/material batch
still needs a VNA spot check. See [`BOM_IFA_TUNING.md`](BOM_IFA_TUNING.md).

板载 1090MHz 倒 F 天线采用 V4.0 实板逐刀定型结果：**铜箔外包络 50.0mm**（辐射臂
中心线跨度 48.5mm），匹配网络直通（`ZS1`=0R、`ZP1`/`ZP2`=DNP）。该板由 53.5mm
逐刀切短并在 50.0mm 停刀，装盒且装电池实测 **1082.5MHz、SWR 1.09、45+j0.5Ω**；
同一组序列给出斜率 **24–25MHz/mm**。
该结果已回灌至 V4.7，但新外壳或板材批次仍应抽样用 VNA 复核。

---

## Specifications / 规格

| | |
|---|---|
| **Outline / Layers** | 100.1 × 62.1 mm, 6 layers |
| **Stackup** | Designed for 0.0994 mm L1→In1 (single-ply 3313); any 0.085–0.115 mm works as-is — see below |
| **Layer usage** | F.Cu signal · In1 solid GND (RF reference) · In2 signal · In3 3V3 plane · In4 solid GND · B.Cu signal |
| **1090 MHz RX** | QPL9547 LNA → TA0970A SAW → BGA2817 → AD8313 log detector → TLV3501 comparator → RP2040 PIO decoder |
| **978 MHz UAT** | CC1312R1F3RGZR sub-GHz transceiver, differential LC match |
| **118–137 MHz airband RX** | BK4819 AM receiver (26 MHz TCXO reference) → PCM1808 24-bit I²S capture → P4's second I²S. Receive only: `PAOUT`/`VRAMP` unconnected, no PA |
| **Antennas** | On-board 1090 IFA + 4 external U.FL (1090 ext `J6` / 978 `J5` / GNSS ext `J2` / airband `J10`), PMOS-switched bias tees; plus `J8` to the internal GNSS patch and `J7` IFA test port |
| **GNSS** | ATGM336H-6N-74, switchable internal patch / external antenna; module `VCC_RF` (U7.14) feeds the bias tees for active-antenna detection |
| **Sensors** | BNO085 IMU (100 Hz fusion), BMP388 barometer, QMC5883P magnetometer |
| **Power** (optional) | CH224K USB-PD sink @9 V → SY6970 charger + fuel gauge → SY7069 5 V boost, single-cell Li-po |
| **Host interface** | 2×20 SMD header, HAT-style stacking onto the carrier |
| **Scale** | 232 placements (240 designators, 8 not placed), 495 vias (all 0.30 mm drill, 0.45/0.50 mm pad), 1867 track segments |

Two assembly variants — with and without the on-board power section.
See [`VARIANTS.md`](VARIANTS.md).
两个装配变体（带/不带板载电源），选贴规则见该文档。

<p align="center">
  <img src="render/bottom.png" alt="Expansion board V4.8 bottom side" width="900">
</p>

---

## Documentation / 文档

Every board document ships in two languages: the English original
(`*.md`) and the Simplified-Chinese working notes (`*-zh_CN.md`) it was
authored in. Tables, part numbers, and net names are identical in both.

| Document | Contents | Lang |
|---|---|---|
| [`PINMAP.md`](PINMAP.md) | Authoritative pin and net map — the single source of truth for the schematic | EN / 中文 |
| [`VARIANTS.md`](VARIANTS.md) | Powered vs. unpowered assembly variants (inverted logic — read before ordering) | EN / 中文 |
| [`ASSEMBLY.md`](ASSEMBLY.md) | Hand-assembly placement list, generated from the board | EN / 中文 |
| [`ASSEMBLY_MAP-V4.8.pdf`](ASSEMBLY_MAP-V4.8.pdf) / [`ASSEMBLY_MAP-V4.8-zh_CN.pdf`](ASSEMBLY_MAP-V4.8-zh_CN.pdf) | Color-coded full-board assembly maps (placement semantics, three pages each) | PDF |
| [`SELECTIVE_PLACEMENT.md`](SELECTIVE_PLACEMENT.md) | Selective-placement groups and **mutex rules** — which parts must never be populated together | EN / 中文 |
| [`CHECKLIST.md`](CHECKLIST.md) | Per-reference BOM checklist for hand placement (also [.xlsx](CHECKLIST-V4.8.xlsx) / [.pdf](CHECKLIST-V4.8.pdf)) | EN / 中文 |
| [`BOM_PURCHASE-zh_CN.md`](BOM_PURCHASE-zh_CN.md) | Authoritative purchasing list, generated from the netlist | 中文 |
| [`BOM_IFA_TUNING.md`](BOM_IFA_TUNING.md) | Antenna tuning kit and the VNA procedure | EN / 中文 |
| [`BASEBOARD_REF.md`](BASEBOARD_REF.md) | Mechanical reference for the Waveshare carrier | EN / 中文 |

---

## Manufacturing package / 制造包

Pre-built packages live in [`release/`](release/):

> 本次发布为 V4.8，两项相对 V4.7 的功能性差异都**已进板并布通**：
> ① GNSS 模块 VBAT 常供电（`U23`/`C101`/`C102`/`D7`/`R63`/`BT1`，且 `U7.pin6`
> 由 `3V3_GNSS` 改接 `GNSS_VBAT`）；② 指示灯 `D8` 电源 / `D9` 充电（`R64`/`R65` 限流）。
> BOM、图纸、安装助手与源包均已包含。
> 上一版 README 中「备用供电尚未进板、本次发布排除」的说法对应 V4.7，已不再适用。
>
> ⚠️ 需手焊、不在任何 CPL 里的三个件：`BT1`（B 面，经济型 SMT 只贴一面）、
> `D8`/`D9`（自备 LED，无嘉立创料号，且只能用 Vf 2.0~2.2V 的红/黄/橙/普绿——
> 蓝、翠绿、白是 3.0~3.1V，在 3.3V 轨上点不亮）。

**Regenerated on 2026-09-14 from the revised PCB — adds the `D8`/`D9` status LEDs on top of the GNSS VBAT backup rail, with C99 = 100 nF and the finalized C100 VREF decoupling layout. Release files are synchronized. The user has accepted retaining Y4 on 3.3 V for this prototype, with bench verification; this is no longer a required PCB change for this build. U22 VREF now includes C100 = 100 nF in parallel with C92.**

| File | Use |
|---|---|
| `expansion-board-v4-gerber-JLC-V4.8-20260914.zip` | **Current V4.8 export** — adds the GNSS VBAT backup rail and the two status LEDs; RS-274-X, 15 files; see the open electrical items below before ordering |
| `expansion-board-v4-kicad-V4.8-20260914.zip` | Source matching the current PCB byte for byte; supplied for review, not uploaded to the fab |
| `expansion-board-v4-gerber-JLC-V4.7-20260913.zip` | ⚠️ Superseded by V4.8 — no GNSS VBAT backup, the module cold-starts on every power-up. Do not order |
| `expansion-board-v4-kicad-V4.7-20260913.zip` | ⚠️ Matching superseded V4.7 source archive |
| `expansion-board-v4-gerber-JLC-V4.6-20260911.zip` | ⚠️ Superseded by V4.7 — no power button (the box cannot be switched off or hard-reset) and the TCXO output is DC-coupled straight into BK4819 `XI`. Do not order |
| `expansion-board-v4-kicad-V4.6-20260911.zip` | ⚠️ Matching superseded V4.6 source archive |
| `expansion-board-v4-gerber-JLC-V4.5-20260911.zip` | ⚠️ Superseded — same board without the airband chain. Do not order |
| `expansion-board-v4-kicad-V4.5-20260911.zip` | ⚠️ Matching superseded V4.5 source archive |
| `expansion-board-v4-gerber-JLC-V4.4-20260904.zip` | ⚠️ Superseded by V4.5 — V4.4 did not wire U7.14 (VCC_RF), so the module always reported `ANTENNA OPEN`. Do not order |
| `expansion-board-v4-kicad-V4.4-20260904.zip` | ⚠️ Matching superseded V4.4 source archive — do not use |
| `expansion-board-v4-gerber-JLC-V4.3-20260902.zip` | ⚠️ Superseded by V4.4 before fabrication — old 53.5mm IFA outer envelope, do not order |
| `expansion-board-v4-kicad-V4.3-20260902.zip` | ⚠️ Matching superseded V4.3 source archive — do not use |
| `expansion-board-v4-gerber-JLC-V4.1-20260830.zip` | ⚠️ Superseded before the QPL9547 bias and power corrections — do not order |
| `expansion-board-v4-kicad-V4.1-20260830.zip` | ⚠️ Matching superseded V4.1 source archive — do not use |
| `expansion-board-v4-gerber-JLC-20260830.zip` | ⚠️ Superseded: electrically includes the ECO but carries the stale V4.0 silkscreen — do not order |
| `expansion-board-v4-kicad-20260830.zip` | ⚠️ Matching stale-V4.0 source archive — do not use as the release source |
| `expansion-board-v4-gerber-JLC-20260828.zip` | ⚠️ Superseded baseline — do not use for a new order. The GNSS antenna/bias topology itself was later verified correct and was not rewired |
| `expansion-board-v4-gerber-20260828.zip` | ⚠️ Older export in Gerber **X2** format — do not use |

### Airband reference clock — V4.7 recheck

`Y4` feeds `U21` (BK4819) `XI` through `C99` (**100 nF X7R, 0402**),
with `R61` (10 kΩ) on the XI side. C99 reuses the C37/C52 BOM item
(Samsung CL05B104KB54PNC, C307331, 50 V ±10%). The former 100 pF value
did not meet the exact Epson part's external DC-block requirement of Cc ≥ 10 nF.

The revised layout moves Y4 away from the crossing 5 V trace and brings C92
close to U22 VREF. Y4 receives 3.3 V, outside X1G0042110003's recommended
2.85–3.15 V operating range. The user accepts this specification deviation for
the prototype and retains the existing supply; it is no longer a required PCB
change for this build. Startup, frequency and output amplitude remain bench checks.
This decision does not extend the manufacturer's guaranteed operating range.
U22 VREF now has C100 (100 nF / 0603) in parallel with C92 (10 µF);
the user finalized its placement and routing before this release.
The XI amplitude, bias, total load and receiver lock also require bench verification.
See the [Epson part specification](https://download.epsondevice.com/td/pdf/td_tcxo_mhz/TG-5006CG_X1G0042110003_en.pdf)
and [TI PCM1808 reference-decoupling guidance](https://www.ti.com/lit/ds/symlink/pcm1808.pdf).

### ⚠️ Three things that will ruin the board if set wrong

1. **Gerbers must be RS-274-X, not X2.** JLCPCB accepts neither KiCad project
   files nor Gerber X2. KiCad exports X2 by default, so the release package is
   regenerated with `--no-x2 --no-netlist`.
2. **Via solder-mask state comes from the file, not the order form.** JLCPCB,
   HQPCB and JiePei all state the same thing: *"if the file is Gerber, we
   process strictly per the file; this option has no effect"* — Gerber cannot
   tell a via from a component hole. This board ships 495 vias: 421 tented,
   74 exposed (all 74 inside pad mask openings, 33 of them thermal via-in-pad).
   `internal/tools/gerber.sh` recomputes this split on every release — never
   copy the numbers from an older revision.
3. **`J4`'s four `SH` holes must NOT be resin-plugged.** They are ⌀0.60 mm
   plated *slots* (Excellon `G85`) that mechanically anchor the USB-C
   receptacle. Plug them and the connector will tear off after a few
   insertions. If you order resin-filled via-in-pad, exclude these explicitly
   in the order notes.

### Stackup: preferred, not mandatory

The 28 controlled-impedance RF nets are 0.15 mm wide, derived from an L1→In1
dielectric of **0.0994 mm (single-ply 3313 prepreg)**. You do **not** need that
exact stackup:

| Your fab's L1→In1 | Result | Action |
|---|---|---|
| **0.085 – 0.115 mm** | 45–55 Ω | Send as-is. Verified: JLCPCB `JLC06161H-3313`, JiePei `JP06161H-3313B2` |
| ~0.125 mm (single-ply 2116) | ≈57 Ω | Still fine — 0.02 dB mismatch loss |
| ~0.19 mm (single-ply 7628) | ≈70 Ω | Buildable — 0.12 dB mismatch loss, slightly lower 1090 sensitivity |

The longest RF run is 11.7 mm ≈ λ/13 at 1090 MHz, so mismatch never builds a
standing wave — every case above costs less than the 0.51 dB that an SWR of 2.0
would.

> 🔴 **The on-board IFA antenna, however, does need retuning for your laminate.**
> The 50.0 mm copper length is tied to JLCPCB's 3313 material. Measured on
> 2026-09-11: the same Gerber built by JiePei on `JP06161H-7628A1` resonated at
> **1062.5 MHz instead of 1085 MHz**, with SWR rising from 1.06 to 1.72 — the
> laminate's εr sets antenna resonance, and clearing all six copper layers removes
> the *copper*, not the 1.6 mm of FR-4 underneath. Trim 0.1–0.2 mm at a time —
> 11.5 MHz/mm is a rough estimate (~2.0 mm); the same-state measured 24–25 MHz/mm gives
> ~0.9 mm — and judge in the enclosure. Irrelevant if you only use
> the external U.FL/SMA antenna.

Full numbers, per-vendor stackup tables and the mismatch-loss derivation:
[`PCB_STACKUP_IMPEDANCE.md`](../PCB_STACKUP_IMPEDANCE.md).

---

## What ships here / 这里交付什么

This directory carries the **results**: the KiCad project, the fabrication
package, and the assembly documents. Open `kicad/` in KiCad 10 to inspect or
modify the design; send `release/` straight to a board house.

本目录交付的是**结果**：KiCad 工程、制造文件包、装配文档。
要看或改设计，用 KiCad 10 打开 `kicad/`；要打板，把 `release/` 直接发板厂。

| Path | What it is |
|---|---|
| `kicad/` | Schematic + PCB source, openable and editable in KiCad 10 |
| `release/` | Gerber, drill, BOM and CPL — ready to send to a fab |
| `ASSEMBLY_MAP-V4.8.pdf` | Per-designator placement map with orientation and polarity |
| `CHECKLIST.md` | Line-by-line check sheet for hand assembly |
| `SELECTIVE_PLACEMENT.md` | Which designators must **not** simply be populated from the BOM |
| `BOM_PURCHASE-zh_CN.md` | Parts list with values, packages and selection constraints |

The release source package has been checked against the finalized PCB, including
component values, footprints and pad nets. The working directory also contains
the GNSS backup-power design, which is now part of this release (V4.8).
Internal scripts export placement, routing, zones and silkscreen from the manually
finalized board. A known zone-restoration error still blocks a complete rebuild;
lossless script replay is not claimed. These internal tools are not published.

本次发布源包已按用户完成的 PCB 核对元件料值、封装及引脚网络。工作区还保留
GNSS 备用供电设计，已随 V4.8 纳入本次发布。内部脚本从手工定稿的板子导出
布局、布线、覆铜与丝印；完整从零重建仍有已知覆铜恢复错误，尚不能保证脚本
无损重放。内部工具不在公开范围内；恢复本次板子请使用发布源包。

Requires KiCad 10 to open the project (it uses KiCad 10 file formats).
打开工程需要 KiCad 10（文件格式为 KiCad 10）。


---

## License / 许可

MIT, same as the parent repository — see the
[main README](../../README.md#开源协议--license).
与主仓库一致（MIT）。

The KiCad project embeds footprints from the KiCad standard libraries, which
are CC-BY-SA 4.0 with an exception explicitly permitting unrestricted use of
boards produced from them — so this does not impose copyleft on your hardware.
本工程内嵌了 KiCad 官方库的封装（CC-BY-SA 4.0，但其例外条款明确允许
由此制造的板子不受限制使用，不会传染到你的硬件）。
