# V3.10 Manual SMT Placement List

Chinese companion: [`ASSEMBLY-zh_CN.md`](ASSEMBLY-zh_CN.md)

> Generated from the authoritative schematic sheets and PCB coordinates. Do not hand-edit.
> Place by designator and coordinate; the PCB side alone is not a variant rule.

## Stage A: Power (9 parts)

| Designator | Value / Model | Footprint | Board position | Rotation | Manual difficulty |
|---|---|---|---|---|---|
| **U3** | TPS7A2033PDBVR | SOT-23-5 | (74.3, 93.3) | 180° | ★ Exposed leads; hand touch-up possible |
| **U1** | ME6211C33M5 | SOT-23-5 | (69.4, 93.3) | 180° | ★ Exposed leads; hand touch-up possible |
| **U2** | TPS7A2033PDBVR | SOT-23-5 | (63.1, 93.3) | 180° | ★ Exposed leads; hand touch-up possible |
| **C1** | 10uF | C_0805_2012Metric | (78.6, 93.3) | -90° | ★ Routine |
| **C2** | 1uF | C_0603_1608Metric | (62.0, 98.3) | 180° | ★ Routine |
| **C4** | 1uF | C_0603_1608Metric | (71.0, 98.3) | 0° | ★ Routine |
| **C3** | 1uF | C_0603_1608Metric | (66.5, 98.3) | 0° | ★ Routine |
| **C6** | 1uF | C_0603_1608Metric | (78.6, 67.0) | 0° | ★ Routine |
| **C5** | 1uF | C_0402_1005Metric | (109.6, 79.2) | 0° | ★★ 0402; use low airflow |

## Stage B: MCU + Flash (32 parts)

| Designator | Value / Model | Footprint | Board position | Rotation | Manual difficulty |
|---|---|---|---|---|---|
| **J4** | USB-C_16P | USB_C_Receptacle_HRO_TYPE- | (53.5, 92.0) | -90° | ★ Routine |
| **U8** | RP2040 | QFN-56-1EP_7x7mm_P0.4mm_EP | (88.4, 80.5) | -90° | ★★★ Exposed pad; reflow/hot air required |
| **U9** | W25Q128JVSIQ | SOIC-8_5.3x5.3mm_P1.27mm | (103.4, 82.1) | 0° | ★ Exposed leads; hand touch-up possible |
| **D1** | B5819W | D_SOD-123 | (97.8, 91.3) | 0° | ★ Exposed leads; hand touch-up possible |
| **Y1** | 12MHz CL=10pF ABM8-272-T3 | Crystal_SMD_3225-4Pin_3.2x | (100.4, 87.7) | 0° | ★ Routine |
| **R51** | 10k | R_0603_1608Metric | (96.0, 85.2) | -90° | ★ Routine |
| **R5** | 10k | R_0603_1608Metric | (95.7, 98.3) | 180° | ★ Routine |
| **R8** | 5.1k | R_0603_1608Metric | (107.8, 98.3) | 0° | ★ Routine |
| **R6** | 10k | R_0603_1608Metric | (99.8, 98.3) | 180° | ★ Routine |
| **R7** | 5.1k | R_0603_1608Metric | (103.8, 98.3) | 0° | ★ Routine |
| **C22** | 100nF | C_0603_1608Metric | (95.7, 95.2) | 0° | ★ Routine |
| **C26** | 100nF | C_0603_1608Metric | (107.8, 95.2) | 0° | ★ Routine |
| **C24** | 100nF | C_0603_1608Metric | (103.8, 95.2) | 0° | ★ Routine |
| **C27** | 100nF | C_0603_1608Metric | (111.9, 95.2) | 0° | ★ Routine |
| **C23** | 100nF | C_0603_1608Metric | (99.8, 95.2) | 0° | ★ Routine |
| **C28** | 1uF | C_0603_1608Metric | (86.5, 87.7) | 0° | ★ Routine |
| **C29** | 1uF | C_0603_1608Metric | (89.5, 87.7) | 0° | ★ Routine |
| **R10** | 27R | R_0603_1608Metric | (95.6, 87.7) | 180° | ★ Routine |
| **R9** | 27R | R_0603_1608Metric | (92.5, 87.7) | 180° | ★ Routine |
| **R50** | 10k | R_0603_1608Metric | (94.0, 85.2) | 90° | ★ Routine |
| **C87** | 10nF C0G | C_0603_1608Metric | (97.8, 84.4) | 90° | ★ Routine |
| **C20** | 15pF C0G | C_0603_1608Metric | (107.6, 87.7) | 180° | ★ Routine |
| **R4** | 1k | R_0603_1608Metric | (110.6, 87.7) | 0° | ★ Routine |
| **C19** | 15pF C0G | C_0603_1608Metric | (104.6, 87.7) | 0° | ★ Routine |
| **C25** | 100nF | C_0603_1608Metric | (109.2, 77.4) | 0° | ★ Routine |
| **C83** | 100nF | C_0402_1005Metric | (93.6, 79.5) | -90° | ★★ 0402; use low airflow |
| **C85** | 100nF | C_0402_1005Metric | (96.0, 79.5) | 0° | ★★ 0402; use low airflow |
| **C86** | 100nF | C_0402_1005Metric | (96.0, 82.0) | 0° | ★★ 0402; use low airflow |
| **C84** | 1uF | C_0402_1005Metric | (93.6, 82.0) | 0° | ★★ 0402; use low airflow |
| **C82** | 100nF | C_0402_1005Metric | (83.0, 84.5) | -90° | ★★ 0402; use low airflow |
| **D5** | TPESD8L3.3 0.3pFtyp 0.5pFmax | D_0402_1005Metric | (60.0, 93.6) | 180° | ★★ 0402; use low airflow |
| **D4** | TPESD8L3.3 0.3pFtyp 0.5pFmax | D_0402_1005Metric | (60.0, 90.4) | 180° | ★★ 0402; use low airflow |

