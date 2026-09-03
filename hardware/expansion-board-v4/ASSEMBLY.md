# V4.4 Manual SMT Placement List

Chinese companion: [`ASSEMBLY-zh_CN.md`](ASSEMBLY-zh_CN.md)

> Generated from the authoritative schematic sheets and PCB coordinates. Do not hand-edit.
> Place by designator and coordinate; the PCB side alone is not a variant rule.

## Assembly variant rule

- Powered variant: populate the power section, but leave R7/R8 DNP.
- Unpowered variant: omit the complete power section and populate both R7/R8.
- Never combine CH224K with R7/R8. See [VARIANTS.md](VARIANTS.md).

## Stage A: Power (39 parts)

| Designator | Value / Model | Footprint | Board position | Rotation | Manual difficulty |
|---|---|---|---|---|---|
| **J9** | MX1.25WT-2P BAT | MX1.25WT-2P_1x02-1MP_P1.25 | (144.8, 99.0) | 0° | ★ Routine |
| **U18** | CH224K | CH224K_ESSOP-10 | (132.5, 68.6) | 0° | ★ Routine |
| **U19** | SY6970 | QFN-24-1EP_4x4mm_P0.5mm_EP | (134.5, 83.7) | 0° | ★★★ Exposed pad; reflow/hot air required |
| **L16** | 1uH | L_Bourns-SRN4018 | (139.7, 83.6) | -90° | ★ Routine |
| **L17** | 4.7uH XEL4030-472MEC | L_Coilcraft_XxL4030 | (144.8, 95.0) | -90° | ★ Routine |
| **U2** | TPS7A2033PDBVR | SOT-23-5 | (82.3, 97.8) | 90° | ★ Exposed leads; hand touch-up possible |
| **U3** | TPS7A2033PDBVR | SOT-23-5 | (89.2, 97.8) | 90° | ★ Exposed leads; hand touch-up possible |
| **U1** | ME6211C33M5 | SOT-23-5 | (74.7, 97.8) | 90° | ★ Exposed leads; hand touch-up possible |
| **U20** | SY7069 | TSOT-23-6 | (139.2, 95.0) | 0° | ★ Exposed leads; hand touch-up possible |
| **C72** | 10uF | C_1206_3216Metric | (130.9, 79.2) | 90° | ★ Routine |
| **C76** | 22uF | C_1206_3216Metric | (134.4, 96.3) | 180° | ★ Routine |
| **C77** | 22uF 16V X5R | C_1206_3216Metric | (141.2, 99.7) | -90° | ★ Routine |
| **RT1** | NCP18XH103F03RB 10k NTC | R_0603_1608Metric | (54.8, 100.9) | 90° | ★ Routine |
| **C1** | 10uF | C_0805_2012Metric | (70.8, 96.5) | 180° | ★ Routine |
| **C75** | 10uF | C_0805_2012Metric | (139.1, 88.1) | -90° | ★ Routine |
| **C74** | 10uF | C_0805_2012Metric | (143.3, 84.9) | 90° | ★ Routine |
| **C73** | 10uF | C_0805_2012Metric | (141.3, 88.1) | -90° | ★ Routine |
| **C71** | 4.7uF(带电源版) | C_0603_1608Metric | (133.4, 79.0) | 90° | ★ Routine |
| **C80** | 1uF 25V | C_0603_1608Metric | (136.3, 74.6) | 180° | ★ Routine |
| **R46** | 10k | R_0603_1608Metric | (129.2, 86.5) | 180° | ★ Routine |
| **R43** | 1k | R_0603_1608Metric | (127.5, 65.0) | 180° | ★ Routine |
| **C70** | 47nF | C_0603_1608Metric | (135.7, 80.2) | 0° | ★ Routine |
| **R48** | 0R | R_0603_1608Metric | (126.8, 67.0) | 180° | ★ Routine |
| **R45** | 10k | R_0603_1608Metric | (129.2, 84.0) | 180° | ★ Routine |
| **C6** | 1uF | C_0603_1608Metric | (92.1, 97.9) | 90° | ★ Routine |
| **R38** | 180R | R_0603_1608Metric | (133.1, 88.2) | -90° | ★ Routine |
| **C2** | 1uF | C_0603_1608Metric | (85.9, 99.1) | 0° | ★ Routine |
| **R41** | 470k | R_0603_1608Metric | (139.1, 98.6) | 90° | ★ Routine |
| **R39** | 5.62k | R_0603_1608Metric | (136.2, 88.2) | 90° | ★ Routine |
| **C79** | 1uF | C_0603_1608Metric | (130.6, 65.0) | 0° | ★ Routine |
| **R37** | 0R | R_0603_1608Metric | (137.0, 66.4) | 90° | ★ Routine |
| **C4** | 1uF | C_0603_1608Metric | (78.7, 97.8) | 0° | ★ Routine |
| **C3** | 1uF | C_0603_1608Metric | (70.8, 98.9) | 180° | ★ Routine |
| **R40** | 31.6k | R_0603_1608Metric | (134.6, 88.2) | -90° | ★ Routine |
| **R49** | 0R | R_0603_1608Metric | (126.8, 69.4) | 180° | ★ Routine |
| **R42** | 150k | R_0603_1608Metric | (137.4, 98.6) | -90° | ★ Routine |
| **C78** | 1uF | C_0603_1608Metric | (134.4, 94.2) | 180° | ★ Routine |
| **R44** | 10k | R_0603_1608Metric | (137.0, 69.5) | 90° | ★ Routine |
| **C5** | 1uF | C_0402_1005Metric | (85.9, 96.7) | 0° | ★★ 0402; use low airflow |

