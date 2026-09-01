# V4.3 Purchase BOM (single authoritative version, auto-exported from the netlist — do not hand-edit)
Chinese version: [`BOM_PURCHASE-zh_CN.md`](BOM_PURCHASE-zh_CN.md)

> ⚠️ **This file supersedes all purchase tables previously scattered across design doc §7/§12/§13.**

> After netlist changes, just re-run `gen_bom.py`; the script carries assertions and errors out on any unclassified component, so nothing gets missed.

> **IFA**: the 52.0mm drawn length / 53.5mm outer envelope is already on the PCB; the default 0R/DNP is just a pass-through, and 3.6nH/3.3pF is only the first-round sweep starting point from the full six-layer rev2 HFSS, not a production value.


193 components / 96 distinct part numbers in total.


| Purchase | Category | Value/Spec | Footprint | Qty | Ref Des | Recommended Part | LCSC Part No. | Notes |
|---|---|---|---|---|---|---|---|---|
| 🔴Must-buy (specified) | RF-978 matching | 2.7pF | C_0402_1005Metric | 1 | C41 | Fenghua (风华) 0402CG2R7C500NT (C0G) | C1561 | ⚠️Must be C0G |
| 🔴Must-buy (specified) | RF-978 matching | 27nH | L_0402_1005Metric | 1 | L10 | muRata LQW15AN27NH00D (wirewound ±3% SRF3.5G) | C113111 | Alternatives: Sunltech SCW1005C27NJST C330864 (±5% SRF3.5G, stock 8872), APV AHW1005C-27NJTF C6364690 (±5% SRF2.48G) |
| 🔴Must-buy (specified) | RF-978 matching | 3.6pF | C_0402_1005Metric | 2 | C40,C44 | YAGEO AC0402CRNPO9BN3R6 (C0G) | C5552081 | ⚠️Must be C0G/NP0; X7R cannot tune the match accurately |
| 🔴Must-buy (specified) | RF-978 matching | 6.2pF | C_0402_1005Metric | 1 | C42 | Fenghua (风华) 0402CG6R2C500NT (C0G) | C41744 | ⚠️Must be C0G |
| 🔴Must-buy (specified) | RF-978 matching | 6.8nH | L_0402_1005Metric | 2 | L11,L12 | muRata LQW15AN6N8G00D (wirewound high-Q) | C82919 | Domestic alternative Sunltech SCW1005C6N8JST C330847 |
| 🔴Must-buy (specified) | RF-978 matching | 7.5nH | L_0402_1005Metric | 2 | L13,L9 | muRata LQW15AN7N5G00D (wirewound high-Q) | C82918 | Domestic alternative Sunltech SCW1005C7N5JST C330848 |
| 🔴Must-buy (specified) | RF-bias choke | 100nH | L_0402_1005Metric | 2 | L14,L8 | DDY WI0402IFR10KST-HF (wirewound) | C18221115 | ⚠️SRF 1.4GHz. Common multilayer types have SRF of only 600-700MHz; by 1090MHz they already act as capacitors and short RF into the supply |
| 🔴Must-buy (specified) | RF-LNA bias | 3.32k | R_0402_1005Metric | 1 | R11 | 0402 3.32k ±1% | — | QPL9547 pin1 Vbias; use the evaluation-board value, never a 0Ω tie to 3V3_RF |
| 🔴Must-buy (specified) | RF-LNA feed choke | 18nH 0402CS-18NXGRW | L_0402_1005Metric | 1 | L1 | Coilcraft 0402CS-18NXGRW or equivalent high-SRF wirewound 18nH | — | QPL9547 pin7 VDD feed; any substitute must remain inductive at 1090MHz |
| 🔴Must-buy (specified) | RF-bias choke | 33nH | L_0402_1005Metric | 2 | L15,L2 | APV AHW1005C-33NJTF (wirewound) | C6807986 | ⚠️SRF 2.35GHz. GNSS 1575MHz requires SRF≥2GHz; multilayer types do not qualify |
| 🔴Must-buy (specified) | RF-antenna-port ESD | ESD 3.3V/0.6pF | D_0402_1005Metric | 2 | D2,D3 | TECH PUBLIC TPESD8L3.3CT5G | C2830293 | ⚠️Junction capacitance Cj 0.3pF typ/0.5pF max. Ordinary 5V ESD diodes with Cj 20-50pF swallow the RF signal outright |
| 🔴Must-buy (specified) | RF-detector coupling | 3pF | C_0402_1005Metric | 1 | C43 | CCTC (三环/Sanring) TCC0402COG3R0C500AT (C0G) | C696883 | ⚠️Must be C0G |
| 🔴Must-buy (specified) | RF coupling/bypass | 100pF C0G | C_0402_1005Metric | 2 | C34,C35 | Fenghua 0402CG101J500NT (C0G) | C1546 | C34/C35 set the 1090MHz detector-input high-pass corner; do not substitute X7R |
| 🔴Must-buy (specified) | RF LNA decoupling | 470pF C0G | C_0402_1005Metric | 1 | C81 | 0402 470pF C0G/NP0 ±5% | — | Dedicated BGA2817 pin1 Cdec; place close to U12 |
| 🔴Must-buy (specified) | USB low-capacitance ESD | TPESD8L3.3 0.6pF | D_0402_1005Metric | 2 | D4,D5 | TECH PUBLIC TPESD8L3.3CT5G | C2830293 | One on each D+/D− line; bidirectional, no polarity requirement |
| 🔴Must-buy (specified) | RF-termination | 52.3R | R_0402_1005Metric | 1 | R19 | YAGEO RC0402FR-0752R3L ±1% | C273696 | Must be ±1%; 51R±5% is not an acceptable substitute |
| 🔴Must-buy (specified) | RF-interstage coupling | 12pF | C_0402_1005Metric | 3 | C31,C32,C33 | Fenghua (风华) 0402CG120J500NT (C0G, basic library) | C1547 | ⚠️Must be C0G. **JLC basic part** — buy exactly what you need, don't switch back to the extended-library part of the same value |
| 🔴Must-buy (specified) | Power-PD | CH224K | CH224K_ESSOP-10 | 1 | U18 | CH224K | C970725 | PD decoy to 9V; 4-22V input |
| 🔴Must-buy (specified) | Power-charging | SY6970 | QFN-24-1EP_4x4mm_P0.5mm_EP2.6x2.6mm_ThermalVias | 1 | U19 | SY6970QCC | C5357542 | 5A charging + I2C fuel gauge, 1.5MHz |
| 🔴Must-buy (specified) | Power-boost | SY7069 | TSOT-23-6 | 1 | U20 | SY7069ADC | Taobao (not seen on LCSC) | Synchronous boost to 5V, I_LIM 3.0A |
| 🔴Must-buy (specified) | Power-boost inductor | 4.7uH XEL4030-472MEC | L_Coilcraft_XxL4030 | 1 | L17 | Coilcraft XEL4030-472MEC | — | Any substitute requires Isat, Irms, DCR, thermal and footprint review |
| 🔴Must-buy (specified) | Power capacitor | 22uF 16V X5R | C_1206_3216Metric | 1 | C77 | 1206 22uF 16V X5R/X7R | — | SY7069 output storage; verify effective capacitance at 5V DC bias |
| 🔴Must-buy (specified) | Power-inductor | 1uH | L_Bourns-SRN4018 | 1 | L16 | 1uH SRN4018 Isat>=5A | — | SY6970 switching inductor, paired with 1.5MHz |
| 🔴Must-buy (specified) | Connector | MX1.25WT-2P BAT | MX1.25WT-2P_1x02-1MP_P1.25mm_Horizontal | 1 | J9 | MX1.25 horizontal 1.25WT-2P | — | Battery connector; MP 1.8x2.4 / outer-edge spacing 7.50, not interchangeable with PicoBlade |
| 🟡Recommended option | CC1312R DCDC | 6.8uH | L_0805_2012Metric | 1 | L7 | TDK MLZ2012N6R8LT000 | C82157 | ⚠️Among all 0805 parts site-wide, only this one has DCR<300mΩ and Isat≥100mA (250mΩ/110mA, just barely at the line) |
| 🟡Recommended option | PMOS high-side switch | AO3401A | SOT-23 | 4 | Q2,Q3,Q4,Q5 | UMW (Youtai 友台) AO3401A | C347476 | Extended library loss8/moq5 → buy 16 pcs. **Without coupons, switch to the AOS original C15127** (basic library; buying 8 pcs waives the setup fee). Pitfalls: ElecSuper C5224202 is non-standard SOT-23-3L; GOODWORK C2938368's description wrongly says N-Channel |
| 🟡Recommended option | U.FL receptacle | U.FL_1090_EXT (via pigtail to SMA) | U.FL_Hirose_U.FL-R-SMT-1_Vertical | 1 | J6 | 聚兴泰 (Juxingtai) AIPEX-1 | C41432122 | IPEX1 board-end socket 50Ω / DC~6GHz / VSWR1.3. **Datasheet marks it 3 PADS TYPE**; pads match the on-board U.FL_Hirose_U.FL-R-SMT-1_Vertical exactly (span 4.00 / pitch 1.90 / 2.20×1.05). 4 pcs: 1090 external / 978 / GNSS external / GNSS internal patch. The two external ones additionally get U.FL→SMA pigtails. ⚠️ When switching brands, verify the pad count — four-pad versions (e.g. 品赞 (Pinzan) C5299419) leave one pin floating |
| 🟡Recommended option | U.FL receptacle | U.FL_978 | U.FL_Hirose_U.FL-R-SMT-1_Vertical | 1 | J5 | 聚兴泰 (Juxingtai) AIPEX-1 | C41432122 | IPEX1 board-end socket 50Ω / DC~6GHz / VSWR1.3. **Datasheet marks it 3 PADS TYPE**; pads match the on-board U.FL_Hirose_U.FL-R-SMT-1_Vertical exactly (span 4.00 / pitch 1.90 / 2.20×1.05). 4 pcs: 1090 external / 978 / GNSS external / GNSS internal patch. The two external ones additionally get U.FL→SMA pigtails. ⚠️ When switching brands, verify the pad count — four-pad versions (e.g. 品赞 (Pinzan) C5299419) leave one pin floating |
| 🟡Recommended option | U.FL receptacle | U.FL_GNSS_EXT (via pigtail to SMA) | U.FL_Hirose_U.FL-R-SMT-1_Vertical | 1 | J2 | 聚兴泰 (Juxingtai) AIPEX-1 | C41432122 | IPEX1 board-end socket 50Ω / DC~6GHz / VSWR1.3. **Datasheet marks it 3 PADS TYPE**; pads match the on-board U.FL_Hirose_U.FL-R-SMT-1_Vertical exactly (span 4.00 / pitch 1.90 / 2.20×1.05). 4 pcs: 1090 external / 978 / GNSS external / GNSS internal patch. The two external ones additionally get U.FL→SMA pigtails. ⚠️ When switching brands, verify the pad count — four-pad versions (e.g. 品赞 (Pinzan) C5299419) leave one pin floating |
| 🟡Recommended option | U.FL receptacle | U.FL_IFA_TEST (tuning port after π) | U.FL_Hirose_U.FL-R-SMT-1_Vertical | 1 | J7 | 聚兴泰 (Juxingtai) AIPEX-1 | C41432122 | IPEX1 board-end socket 50Ω / DC~6GHz / VSWR1.3. **Datasheet marks it 3 PADS TYPE**; pads match the on-board U.FL_Hirose_U.FL-R-SMT-1_Vertical exactly (span 4.00 / pitch 1.90 / 2.20×1.05). 4 pcs: 1090 external / 978 / GNSS external / GNSS internal patch. The two external ones additionally get U.FL→SMA pigtails. ⚠️ When switching brands, verify the pad count — four-pad versions (e.g. 品赞 (Pinzan) C5299419) leave one pin floating |
| 🟡Recommended option | U.FL receptacle | U.FL→internal patch | U.FL_Hirose_U.FL-R-SMT-1_Vertical | 1 | J8 | 聚兴泰 (Juxingtai) AIPEX-1 | C41432122 | IPEX1 board-end socket 50Ω / DC~6GHz / VSWR1.3. **Datasheet marks it 3 PADS TYPE**; pads match the on-board U.FL_Hirose_U.FL-R-SMT-1_Vertical exactly (span 4.00 / pitch 1.90 / 2.20×1.05). 4 pcs: 1090 external / 978 / GNSS external / GNSS internal patch. The two external ones additionally get U.FL→SMA pigtails. ⚠️ When switching brands, verify the pad count — four-pad versions (e.g. 品赞 (Pinzan) C5299419) leave one pin floating |
| 🟡Recommended option | USB-C | USB-C_16P | USB_C_Receptacle_HRO_TYPE-C-31-M-12 | 1 | J4 | HRO TYPE-C-31-M-12 | — | RP2040 flashing port |
| 🟡Recommended option | Energy storage | 22uF | C_0805_2012Metric | 2 | C60,C66 | Samsung CL21A226KOQNNNE | C296720 | 16V ±10% X5R (original design voltage rating). Extended library; **without coupons, switch to basic-library C45783** (25V ±20%, waives the ¥20 setup fee) |
| 🟡Recommended option | Decoupling | 100nF | C_0603_1608Metric | 19 | C10,C11,C12,C13,C14,C16…(19) | Venkel C0603X7R500-104KNP | C3834556 | 0603/X7R/50V/±10%. Extended library loss6/moq20 → buy 44 pcs, parts cost ¥1.79. **Without coupons, switch to basic-library C14663** (YAGEO, 38 pcs for ¥3.08, waives the ¥20 setup fee) |
| 🟡Recommended option | RF switch | XA17-G4K(or AS179-92LF) | SOT-363_SC-70-6 | 2 | U16,U17 | 信路达 (Xinluda) XA17-G4K | C513494 | Pin-compatible with AS179-92LF(C83422); one package, dual source. SOT-363 is tiny; keep 3 extra spares |
| 🔴Must-buy (specified) | Crystal | 12MHz CL=10pF ABM8-272-T3 | Crystal_SMD_3225-4Pin_3.2x2.5mm | 1 | Y1 | Abracon ABM8-12.000MHZ-10-1-U-T3 / ABM8-272-T3 | — | RP2040 main crystal; CL must be 10pF, paired with C19/C20=15pF C0G |
| 🟡Recommended option | Crystal | 32.768kHz FC-135 | Crystal_SMD_3215-2Pin_3.2x1.5mm | 1 | Y3 | EPSON Q13FC13500004 | C32346 |  |
| 🟡Recommended option | Crystal | 48MHz ABM8W-7pF | Crystal_SMD_3225-4Pin_3.2x2.5mm | 1 | Y2 | Abracon ABM8W-48.0000MHZ-7-D1X-T3 | C6732653 | CC1312R main crystal, 7pF load |
| 🟡Recommended option | Crystal load | 18pF | C_0603_1608Metric | 2 | C68,C69 | Samsung CL10C180JB8NNNC (basic library) | C1647 | Matches Y3 Q13FC13500004 (FC-135) CL=12.5pF → C=(12.5−3)×2=19pF, nominal 18pF chosen. Both parts must be the same value; ⚠️must be C0G |
| 🔴Must-buy (specified) | Crystal load | 15pF C0G | C_0603_1608Metric | 2 | C19,C20 | Samsung CL10C150JB8NNNC (C0G) | C1644 | Paired with ABM8-272-T3 (CL=10pF); both capacitors must use the same value |
| 🟡Recommended option | Power-thermistor | NCP18XH103F03RB 10k NTC | R_0603_1608Metric | 1 | RT1 | Murata NCP18XH103F03RB / TDK NTCG163JF103FT1 / domestic 0603 10K 1% | — | **Must be an SMD NTC, not an ordinary resistor**. The battery is 3M-taped directly onto the B side with no gap; a through-hole glass-head type won't fit. B25/85=3428K differs by 0.2% from the datasheet's 103AT-2 (3435K); divider resistors R39/R40 need no change |
| 🟡Recommended option | Power-capacitor | 22uF | C_1206_3216Metric | 1 | C76 | 1206 22uF 10V X5R/X7R | — | Input/storage capacitor; verify effective capacitance at operating voltage |
| 🟡Recommended option | Power-capacitor | 47nF | C_0603_1608Metric | 1 | C70 | 0603 47nF 25V X7R | — | SY6970 BST bootstrap |
| 🟡Recommended option | Power-capacitor | 10uF | C_1206_3216Metric | 1 | C72 | 1206 10uF 25V X5R/X7R | — | SY6970 PMID; the datasheet requires 10µF. Verify effective capacitance under 9V DC bias |
| 🟡Recommended option | Power-resistor | 150k | R_0603_1608Metric | 1 | R42 | 0603 150k 1% | — | FB lower divider; 470k/150k → 4.96V |
| 🟡Recommended option | Power-resistor | 180R | R_0603_1608Metric | 1 | R38 | 0603 180R 1% | — | SY6970 R_ILIM; K_ILIM=360A·Ω → 2A |
| 🟡Recommended option | Power-resistor | 31.6k | R_0603_1608Metric | 1 | R40 | 0603 31.6k 1% | — | NTC divider lower leg (nearest E96 value) |
| 🟡Recommended option | Power-resistor | 470k | R_0603_1608Metric | 1 | R41 | 0603 470k 1% | — | SY7069 FB upper divider |
| 🟡Recommended option | Power-resistor | 5.62k | R_0603_1608Metric | 1 | R39 | 0603 5.62k 1% | — | NTC divider upper leg (nearest E96 value) |
| ✅Already have | Jumper | 0R | R_0603_1608Metric | 6 | R21,R22,R23,R37,R48,R49 | 0603 resistor bin 0R | — | R37/R48/R49 force CH224K CFG1/2/3 low for the documented 9V profile |
| 🟡Recommended option | RP2040 DVDD decoupling | 100nF | C_0201_0603Metric | 2 | C82,C83 | 0201 100nF X5R/X7R, ≥6.3V | — | One close to each DVDD pin23/pin50 |
| 🟡Recommended option | Reset pull-up | 10k | R_0201_0603Metric | 1 | R47 | 0201 10k ±1% | — | CC1312R RESET_N pull-up; firmware must only pull reset low with open-drain behavior |
| 🟡Recommended option | Resistor | 1k | R_0402_1005Metric | 1 | R30 | UNI-ROYAL (厚声) 0402WGF1001TCE (basic library) | C11702 | 1kΩ ±1%. **JLC basic part**, waives the ¥20 setup fee |
| 🟡Recommended option | Power - REGN storage | 4.7uF (powered variant) | C_0603_1608Metric | 1 | C71 | Samsung CL10A475KO8NNNC | C19666 | CHG_REGN decoupling. Populate only on the powered variant; omit with the complete U19 group on the unpowered variant |
| 🟡Recommended option | Magnetometer energy storage | 4.7uF | C_0603_1608Metric | 1 | C15 | Samsung CL10A475KO8NNNC | C19666 | Low ESR, required by QMC5883P |
| 🟡Recommended option | Resettable fuse | 6V/200mA | Fuse_0805_2012Metric | 4 | F2,F3,F4,F5 | 金瑞 (Jinrui) JK-SMD0805-020-30V | C516070 | Ih=200mA, 30V rating, $0.031, stock 600k |
| 🟢General/parts bin | IFA matching-series | 0R series | L_0603_1608Metric | 1 | ZS1 | Default 0R pass-through; swap value during tuning | — | Stock 1-30nH; the first HFSS round can start sweeping from 3.6nH. ⚠️Production units must be re-measured after Q changes |
| 🟢General/parts bin | IFA matching-parallel | DNP shunt-antenna side | C_0603_1608Metric | 1 | ZP1 | For tuning; value fixed by on-board measurement | — | 0603 for easy value swaps; stock C0G 0.5-12pF. Default DNP; the HFSS first round also keeps DNP |
| 🟢General/parts bin | IFA matching-parallel | DNP shunt-radio side | C_0603_1608Metric | 1 | ZP2 | For tuning; value fixed by on-board measurement | — | Default DNP; per the full six-layer rev2 HFSS result of about 20.97Ω at the target, the first round can start sweeping from 3.3pF with 3.6/3.9pF on hand; the final value is subject to the in-enclosure VNA |
| 🟢General/parts bin | Energy storage | 10uF | C_0805_2012Metric | 5 | C1,C18,C73,C74,C75 | Any 10uF X5R 0805 | — | Use the parts bin |
| 🟢General/parts bin | Decoupling | 100nF | C_0402_1005Metric | 2 | C37,C52 | Any 100nF X7R | — | Use the parts bin |
| 🟢General/parts bin | Decoupling | 1uF | C_0402_1005Metric | 2 | C48,C5 | Any 1uF X5R | — | Use the parts bin |
| 🟢General/parts bin | Experimental jumper | 0R DNP (bypass external) | R_0402_1005Metric | 1 | R24 | 0R (not populated by default) | — | After removing the switch, hard-jump the external branch for a "with-switch vs without-switch" sensitivity comparison |
| 🟢General/parts bin | Experimental jumper | 0R DNP (bypass onboard) | R_0402_1005Metric | 1 | R25 | 0R (not populated by default) | — | Same as above; hard-jump the onboard IFA |
| 🟢General/parts bin | DC block/bypass | 100pF | C_0402_1005Metric | 10 | C30,C36,C38,C39,C45,C53…(10) | Any 100pF (X7R is fine) | — | ✅Actual calculation: X7R vs C0G differs by only 0.13dB over the full path; not worth buying NP0 separately. The existing parts bin suffices |
| ✅Already have | USB series resistor | 27R | R_0603_1608Metric | 2 | R10,R9 | 0603 resistor bin 27R | — | Use the existing parts bin |
| 🟡Recommended option | Power-variant resistor | 5.1k DNP (powered variant) | R_0603_1608Metric | 2 | R7,R8 | 0603 5.1k ±1% | — | Powered variant: DNP. Unpowered variant: both must be populated. Never mix the two assembly rules |
| ✅Already have | Pull-up/pull-down | 10k | R_0603_1608Metric | 13 | R1,R17,R18,R26,R27,R32…(13) | 0603 resistor bin 10k | — | Use the existing parts bin |
| ✅Already have | Pull-up | 4.7k | R_0603_1608Metric | 2 | R2,R3 | 0603 resistor bin 4.7k | — | Use the existing parts bin |
| ✅Already have | Main IC | AD8313ARMZ | MSOP-8_3x3mm_P0.65mm | 1 | U14 | AD8313ARMZ | — | Ordered per the §7 list |
| ✅Already have | Main IC | ATGM336H-6N-74 | ATGM336H_LCC-18 | 1 | U7 | ATGM336H-6N-74 | — | Ordered per the §7 list |
| ✅Already have | Main IC | BGA2817 | SOT-363_SC-70-6 | 1 | U12 | BGA2817 | — | Ordered per the §7 list |
| ✅Already have | Main IC | BMP388 | BMP388_LGA-10 | 1 | U5 | BMP388 | — | Ordered per the §7 list |
| ✅Already have | Main IC | BNO085 | BNO085_LGA-28 | 1 | U4 | BNO085 | — | Ordered per the §7 list |
| ✅Already have | Main IC | CC1312R1F3RGZR | QFN-48-1EP_7x7mm_P0.5mm_EP5.15x5.15mm | 1 | U10 | CC1312R1F3RGZR | — | Ordered per the §7 list |
| ✅Already have | Main IC | ME6211C33M5 | SOT-23-5 | 1 | U1 | ME6211C33M5 | — | Ordered per the §7 list |
| ✅Already have | Main IC | QMC5883P | LGA-16_3x3mm_P0.5mm | 1 | U6 | QMC5883P | — | Ordered per the §7 list |
| ✅Already have | Main IC | QPL9547TR7 | DFN-8-1EP_2x2mm_P0.5mm_EP0.86x1.55mm | 1 | U11 | QPL9547TR7 | — | Ordered per the §7 list |
| ✅Already have | Main IC | RP2040 | QFN-56-1EP_7x7mm_P0.4mm_EP3.2x3.2mm | 1 | U8 | RP2040 | — | Ordered per the §7 list |
| ✅Already have | Main IC | TA0970A | TA0970A_SMD3838-6 | 2 | FL1,FL2 | TA0970A | — | Ordered per the §7 list |
| ✅Already have | Main IC | TLV3501(TOKMAS) | SOT-23-6 | 1 | U15 | TLV3501(TOKMAS) | — | Ordered per the §7 list |
| ✅Already have | Main IC | TPS7A2033PDBVR | SOT-23-5 | 2 | U2,U3 | TPS7A2033PDBVR | — | Ordered per the §7 list |
| ✅Already have | Main IC | W25Q128JVSIQ | SOIC-8_5.3x5.3mm_P1.27mm | 1 | U9 | W25Q128JVSIQ | — | Ordered per the §7 list |
| ✅Already have | Divider/pull-down | 100k | R_0603_1608Metric | 3 | R33,R50,R51 | 0603 resistor bin 100k | — | R50/R51 form the equal USB VBUS divider feeding GPIO28 |
| ✅Already have | Decoupling | 1uF | C_0603_1608Metric | 10 | C2,C28,C29,C3,C4,C6…(10) | 0603 capacitor bin 1uF X5R | — | Use the existing parts bin |
| ✅Already have | Stacking pin header | J3_HAT_2x20_SMD header | PinHeader_2x20_P2.54mm_Vertical_SMD | 1 | J1 | 2×20 2.54mm SMD pin header | — | ✅User already has one (measured 51mm long / SMD pins 2.85 / mating board spacing 7.0) |
| ✅Already have | Bypass | 100pF | C_0603_1608Metric | 3 | C21,C55,C56 | 0603 capacitor bin 100pF | — | Use the existing parts bin |
| ✅Already have | Onboard 1090 antenna | IFA_1090 | ANT_IFA_1090MHz | 1 | ANT1 | PCB copper foil (no component) | — | 52.0mm drawn length / 53.5mm outer envelope already on the PCB; pending in-enclosure VNA + message A/B; sized per the archived v3 HFSS antenna study (internal engineering notes) §8; not counted in the BOM |
| ✅Already have | Filtering | 1nF | C_0603_1608Metric | 1 | C51 | 0603 capacitor bin 1nF | — | Use the existing parts bin |
| ✅Already have | Filtering | 200pF | C_0603_1608Metric | 1 | C47 | 0603 capacitor bin 200pF | — | Use the existing parts bin |
| ✅Already have | Filtering | 3pF | C_0603_1608Metric | 1 | C46 | 0603 capacitor bin 3pF | — | Use the existing parts bin |
| ✅Already have | Solder jumper | BOOTSEL solder jumper | SolderJumper-2_P1.3mm_Open_Pad1.0x1.5mm | 1 | SW2 | PCB pads, no component | — | Same as above |
| ✅Already have | Solder jumper | RESET solder jumper | SolderJumper-2_P1.3mm_Open_Pad1.0x1.5mm | 1 | SW1 | PCB pads, no component | — | Short with tweezers for RUN reset; no part to purchase |
| ✅Already have | Test point | TestPoint | TestPoint_Pad_1.0x1.0mm | 7 | TP1,TP2,TP3,TP4,TP5,TP6…(7) | PCB pads, no component | — | SWD and 1090-demodulation observation points; no part to purchase |
| ✅Already have | Current-limit/bias | 1k | R_0603_1608Metric | 3 | R31,R4,R43 | 0603 resistor bin 1k | — | Use the existing parts bin |
| ✅Already have | DNP comparator output | 1k DNP | R_0603_1608Metric | 1 | R36 | Do not purchase/populate | — | R36 must remain DNP so it cannot bridge 3V3_RF and 3V3_DIG through an output pull-up |