## Stage C: Sensors + GNSS (33 parts)

| Designator | Value / Model | Footprint | Board position | Rotation | Manual difficulty |
|---|---|---|---|---|---|
| **U7** | ATGM336H-6N-74 | ATGM336H_LCC-18 | (70.4, 66.4) | 0° | ★★★ Exposed pad; reflow/hot air required |
| **U4** | BNO085 | BNO085_LGA-28 | (63.7, 78.3) | 0° | ★★★ Exposed pad; reflow/hot air required |
| **J2** | U.FL_GNSS_EXT(经尾线转SMA) | U.FL_Hirose_U.FL-R-SMT-1_V | (52.9, 74.1) | 180° | ★ Routine |
| **J8** | U.FL→内置patch | U.FL_Hirose_U.FL-R-SMT-1_V | (52.9, 64.4) | 180° | ★ Routine |
| **U6** | QMC5883P | LGA-16_3x3mm_P0.5mm | (71.3, 77.0) | 0° | ★★★ Exposed pad; reflow/hot air required |
| **Q5** | AO3401A | SOT-23 | (57.8, 72.6) | 0° | ★ Exposed leads; hand touch-up possible |
| **Q4** | AO3401A | SOT-23 | (61.8, 70.2) | 0° | ★ Exposed leads; hand touch-up possible |
| **U5** | BMP388 | BMP388_LGA-10 | (71.3, 81.2) | 0° | ★★★ Exposed pad; reflow/hot air required |
| **C18** | 10uF | C_0805_2012Metric | (78.9, 64.6) | 180° | ★ Routine |
| **U17** | XA17-G4K | SOT-363_SC-70-6 | (58.8, 63.3) | 0° | ★ Exposed leads; hand touch-up possible |
| **F5** | 6V/200mA | Fuse_0805_2012Metric | (56.4, 69.7) | 0° | ★ Routine |
| **F4** | 6V/200mA | Fuse_0805_2012Metric | (61.8, 67.4) | 0° | ★ Routine |
| **R27** | 10k | R_0603_1608Metric | (61.8, 73.3) | 0° | ★ Routine |
| **C57** | 100pF | C_0402_1005Metric | (63.4, 63.3) | -90° | ★★ 0402; use low airflow |
| **R55** | 10k | R_0603_1608Metric | (59.1, 80.3) | 90° | ★ Routine |
| **C17** | 100nF | C_0603_1608Metric | (78.5, 61.4) | 0° | ★ Routine |
| **R3** | 4.7k | R_0603_1608Metric | (61.4, 108.7) | 0° | ★ Routine |
| **C14** | 100nF | C_0603_1608Metric | (61.8, 87.2) | 0° | ★ Routine |
| **C15** | 4.7uF | C_0603_1608Metric | (66.1, 87.2) | 180° | ★ Routine |
| **R1** | 10k | R_0603_1608Metric | (79.0, 87.2) | 180° | ★ Routine |
| **C16** | 100nF | C_0603_1608Metric | (70.4, 87.2) | 0° | ★ Routine |
| **C11** | 100nF | C_0603_1608Metric | (70.6, 84.5) | 0° | ★ Routine |
| **C13** | 100nF | C_0603_1608Metric | (79.1, 84.5) | 0° | ★ Routine |
| **C12** | 100nF | C_0603_1608Metric | (74.8, 84.5) | 0° | ★ Routine |
| **C10** | 100nF | C_0603_1608Metric | (66.4, 84.5) | 0° | ★ Routine |
| **R2** | 4.7k | R_0603_1608Metric | (61.4, 106.1) | 0° | ★ Routine |
| **R26** | 10k | R_0603_1608Metric | (58.8, 76.4) | 0° | ★ Routine |
| **L15** | 33nH | L_0402_1005Metric | (58.0, 67.6) | 90° | ★★ 0402; use low airflow |
| **R52** | 10k | R_0402_1005Metric | (60.0, 83.0) | 0° | ★★ 0402; use low airflow |
| **R53** | 10k | R_0402_1005Metric | (63.0, 84.4) | 180° | ★★ 0402; use low airflow |
| **C59** | 100pF | C_0402_1005Metric | (56.3, 62.4) | 0° | ★★ 0402; use low airflow |
| **C58** | 100pF | C_0402_1005Metric | (56.5, 64.6) | 90° | ★★ 0402; use low airflow |
| **L2** | 33nH | L_0402_1005Metric | (61.8, 64.8) | 90° | ★★ 0402; use low airflow |