## Stage B: MCU + Flash (29 parts)

| Designator | Value / Model | Footprint | Board position | Rotation | Manual difficulty |
|---|---|---|---|---|---|
| **J4** | USB-C_16P | USB_C_Receptacle_HRO_TYPE- | (145.8, 73.0) | 90° | ★ Routine |
| **U8** | RP2040 | QFN-56-1EP_7x7mm_P0.4mm_EP | (86.4, 76.9) | 180° | ★★★ Exposed pad; reflow/hot air required |
| **U9** | W25Q128JVSIQ | SOIC-8_5.3x5.3mm_P1.27mm | (93.7, 85.9) | 0° | ★ Exposed leads; hand touch-up possible |
| **Y1** | 12MHz CL=10pF ABM8-272-T3 | Crystal_SMD_3225-4Pin_3.2x | (85.1, 69.1) | 180° | ★ Routine |
| **C20** | 15pF C0G | C_0603_1608Metric | (79.8, 69.1) | 90° | ★ Routine |
| **R51** | 10k | R_0603_1608Metric | (85.5, 88.5) | 90° | ★ Routine |
| **C28** | 1uF | C_0603_1608Metric | (83.1, 83.3) | -90° | ★ Routine |
| **C27** | 100nF | C_0603_1608Metric | (77.9, 82.5) | 180° | ★ Routine |
| **R5** | 10k | R_0603_1608Metric | (80.0, 73.2) | 180° | ★ Routine |
| **C19** | 15pF C0G | C_0603_1608Metric | (88.4, 69.1) | -90° | ★ Routine |
| **C87** | 10nF C0G | C_0603_1608Metric | (83.5, 88.5) | 90° | ★ Routine |
| **C82** | 100nF | C_0603_1608Metric | (91.4, 67.5) | 0° | ★ Routine |
| **C86** | 100nF | C_0603_1608Metric | (123.2, 89.6) | 0° | ★ Routine |
| **C22** | 100nF | C_0603_1608Metric | (93.2, 75.8) | 0° | ★ Routine |
| **R6** | 10k | R_0603_1608Metric | (87.9, 86.4) | 90° | ★ Routine |
| **C29** | 1uF | C_0603_1608Metric | (80.9, 83.3) | -90° | ★ Routine |
| **C26** | 100nF | C_0603_1608Metric | (78.9, 74.6) | 180° | ★ Routine |
| **C25** | 100nF | C_0603_1608Metric | (83.2, 71.6) | 180° | ★ Routine |
| **C24** | 100nF | C_0603_1608Metric | (80.9, 80.2) | 90° | ★ Routine |
| **R4** | 1k | R_0603_1608Metric | (82.0, 69.1) | 90° | ★ Routine |
| **C85** | 100nF | C_0603_1608Metric | (113.7, 80.5) | 0° | ★ Routine |
| **R9** | 27R | R_0603_1608Metric | (87.9, 83.3) | -90° | ★ Routine |
| **C84** | 1uF | C_0603_1608Metric | (110.7, 80.5) | 0° | ★ Routine |
| **R10** | 27R | R_0603_1608Metric | (85.3, 83.3) | -90° | ★ Routine |
| **R50** | 30k | R_0603_1608Metric | (86.5, 92.8) | 0° | ★ Routine |
| **C83** | 100nF | C_0603_1608Metric | (91.4, 69.8) | 0° | ★ Routine |
| **C23** | 100nF | C_0603_1608Metric | (91.8, 81.5) | 0° | ★ Routine |
| **D4** | TPESD8L3.3 0.3pFtyp 0.5pFmax | D_0402_1005Metric | (139.3, 75.4) | 90° | ★★ 0402; use low airflow |
| **D5** | TPESD8L3.3 0.3pFtyp 0.5pFmax | D_0402_1005Metric | (139.3, 77.8) | 0° | ★★ 0402; use low airflow |

