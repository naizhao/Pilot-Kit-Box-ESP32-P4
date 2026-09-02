#!/usr/bin/env python3
"""MCU_RP2040 页：RP2040 + W25Q128 + 12MHz 晶振 + USB-C(UF2 刷机) + RUN/BOOTSEL + SWD/调试 TP。

依据 PINMAP.md §2；GPIO2/3 新分配为 1090/978 天线偏置 Tee 使能（PINMAP 同步更新）。
晶振电路按 RPi 硬件设计指南：12MHz + 2×15pF + XOUT 串 1k。
USB 按 RP2040 minimal design：DP/DM 串 27R；VBUS 经肖特基并入 VCC_5V（与 J3 5V 或运行）。
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from gen_sch import Sheet

C0402 = "Capacitor_SMD:C_0402_1005Metric"
R0402 = "Resistor_SMD:R_0402_1005Metric"

s = Sheet("mcu", "Expansion Board V1 - MCU RP2040 + Flash + USB")

s.place("U8", "MCU_RaspberryPi", "RP2040", 101.6, 101.6, {
    # 电源
    "IOVDD": "3V3_DIG", "USB_VDD": "3V3_DIG", "VREG_VIN": "3V3_DIG",
    "ADC_AVDD": "3V3_DIG", "DVDD": "RP_1V1", "VREG_VOUT": "RP_1V1",
    "TESTEN": "GND", "GND": "GND",
    # UART → P4
    "GPIO0": "ADSB_TXD", "GPIO1": "ADSB_RXD",
    # 偏置 Tee 使能（低有效，PMOS 栅极）
    "GPIO2": "BIAS_EN_1090", "GPIO3": "BIAS_EN_978",
    # 天线切换控制（各一对互补脚驱动 SPDT 开关）
    "GPIO4": "ANT_SEL_1090_A", "GPIO5": "ANT_SEL_1090_B",
    "GPIO6": "ANT_SEL_GNSS_A", "GPIO7": "ANT_SEL_GNSS_B",
    "GPIO8": "NC", "GPIO9": "NC",
    # CC1312R：SPI master + 中断/同步 + cJTAG 代刷
    "GPIO10": "SUBG_SCK", "GPIO11": "SUBG_MOSI", "GPIO12": "SUBG_MISO",
    "GPIO13": "SUBG_CSN", "GPIO14": "SUBG_IRQ", "GPIO15": "SUBG_SYNC",
    "GPIO16": "SUBG_TMSC", "GPIO17": "SUBG_TCKC", "GPIO18": "SUBG_RESET",
    # 1090 解码
    "GPIO19": "PULSES",
    "GPIO20": "DEMOD0", "GPIO21": "DEMOD1", "GPIO22": "DEMOD2", "GPIO23": "DEMOD3",
    "GPIO24": "RECOVERED_CLK", "GPIO25": "TL_PWM",
    "GPIO26/ADC0": "LEVEL_BIAS", "GPIO27/ADC1": "RSSI",
    "GPIO28/ADC2": "USB_VBUS_SENSE", "GPIO29/ADC3": "NC",
    # QSPI Flash
    "QSPI_SCLK": "QSPI_SCLK", "QSPI_SD0": "QSPI_SD0", "QSPI_SD1": "QSPI_SD1",
    "QSPI_SD2": "QSPI_SD2", "QSPI_SD3": "QSPI_SD3", "~{QSPI_SS}": "QSPI_SS",
    # 时钟/控制/调试
    "XIN": "RP_XIN", "XOUT": "RP_XOUT", "RUN": "RP_RUN",
    "SWCLK": "SWCLK", "SWDIO": "SWDIO",
    "USB_DP": "RP_USB_DP", "USB_DM": "RP_USB_DM",
}, footprint="Package_DFN_QFN:QFN-56-1EP_7x7mm_P0.4mm_EP3.2x3.2mm")

# Flash
s.place("U9", "Memory_Flash", "W25Q128JVS", 190.5, 76.2, {
    "~{CS}": "QSPI_SS", "CLK": "QSPI_SCLK",
    "DI/IO_{0}": "QSPI_SD0", "DO/IO_{1}": "QSPI_SD1",
    "~{WP}/IO_{2}": "QSPI_SD2", "~{HOLD}/~{RESET}/IO_{3}": "QSPI_SD3",
    "VCC": "3V3_DIG", "GND": "GND",
}, value="W25Q128JVSIQ", footprint="Package_SO:SOIC-8_5.3x5.3mm_P1.27mm")

# 晶振：12MHz 3225 四脚（2/4 = GND 壳）
s.place("Y1", "Device", "Crystal_GND24", 190.5, 114.3,
        {"1": "RP_XIN", "3": "RP_XT2", "G": "GND"},
        value="12MHz CL=10pF ABM8-272-T3",
        footprint="Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm")
s.place("R4", "Device", "R", 215.9, 114.3, {"1": "RP_XOUT", "2": "RP_XT2"}, value="1k", footprint=R0402)
s.place("C19", "Device", "C", 228.6, 114.3, {"1": "RP_XIN", "2": "GND"}, value="15pF C0G", footprint=C0402)
s.place("C20", "Device", "C", 241.3, 114.3, {"1": "RP_XT2", "2": "GND"}, value="15pF C0G", footprint=C0402)

# RUN / BOOTSEL
s.place("R5", "Device", "R", 190.5, 139.7, {"1": "3V3_DIG", "2": "RP_RUN"}, value="10k", footprint=R0402)
s.place("SW1", "Jumper", "SolderJumper_2_Open", 215.9, 139.7, {"A": "RP_RUN", "B": "GND"},
        value="RESET短接焊盘", footprint="Jumper:SolderJumper-2_P1.3mm_Open_Pad1.0x1.5mm")
s.place("R6", "Device", "R", 190.5, 152.4, {"1": "3V3_DIG", "2": "QSPI_SS"}, value="10k", footprint=R0402)
s.place("SW2", "Jumper", "SolderJumper_2_Open", 215.9, 152.4, {"A": "QSPI_SS", "B": "GND"},
        value="BOOTSEL短接焊盘", footprint="Jumper:SolderJumper-2_P1.3mm_Open_Pad1.0x1.5mm")

# USB-C（仅 USB2.0 刷机/调试）
s.place("J4", "Connector", "USB_C_Receptacle_USB2.0_16P", 63.5, 190.5, {
    "VBUS": "USB_VBUS", "GND": "GND", "SHIELD": "GND",
    "CC1": "USB_CC1", "CC2": "USB_CC2",
    "D+": "USB_DP", "D-": "USB_DM",
    "SBU1": "NC", "SBU2": "NC",
}, value="USB-C_16P", footprint="Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12")
s.place("R7", "Device", "R", 114.3, 190.5, {"1": "USB_CC1", "2": "GND"}, value="5.1k", footprint=R0402)
s.place("R8", "Device", "R", 127, 190.5, {"1": "USB_CC2", "2": "GND"}, value="5.1k", footprint=R0402)
s.place("R9", "Device", "R", 139.7, 190.5, {"1": "RP_USB_DP", "2": "USB_DP"}, value="27R", footprint=R0402)
s.place("R10", "Device", "R", 152.4, 190.5, {"1": "RP_USB_DM", "2": "USB_DM"}, value="27R", footprint=R0402)
s.place("D4", "Device", "D_TVS", 165.1, 203.2, {"A1": "USB_DP", "A2": "GND"},
        value="TPESD8L3.3 0.3pFtyp 0.5pFmax", footprint="Diode_SMD:D_0402_1005Metric")
s.place("D5", "Device", "D_TVS", 177.8, 203.2, {"A1": "USB_DM", "A2": "GND"},
        value="TPESD8L3.3 0.3pFtyp 0.5pFmax", footprint="Diode_SMD:D_0402_1005Metric")
s.place("R50", "Device", "R", 190.5, 203.2,
        {"1": "USB_VBUS", "2": "USB_VBUS_SENSE"}, value="10k", footprint=R0402)
s.place("R51", "Device", "R", 203.2, 203.2,
        {"1": "USB_VBUS_SENSE", "2": "GND"}, value="10k", footprint=R0402)
s.place("C87", "Device", "C", 215.9, 203.2,
        {"1": "USB_VBUS_SENSE", "2": "GND"}, value="10nF C0G", footprint=C0402)
s.place("D1", "Device", "D_Schottky", 165.1, 190.5, {"A": "USB_VBUS", "K": "VCC_5V"},
        value="B5819W", footprint="Diode_SMD:D_SOD-123")

# 去耦：IOVDD×6 + 1V1×2；VREG_VIN、USB_VDD、ADC_AVDD 另设本地去耦。
for i in range(6):
    s.place(f"C{22+i}", "Device", "C", 63.5 + i * 12.7, 63.5, {"1": "3V3_DIG", "2": "GND"},
            value="100nF", footprint=C0402)
s.place("C28", "Device", "C", 63.5, 76.2, {"1": "RP_1V1", "2": "GND"}, value="1uF", footprint=C0402)
s.place("C29", "Device", "C", 76.2, 76.2, {"1": "RP_1V1", "2": "GND"}, value="1uF", footprint=C0402)
s.place("C82", "Device", "C", 88.9, 76.2, {"1": "RP_1V1", "2": "GND"},
        value="100nF", footprint=C0402)
s.place("C83", "Device", "C", 101.6, 76.2, {"1": "RP_1V1", "2": "GND"},
        value="100nF", footprint=C0402)
s.place("C84", "Device", "C", 114.3, 76.2, {"1": "3V3_DIG", "2": "GND"},
        value="1uF", footprint=C0402)
s.place("C85", "Device", "C", 127.0, 76.2, {"1": "3V3_DIG", "2": "GND"},
        value="100nF", footprint=C0402)
s.place("C86", "Device", "C", 139.7, 76.2, {"1": "3V3_DIG", "2": "GND"},
        value="100nF", footprint=C0402)

# 调试测试点
TPS = [("TP1", "SWCLK"), ("TP2", "SWDIO"), ("TP3", "DEMOD0"), ("TP4", "DEMOD1"),
       ("TP5", "DEMOD2"), ("TP6", "DEMOD3"), ("TP7", "RECOVERED_CLK")]
for i, (ref, net) in enumerate(TPS):
    s.place(ref, "Connector", "TestPoint", 63.5 + i * 12.7, 241.3, {"1": net},
            footprint="TestPoint:TestPoint_Pad_1.0x1.0mm")

s.write()