## Stage D: 978 Transceiver (34 parts)

| Designator | Value / Model | Footprint | Board position | Rotation | Manual difficulty |
|---|---|---|---|---|---|
| **U10** | CC1312R1F3RGZR | QFN-48-1EP_7x7mm_P0.5mm_EP | (126.2, 85.0) | 0° | ★★★ Exposed pad; reflow/hot air required |
| **J5** | U.FL_978 | U.FL_Hirose_U.FL-R-SMT-1_V | (145.5, 83.5) | 0° | ★ Routine |
| **Y3** | 32.768kHz FC-135 | Crystal_SMD_3215-2Pin_3.2x | (122.7, 91.3) | 0° | ★ Routine |
| **Y2** | 48MHz ABM8W-7pF | Crystal_SMD_3225-4Pin_3.2x | (133.8, 81.7) | 90° | ★ Routine |
| **Q2** | AO3401A | SOT-23 | (125.1, 105.2) | 0° | ★ Exposed leads; hand touch-up possible |
| **F2** | 6V/200mA | Fuse_0805_2012Metric | (129.8, 105.2) | -90° | ★ Routine |
| **C66** | 22uF | C_0805_2012Metric | (123.2, 100.5) | 0° | ★ Routine |
| **C60** | 22uF | C_0805_2012Metric | (136.2, 93.1) | 0° | ★ Routine |
| **L7** | 6.8uH | L_0805_2012Metric | (131.5, 93.1) | 0° | ★ Routine |
| **R17** | 10k | R_0603_1608Metric | (121.4, 105.2) | 90° | ★ Routine |
| **C69** | 18pF | C_0603_1608Metric | (127.7, 100.5) | 0° | ★ Routine |
| **C68** | 18pF | C_0603_1608Metric | (123.1, 93.8) | 0° | ★ Routine |
| **R47** | 10k | R_0603_1608Metric | (132.5, 85.0) | 0° | ★ Routine |
| **R56** | 10k | R_0603_1608Metric | (132.3, 89.7) | 0° | ★ Routine |
| **C65** | 100nF | C_0603_1608Metric | (136.8, 97.0) | 0° | ★ Routine |
| **C63** | 100nF | C_0603_1608Metric | (129.0, 97.0) | 0° | ★ Routine |
| **C64** | 100nF | C_0603_1608Metric | (132.9, 97.0) | 0° | ★ Routine |
| **C62** | 100nF | C_0603_1608Metric | (125.1, 97.0) | 0° | ★ Routine |
| **C61** | 100nF | C_0603_1608Metric | (121.2, 97.0) | 180° | ★ Routine |
| **C67** | 1uF | C_0603_1608Metric | (126.9, 93.1) | 180° | ★ Routine |
| **C39** | 100pF | C_0402_1005Metric | (140.0, 83.6) | -90° | ★★ 0402; use low airflow |
| **C40** | 3.6pF | C_0402_1005Metric | (123.6, 76.3) | 0° | ★★ 0402; use low airflow |
| **C41** | 2.7pF | C_0402_1005Metric | (126.9, 76.3) | 0° | ★★ 0402; use low airflow |
| **C42** | 6.2pF | C_0402_1005Metric | (130.2, 76.3) | 180° | ★★ 0402; use low airflow |
| **C45** | 100pF | C_0402_1005Metric | (119.2, 84.3) | 180° | ★★ 0402; use low airflow |
| **C43** | 3pF | C_0402_1005Metric | (137.6, 78.3) | 180° | ★★ 0402; use low airflow |
| **L13** | 7.5nH | L_0402_1005Metric | (119.2, 83.2) | 0° | ★★ 0402; use low airflow |
| **L12** | 6.8nH | L_0402_1005Metric | (137.6, 76.3) | 0° | ★★ 0402; use low airflow |
| **L11** | 6.8nH | L_0402_1005Metric | (134.2, 76.3) | 0° | ★★ 0402; use low airflow |
| **L10** | 27nH | L_0402_1005Metric | (120.8, 76.3) | 0° | ★★ 0402; use low airflow |
| **C44** | 3.6pF | C_0402_1005Metric | (121.7, 78.0) | 0° | ★★ 0402; use low airflow |
| **L8** | 100nH | L_0402_1005Metric | (142.6, 87.0) | 90° | ★★ 0402; use low airflow |
| **D3** | TPESD8L3.3 0.3pFtyp 0.5pFmax | D_0402_1005Metric | (140.0, 86.9) | -90° | ★★ 0402; use low airflow |
| **L9** | 7.5nH | L_0402_1005Metric | (124.2, 78.0) | 0° | ★★ 0402; use low airflow |