## Stage C: Sensors + GNSS (33 parts)

| Designator | Value / Model | Footprint | Board position | Rotation | Manual difficulty |
|---|---|---|---|---|---|
| **U7** | ATGM336H-6N-74 | ATGM336H_LCC-18 | (73.7, 89.1) | 0° | ★★★ Exposed pad; reflow/hot air required |
| **U4** | BNO085 | BNO085_LGA-28 | (59.7, 84.5) | 90° | ★★★ Exposed pad; reflow/hot air required |
| **J2** | U.FL_GNSS_EXT(经尾线转SMA) | U.FL_Hirose_U.FL-R-SMT-1_V | (54.0, 75.9) | 180° | ★ Routine |
| **J8** | U.FL→内置patch | U.FL_Hirose_U.FL-R-SMT-1_V | (54.0, 69.2) | 180° | ★ Routine |
| **U6** | QMC5883P | LGA-16_3x3mm_P0.5mm | (59.7, 96.5) | -90° | ★★★ Exposed pad; reflow/hot air required |
| **Q4** | AO3401A | SOT-23 | (63.5, 75.9) | 180° | ★ Exposed leads; hand touch-up possible |
| **Q5** | AO3401A | SOT-23 | (63.5, 69.2) | 180° | ★ Exposed leads; hand touch-up possible |
| **U5** | BMP388 | BMP388_LGA-10 | (61.0, 88.9) | -90° | ★★★ Exposed pad; reflow/hot air required |
| **U17** | XA17-G4K | SOT-363_SC-70-6 | (68.6, 78.1) | 90° | ★ Exposed leads; hand touch-up possible |
| **C18** | 10uF | C_0805_2012Metric | (74.4, 80.8) | 90° | ★ Routine |
| **F5** | 6V/200mA | Fuse_0805_2012Metric | (60.3, 69.2) | -90° | ★ Routine |
| **F4** | 6V/200mA | Fuse_0805_2012Metric | (60.3, 75.9) | 90° | ★ Routine |
| **R26** | 10k | R_0603_1608Metric | (67.3, 73.7) | 0° | ★ Routine |
| **C12** | 100nF | C_0603_1608Metric | (56.5, 87.7) | 180° | ★ Routine |
| **C11** | 100nF | C_0603_1608Metric | (64.6, 87.2) | 180° | ★ Routine |
| **R55** | 10k | R_0603_1608Metric | (57.4, 89.8) | 0° | ★ Routine |
| **R3** | 4.7k | R_0603_1608Metric | (64.1, 95.2) | 180° | ★ Routine |
| **C15** | 4.7uF | C_0603_1608Metric | (59.9, 99.3) | 0° | ★ Routine |
| **C10** | 100nF | C_0603_1608Metric | (61.0, 92.6) | 90° | ★ Routine |
| **C14** | 100nF | C_0603_1608Metric | (64.6, 89.9) | 180° | ★ Routine |
| **R2** | 4.7k | R_0603_1608Metric | (64.1, 97.8) | 180° | ★ Routine |
| **C17** | 100nF | C_0603_1608Metric | (71.4, 81.0) | 90° | ★ Routine |
| **C13** | 100nF | C_0603_1608Metric | (59.1, 92.6) | 90° | ★ Routine |
| **R27** | 10k | R_0603_1608Metric | (67.3, 71.1) | 0° | ★ Routine |
| **C16** | 100nF | C_0603_1608Metric | (64.6, 92.5) | 180° | ★ Routine |
| **R1** | 10k | R_0603_1608Metric | (55.2, 84.5) | -90° | ★ Routine |
| **R53** | 10k | R_0402_1005Metric | (64.3, 83.0) | 180° | ★★ 0402; use low airflow |
| **R52** | 10k | R_0402_1005Metric | (64.4, 85.0) | 90° | ★★ 0402; use low airflow |
| **L15** | 33nH | L_0402_1005Metric | (57.6, 68.7) | 180° | ★★ 0402; use low airflow |
| **L2** | 33nH | L_0402_1005Metric | (57.7, 75.4) | 180° | ★★ 0402; use low airflow |
| **C58** | 100pF | C_0402_1005Metric | (57.7, 76.5) | 0° | ★★ 0402; use low airflow |
| **C59** | 100pF | C_0402_1005Metric | (57.6, 69.8) | 0° | ★★ 0402; use low airflow |
| **C57** | 100pF | C_0402_1005Metric | (68.3, 81.0) | 90° | ★★ 0402; use low airflow |