## Stage E: 1090 Receive Chain (49 parts)

| Designator | Value / Model | Footprint | Board position | Rotation | Manual difficulty |
|---|---|---|---|---|---|
| **U14** | AD8313ARMZ | MSOP-8_3x3mm_P0.65mm | (114.1, 82.1) | 0° | ★ Exposed leads; hand touch-up possible |
| **FL2** | TA0970A | TA0970A_SMD3838-6 | (125.1, 71.2) | 0° | ★★★ Exposed pad; reflow/hot air required |
| **FL1** | TA0970A | TA0970A_SMD3838-6 | (110.2, 71.2) | 0° | ★★★ Exposed pad; reflow/hot air required |
| **J6** | U.FL_1090_EXT(经尾线转SMA) | U.FL_Hirose_U.FL-R-SMT-1_V | (124.3, 62.1) | 0° | ★ Routine |
| **J7** | U.FL_IFA_TEST(π后调试口) | U.FL_Hirose_U.FL-R-SMT-1_V | (103.1, 61.3) | 90° | ★ Routine |
| **U15** | TLV3501(TOKMAS) | SOT-23-6 | (83.1, 91.3) | 0° | ★ Exposed leads; hand touch-up possible |
| **Q3** | AO3401A | SOT-23 | (132.0, 62.1) | 180° | ★ Exposed leads; hand touch-up possible |
| **U11** | QPL9547TR7 | DFN-8-1EP_2x2mm_P0.5mm_EP0 | (101.6, 71.2) | 0° | ★★★ Exposed pad; reflow/hot air required |
| **U12** | BGA2817 | SOT-363_SC-70-6 | (117.7, 71.2) | 180° | ★ Exposed leads; hand touch-up possible |
| **U16** | XA17-G4K | SOT-363_SC-70-6 | (114.6, 62.1) | 0° | ★ Exposed leads; hand touch-up possible |
| **F3** | 6V/200mA | Fuse_0805_2012Metric | (140.2, 62.1) | 0° | ★ Routine |
| **R18** | 10k | R_0603_1608Metric | (136.2, 62.1) | 0° | ★ Routine |
| **R57** | 33R | R_0603_1608Metric | (85.7, 95.3) | -90° | ★ Routine |
| **C51** | 1nF | C_0603_1608Metric | (55.6, 85.2) | 180° | ★ Routine |
| **ZS1** | 0R 串 | L_0603_1608Metric | (88.0, 62.9) | 0° | ★ Routine |
| **C49** | 100nF | C_0603_1608Metric | (55.6, 80.1) | 180° | ★ Routine |
| **R34** | 10k | R_0603_1608Metric | (55.6, 77.5) | 0° | ★ Routine |
| **R35** | 10k | R_0603_1608Metric | (55.6, 82.6) | 0° | ★ Routine |
| **R21** | 0R | R_0603_1608Metric | (115.2, 85.0) | 180° | ★ Routine |
| **R32** | 10k | R_0603_1608Metric | (112.5, 75.2) | 0° | ★ Routine |
| **R33** | 100k | R_0603_1608Metric | (108.7, 75.2) | 180° | ★ Routine |
| **C46** | 3pF | C_0603_1608Metric | (118.7, 86.2) | 180° | ★ Routine |
| **C47** | 200pF | C_0603_1608Metric | (118.7, 88.4) | 0° | ★ Routine |
| **C55** | 100pF | C_0603_1608Metric | (90.2, 74.0) | 0° | ★ Routine |
| **C56** | 100pF | C_0603_1608Metric | (85.1, 74.0) | 180° | ★ Routine |
| **R22** | 0R | R_0603_1608Metric | (87.5, 71.2) | 0° | ★ Routine |
| **R23** | 0R | R_0603_1608Metric | (91.4, 71.2) | 180° | ★ Routine |
| **R31** | 1k | R_0603_1608Metric | (115.0, 87.3) | 0° | ★ Routine |
| **C81** | 470pF C0G | C_0402_1005Metric | (118.6, 73.5) | 90° | ★★ 0402; use low airflow |
| **C38** | 100pF | C_0402_1005Metric | (137.3, 69.2) | 0° | ★★ 0402; use low airflow |
| **C31** | 100pF C0G | C_0402_1005Metric | (106.0, 71.2) | 0° | ★★ 0402; use low airflow |
| **R54** | 10R | R_0402_1005Metric | (109.6, 82.1) | 90° | ★★ 0402; use low airflow |
| **C53** | 100pF | C_0402_1005Metric | (110.2, 61.2) | 0° | ★★ 0402; use low airflow |
| **C30** | 100pF | C_0402_1005Metric | (120.4, 62.1) | 180° | ★★ 0402; use low airflow |
| **C54** | 100pF | C_0402_1005Metric | (105.3, 68.1) | 180° | ★★ 0402; use low airflow |
| **R11** | 3.32k | R_0402_1005Metric | (98.8, 68.0) | 90° | ★★ 0402; use low airflow |
| **R19** | 52.3R | R_0402_1005Metric | (138.5, 71.2) | 0° | ★★ 0402; use low airflow |
| **C52** | 100nF | C_0402_1005Metric | (60.4, 96.2) | 0° | ★★ 0402; use low airflow |
| **C21** | 100pF | C_0402_1005Metric | (101.0, 67.5) | 90° | ★★ 0402; use low airflow |
| **C35** | 100pF C0G | C_0402_1005Metric | (141.2, 71.2) | 0° | ★★ 0402; use low airflow |
| **C34** | 100pF C0G | C_0402_1005Metric | (129.4, 71.2) | 0° | ★★ 0402; use low airflow |
| **C33** | 100pF C0G | C_0402_1005Metric | (120.9, 71.2) | 0° | ★★ 0402; use low airflow |
| **C32** | 100pF C0G | C_0402_1005Metric | (114.5, 71.2) | 0° | ★★ 0402; use low airflow |
| **C36** | 100pF | C_0402_1005Metric | (103.0, 66.5) | 0° | ★★ 0402; use low airflow |
| **C48** | 1uF | C_0402_1005Metric | (99.6, 65.5) | 90° | ★★ 0402; use low airflow |
| **C37** | 100nF | C_0402_1005Metric | (109.6, 85.0) | 0° | ★★ 0402; use low airflow |
| **L14** | 100nH | L_0402_1005Metric | (120.4, 63.3) | 0° | ★★ 0402; use low airflow |
| **L1** | 18nH 0402CS-18NXGRW | L_0402_1005Metric | (105.5, 69.8) | 90° | ★★ 0402; use low airflow |
| **D2** | TPESD8L3.3 0.3pFtyp 0.5pFmax | D_0402_1005Metric | (128.3, 62.1) | 0° | ★★ 0402; use low airflow |