## Stage D: 978 Transceiver (34 parts)

| Designator | Value / Model | Footprint | Board position | Rotation | Manual difficulty |
|---|---|---|---|---|---|
| **U10** | CC1312R1F3RGZR | QFN-48-1EP_7x7mm_P0.5mm_EP | (104.5, 92.0) | 180° | ★★★ Exposed pad; reflow/hot air required |
| **J5** | U.FL_978 | U.FL_Hirose_U.FL-R-SMT-1_V | (118.5, 86.3) | 90° | ★ Routine |
| **Y3** | 32.768kHz FC-135 | Crystal_SMD_3215-2Pin_3.2x | (111.6, 87.0) | 0° | ★ Routine |
| **Y2** | 48MHz ABM8W-7pF | Crystal_SMD_3225-4Pin_3.2x | (108.3, 98.3) | 90° | ★ Routine |
| **Q2** | AO3401A | SOT-23 | (121.9, 95.8) | 90° | ★ Exposed leads; hand touch-up possible |
| **C66** | 22uF | C_0805_2012Metric | (102.9, 97.2) | 180° | ★ Routine |
| **C60** | 22uF | C_0805_2012Metric | (99.3, 98.0) | 90° | ★ Routine |
| **F2** | 6V/200mA | Fuse_0805_2012Metric | (121.9, 92.5) | 180° | ★ Routine |
| **L7** | 6.8uH | L_0805_2012Metric | (97.3, 94.7) | -90° | ★ Routine |
| **C63** | 100nF | C_0603_1608Metric | (107.1, 84.6) | 90° | ★ Routine |
| **C61** | 100nF | C_0603_1608Metric | (102.7, 99.1) | 180° | ★ Routine |
| **C64** | 100nF | C_0603_1608Metric | (100.2, 85.8) | 180° | ★ Routine |
| **C62** | 100nF | C_0603_1608Metric | (105.4, 98.3) | -90° | ★ Routine |
| **C68** | 18pF | C_0603_1608Metric | (113.2, 85.0) | 0° | ★ Routine |
| **R56** | 10k | R_0603_1608Metric | (98.5, 90.0) | 0° | ★ Routine |
| **C65** | 100nF | C_0603_1608Metric | (99.0, 94.7) | -90° | ★ Routine |
| **R17** | 10k | R_0603_1608Metric | (121.8, 98.7) | 180° | ★ Routine |
| **R47** | 10k | R_0603_1608Metric | (98.5, 91.7) | 0° | ★ Routine |
| **C67** | 1uF | C_0603_1608Metric | (103.6, 85.8) | 0° | ★ Routine |
| **C69** | 18pF | C_0603_1608Metric | (110.1, 85.0) | 180° | ★ Routine |
| **L10** | 27nH | L_0402_1005Metric | (110.9, 95.7) | 90° | ★★ 0402; use low airflow |
| **L13** | 7.5nH | L_0402_1005Metric | (110.7, 92.2) | 180° | ★★ 0402; use low airflow |
| **L8** | 100nH | L_0402_1005Metric | (119.3, 90.3) | 180° | ★★ 0402; use low airflow |
| **D3** | TPESD8L3.3 0.3pFtyp 0.5pFmax | D_0402_1005Metric | (117.5, 89.9) | -90° | ★★ 0402; use low airflow |
| **L11** | 6.8nH | L_0402_1005Metric | (118.2, 97.5) | 90° | ★★ 0402; use low airflow |
| **L12** | 6.8nH | L_0402_1005Metric | (118.2, 95.2) | 90° | ★★ 0402; use low airflow |
| **L9** | 7.5nH | L_0402_1005Metric | (112.3, 94.8) | 90° | ★★ 0402; use low airflow |
| **C43** | 3pF | C_0402_1005Metric | (115.9, 92.8) | 90° | ★★ 0402; use low airflow |
| **C45** | 100pF | C_0402_1005Metric | (110.7, 90.2) | 0° | ★★ 0402; use low airflow |
| **C44** | 3.6pF | C_0402_1005Metric | (112.8, 96.5) | 0° | ★★ 0402; use low airflow |
| **C39** | 100pF | C_0402_1005Metric | (118.2, 92.8) | 90° | ★★ 0402; use low airflow |
| **C42** | 6.2pF | C_0402_1005Metric | (115.9, 95.2) | 90° | ★★ 0402; use low airflow |
| **C40** | 3.6pF | C_0402_1005Metric | (110.7, 94.2) | 0° | ★★ 0402; use low airflow |
| **C41** | 2.7pF | C_0402_1005Metric | (115.9, 97.5) | 90° | ★★ 0402; use low airflow |

## Stage E: 1090 Receive Chain (49 parts)

| Designator | Value / Model | Footprint | Board position | Rotation | Manual difficulty |
|---|---|---|---|---|---|
| **FL2** | TA0970A | TA0970A_SMD3838-6 | (118.4, 66.5) | 180° | ★★★ Exposed pad; reflow/hot air required |
| **FL1** | TA0970A | TA0970A_SMD3838-6 | (112.0, 65.5) | 0° | ★★★ Exposed pad; reflow/hot air required |
| **U14** | AD8313ARMZ | MSOP-8_3x3mm_P0.65mm | (111.1, 73.4) | 180° | ★ Exposed leads; hand touch-up possible |
| **J7** | U.FL_IFA_TEST(π后调试口) | U.FL_Hirose_U.FL-R-SMT-1_V | (90.0, 61.3) | 90° | ★ Routine |
| **J6** | U.FL_1090_EXT(经尾线转SMA) | U.FL_Hirose_U.FL-R-SMT-1_V | (106.5, 67.3) | 90° | ★ Routine |
| **U15** | TLV3501(TOKMAS) | SOT-23-6 | (100.6, 79.5) | 180° | ★ Exposed leads; hand touch-up possible |
| **Q3** | AO3401A | SOT-23 | (95.3, 71.7) | 0° | ★ Exposed leads; hand touch-up possible |
| **U11** | QPL9547TR7 | DFN-8-1EP_2x2mm_P0.5mm_EP0 | (107.7, 61.9) | 0° | ★★★ Exposed pad; reflow/hot air required |
| **U16** | XA17-G4K | SOT-363_SC-70-6 | (98.0, 62.9) | 90° | ★ Exposed leads; hand touch-up possible |
| **U12** | BGA2817 | SOT-363_SC-70-6 | (117.1, 62.5) | 90° | ★ Exposed leads; hand touch-up possible |
| **F3** | 6V/200mA | Fuse_0805_2012Metric | (98.3, 71.7) | 90° | ★ Routine |
| **R32** | 10k | R_0603_1608Metric | (104.9, 75.7) | 180° | ★ Routine |
| **R18** | 10k | R_0603_1608Metric | (91.4, 71.4) | 180° | ★ Routine |
| **R57** | 33R | R_0603_1608Metric | (104.0, 82.5) | 0° | ★ Routine |
| **C55** | 100pF | C_0603_1608Metric | (97.6, 66.5) | 90° | ★ Routine |
| **R22** | 0R | R_0603_1608Metric | (100.3, 66.5) | 90° | ★ Routine |
| **R31** | 1k | R_0603_1608Metric | (101.6, 75.7) | 0° | ★ Routine |
| **R34** | 10k | R_0603_1608Metric | (77.0, 73.2) | 0° | ★ Routine |
| **R35** | 10k | R_0603_1608Metric | (77.0, 78.4) | 0° | ★ Routine |
| **C49** | 100nF | C_0603_1608Metric | (76.7, 75.4) | -90° | ★ Routine |
| **C56** | 100pF | C_0603_1608Metric | (95.0, 66.5) | -90° | ★ Routine |
| **R33** | 100k | R_0603_1608Metric | (100.9, 82.7) | 90° | ★ Routine |
| **C46** | 3pF | C_0603_1608Metric | (104.9, 78.5) | 0° | ★ Routine |
| **C51** | 1nF | C_0603_1608Metric | (77.0, 81.0) | 180° | ★ Routine |
| **R21** | 0R | R_0603_1608Metric | (105.0, 72.6) | 180° | ★ Routine |
| **R23** | 0R | R_0603_1608Metric | (93.2, 77.2) | 0° | ★ Routine |
| **ZS1** | 0R 串 | L_0603_1608Metric | (81.7, 62.9) | 0° | ★ Routine |
| **C47** | 200pF | C_0603_1608Metric | (104.9, 80.4) | 0° | ★ Routine |
| **D2** | TPESD8L3.3 0.3pFtyp 0.5pFmax | D_0402_1005Metric | (101.1, 72.6) | 180° | ★★ 0402; use low airflow |
| **R11** | 3.32k | R_0402_1005Metric | (105.1, 59.7) | 0° | ★★ 0402; use low airflow |
| **L14** | 100nH | L_0402_1005Metric | (101.1, 70.6) | 0° | ★★ 0402; use low airflow |
| **R19** | 52.3R | R_0402_1005Metric | (115.1, 73.4) | 90° | ★★ 0402; use low airflow |
| **L1** | 18nH 0402CS-18NXGRW | L_0402_1005Metric | (110.4, 59.8) | 90° | ★★ 0402; use low airflow |
| **R54** | 10R | R_0402_1005Metric | (116.5, 77.0) | 90° | ★★ 0402; use low airflow |
| **C30** | 100pF | C_0402_1005Metric | (102.8, 67.5) | 90° | ★★ 0402; use low airflow |
| **C52** | 100nF | C_0402_1005Metric | (99.6, 82.3) | -90° | ★★ 0402; use low airflow |
| **C35** | 100pF C0G | C_0402_1005Metric | (115.1, 71.1) | 90° | ★★ 0402; use low airflow |
| **C54** | 100pF | C_0402_1005Metric | (103.9, 61.7) | 0° | ★★ 0402; use low airflow |
| **C36** | 100pF | C_0402_1005Metric | (106.0, 64.3) | 90° | ★★ 0402; use low airflow |
| **C48** | 1uF | C_0402_1005Metric | (107.4, 64.3) | 90° | ★★ 0402; use low airflow |
| **C38** | 100pF | C_0402_1005Metric | (111.6, 77.0) | -90° | ★★ 0402; use low airflow |
| **C81** | 470pF C0G | C_0402_1005Metric | (115.2, 64.5) | -90° | ★★ 0402; use low airflow |
| **C31** | 100pF C0G | C_0402_1005Metric | (111.3, 62.2) | -90° | ★★ 0402; use low airflow |
| **C21** | 100pF | C_0402_1005Metric | (103.6, 59.7) | 90° | ★★ 0402; use low airflow |
| **C53** | 100pF | C_0402_1005Metric | (93.6, 62.9) | 0° | ★★ 0402; use low airflow |
| **C32** | 100pF C0G | C_0402_1005Metric | (114.3, 62.2) | 90° | ★★ 0402; use low airflow |
| **C34** | 100pF C0G | C_0402_1005Metric | (117.1, 73.4) | -90° | ★★ 0402; use low airflow |
| **C33** | 100pF C0G | C_0402_1005Metric | (120.4, 63.2) | 0° | ★★ 0402; use low airflow |
| **C37** | 100nF | C_0402_1005Metric | (113.9, 77.0) | -90° | ★★ 0402; use low airflow |