## Stage F: External Interface (1 parts)

| Designator | Value / Model | Footprint | Board position | Rotation | Manual difficulty |
|---|---|---|---|---|---|
| **J1** | J3_HAT_2x20_SMD排针 | PinHeader_2x20_P2.54mm_Ver | (92.6, 106.3) | 90° | ★ Routine |

## Not placed (20 positions)

| Designator | Reason |
|---|---|
| **ANT1** | 板载天线，PCB铜箔本身，无器件；V3.10已回灌50.0mm外包络/48.5mm中心线 |
| **H1** | M2.5 安装孔（⌀2.7mm NPTH），不是元件 |
| **H2** | M2.5 安装孔（⌀2.7mm NPTH），不是元件 |
| **H3** | M2.5 安装孔（⌀2.7mm NPTH），不是元件 |
| **H4** | M2.5 安装孔（⌀2.7mm NPTH），不是元件 |
| **R24** | DNP，设计上默认不贴（0R DNP(旁路外接)） |
| **R25** | DNP，设计上默认不贴（0R DNP(旁路板载)） |
| **R30** | DNP，设计上默认不贴（1k DNP） |
| **R36** | DNP，设计上默认不贴（1k DNP） |
| **SW1** | 短接焊盘，用镊子短接，不贴件 |
| **SW2** | 短接焊盘，用镊子短接，不贴件 |
| **TP1** | 测试点焊盘，探针/飞线用，不贴件 |
| **TP2** | 测试点焊盘，探针/飞线用，不贴件 |
| **TP3** | 测试点焊盘，探针/飞线用，不贴件 |
| **TP4** | 测试点焊盘，探针/飞线用，不贴件 |
| **TP5** | 测试点焊盘，探针/飞线用，不贴件 |
| **TP6** | 测试点焊盘，探针/飞线用，不贴件 |
| **TP7** | 测试点焊盘，探针/飞线用，不贴件 |
| **ZP1** | DNP，设计上默认不贴（DNP 并-天线侧） |
| **ZP2** | DNP，设计上默认不贴（DNP 并-电台侧） |