## Stage F: External Interface (1 parts)

| Designator | Value / Model | Footprint | Board position | Rotation | Manual difficulty |
|---|---|---|---|---|---|
| **J1** | J3_HAT_2x20_SMD排针 | PinHeader_2x20_P2.54mm_Ver | (92.6, 106.3) | 90° | ★ Routine |

## Not placed (22 positions)

| Designator | Reason |
|---|---|
| **ANT1** | 板载天线，PCB铜箔本身，无器件。50.0mm外包络/48.5mm中心线；V4.0装盒实测1082.5MHz、SWR 1.09 |
| **H1** | M2.5 安装孔（⌀2.7mm NPTH），不是元件 |
| **H2** | M2.5 安装孔（⌀2.7mm NPTH），不是元件 |
| **H3** | M2.5 安装孔（⌀2.7mm NPTH），不是元件 |
| **H4** | M2.5 安装孔（⌀2.7mm NPTH），不是元件 |
| **R7** | DNP，设计上默认不贴（5.1k DNP(带电源版)） |
| **R8** | DNP，设计上默认不贴（5.1k DNP(带电源版)） |
| **R24** | DNP，设计上默认不贴（0R DNP(旁路外接)） |
| **R25** | DNP，设计上默认不贴（0R DNP(旁路板载)） |
| **R30** | DNP，设计上默认不贴（1k DNP） |
| **R36** | DNP，设计上默认不贴（1k DNP） |
| **SW1** | 短接焊盘（BOOTSEL），用镊子短接，不贴件。**焊盘不占 SMT 贴装面数**，成本影响为 0 |
| **SW2** | 短接焊盘（BOOTSEL），用镊子短接，不贴件。**焊盘不占 SMT 贴装面数**，成本影响为 0 |
| **TP1** | 测试点焊盘，探针/飞线用，不贴件 |
| **TP2** | 测试点焊盘，探针/飞线用，不贴件 |
| **TP3** | 测试点焊盘，探针/飞线用，不贴件 |
| **TP4** | 测试点焊盘，探针/飞线用，不贴件 |
| **TP5** | 测试点焊盘，探针/飞线用，不贴件 |
| **TP6** | 测试点焊盘，探针/飞线用，不贴件 |
| **TP7** | 测试点焊盘，探针/飞线用，不贴件 |
| **ZP1** | DNP，设计上默认不贴（DNP 并-天线侧） |
| **ZP2** | DNP，设计上默认不贴（DNP 并-电台侧） |
