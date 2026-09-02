#!/usr/bin/env python3
"""SubGHz_978 页：CC1312R + 差分 lattice balun 匹配 + 偏置 Tee + 双晶振 + DCDC 外围。

网表逐节点依据参考原理图转录（2026-08-01 转录报告）：
- 匹配：RF_P—L9(7.5n)—N3；RF_N—C40(3.6p)—N3（lattice 合成）；L10(27n) 跨接 P/N；
  C44(3.6p) RF_P 对地；RX_TX 经 L13(7.5n) 接 RF_N、经 C45(100p) 交流接地；
  N3—L11(6.8n)—N4—L12(6.8n)—N5—C39(100p)—天线；C41(2.7p)/C42(6.2p)/C43(3p) 依次对地
- 电源：VDDS×4 → 3V3_DIG；DCDC_SW—L7(6.8uH)—VDDR/VDDR_RF（内部 DCDC）；DCOUPL—1uF
- 晶振：48M 无外部负载电容（片内阵列）；32k 带 12pF/15pF（原图不对称，照抄）
- 偏置 Tee：PMOS S→3V3_GNSS，G=10k 上拉+BIAS_EN_978(低有效)，D→保险丝→100nH→天线节点；978 侧无 ESD（照抄，V1 维持）
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from gen_sch import Sheet

C0402 = "Capacitor_SMD:C_0402_1005Metric"
R0402 = "Resistor_SMD:R_0402_1005Metric"
L0402 = "Inductor_SMD:L_0402_1005Metric"

s = Sheet("subghz", "Expansion Board V1 - 978MHz UAT: CC1312R")

s.place("U10", "MCU_Texas_SimpleLink", "CC1312R1F3RGZ", 88.9, 114.3, {
    "RF_P": "SUBG_RFP", "RF_N": "SUBG_RFN", "RX_TX": "SUBG_RXTX",
    "X32K_Q1": "SUBG_X32K1", "X32K_Q2": "SUBG_X32K2",
    "X48M_P": "SUBG_X48P", "X48M_N": "SUBG_X48N",
    "JTAG_TMSC": "SUBG_TMSC", "JTAG_TCKC": "SUBG_TCKC", "~{RESET}": "SUBG_RESET",
    "DIO_8": "SUBG_MOSI", "DIO_9": "SUBG_MISO", "DIO_10": "SUBG_SCK",
    "DIO_11": "SUBG_CSN", "DIO_12": "SUBG_IRQ", "DIO_13": "SUBG_SYNC",
    "DIO_1": "NC", "DIO_2": "NC", "DIO_3": "NC", "DIO_4": "NC", "DIO_5": "NC",
    "DIO_6": "NC", "DIO_7": "NC", "DIO_14": "NC", "DIO_15": "NC",
    "DIO_16/JTAG_TDO": "NC", "DIO_17/JTAG_TDI": "NC", "DIO_18": "NC",
    "DIO_19": "NC", "DIO_20": "NC", "DIO_21": "NC", "DIO_22": "NC",
    "DIO_23": "NC", "DIO_24": "NC", "DIO_25": "NC", "DIO_26": "NC",
    "DIO_27": "NC", "DIO_28": "NC", "DIO_29": "NC", "DIO_30": "NC",
    "VDDS": "3V3_DIG", "VDDS2": "3V3_DIG", "VDDS3": "3V3_DIG", "VDDS_DCDC": "3V3_DIG",
    "VDDR": "SUBG_VDDR", "VDDR_RF": "SUBG_VDDR",
    "DCDC_SW": "SUBG_SW", "DCOUPL": "SUBG_DCOUPL", "GND": "GND",
}, value="CC1312R1F3RGZR",
   footprint="Package_DFN_QFN:QFN-48-1EP_7x7mm_P0.5mm_EP5.15x5.15mm")
s.place("R47", "Device", "R", 127, 114.3,
        {"1": "3V3_DIG", "2": "SUBG_RESET"}, value="10k", footprint=R0402)

# 差分匹配（lattice balun，位号沿用转录以便对图复查）
s.place("L10", "Device", "L", 165.1, 63.5, {"1": "SUBG_RFP", "2": "SUBG_RFN"}, value="27nH", footprint=L0402)
s.place("C44", "Device", "C", 177.8, 63.5, {"1": "SUBG_RFP", "2": "GND"}, value="3.6pF", footprint=C0402)
s.place("L9", "Device", "L", 190.5, 63.5, {"1": "SUBG_RFP", "2": "SUBG_N3"}, value="7.5nH", footprint=L0402)
s.place("C40", "Device", "C", 203.2, 63.5, {"1": "SUBG_RFN", "2": "SUBG_N3"}, value="3.6pF", footprint=C0402)
s.place("L13", "Device", "L", 215.9, 63.5, {"1": "SUBG_RFN", "2": "SUBG_RXTX"}, value="7.5nH", footprint=L0402)
s.place("C45", "Device", "C", 228.6, 63.5, {"1": "SUBG_RXTX", "2": "GND"}, value="100pF", footprint=C0402)
s.place("C41", "Device", "C", 165.1, 76.2, {"1": "SUBG_N3", "2": "GND"}, value="2.7pF", footprint=C0402)
s.place("L11", "Device", "L", 177.8, 76.2, {"1": "SUBG_N3", "2": "SUBG_N4"}, value="6.8nH", footprint=L0402)
s.place("C42", "Device", "C", 190.5, 76.2, {"1": "SUBG_N4", "2": "GND"}, value="6.2pF", footprint=C0402)
s.place("L12", "Device", "L", 203.2, 76.2, {"1": "SUBG_N4", "2": "SUBG_N5"}, value="6.8nH", footprint=L0402)
s.place("C43", "Device", "C", 215.9, 76.2, {"1": "SUBG_N5", "2": "GND"}, value="3pF", footprint=C0402)
s.place("C39", "Device", "C", 228.6, 76.2, {"1": "SUBG_N5", "2": "ANT_978"}, value="100pF", footprint=C0402)

# 偏置 Tee（Q/F/L 参照转录：PMOS→保险丝→100nH→天线节点；978 侧无 ESD）
s.place("Q2", "Transistor_FET", "AO3401A", 165.1, 101.6, {"G": "BIAS_EN_978", "S": "3V3_GNSS", "D": "SUBG_FUSE"},
        value="AO3401A", footprint="Package_TO_SOT_SMD:SOT-23")
# 片选上拉。SUBG_CSN 只挂 U10.17 和 RP2040 的 GPIO13，MCU 复位期间那个 GPIO 是
# 高阻，CC1312R 会被随机选中并把总线噪声当命令吃进去。上拉到驱动侧的 3V3_DIG。
s.place("R56", "Device", "R", 228.6, 127.0,
        {"1": "3V3_DIG", "2": "SUBG_CSN"}, value="10k", footprint=R0402)
s.place("R17", "Device", "R", 190.5, 101.6, {"1": "3V3_DIG", "2": "BIAS_EN_978"}, value="10k", footprint=R0402)
s.place("F2", "Device", "Polyfuse", 203.2, 101.6, {"1": "SUBG_FUSE", "2": "SUBG_FEED"},
        value="6V/200mA", footprint="Fuse:Fuse_0805_2012Metric")
s.place("L8", "Device", "L", 215.9, 101.6, {"1": "SUBG_FEED", "2": "ANT_978"}, value="100nH", footprint=L0402)
s.place("D3", "Device", "D_TVS", 254, 114.3, {"A1": "ANT_978", "A2": "GND"},
        value="TPESD8L3.3 0.3pFtyp 0.5pFmax", footprint="Diode_SMD:D_0402_1005Metric")
s.place("J5", "Connector", "Conn_Coaxial", 241.3, 101.6, {"1": "ANT_978", "2": "GND"},
        value="U.FL_978", footprint="Connector_Coaxial:U.FL_Hirose_U.FL-R-SMT-1_Vertical")

# 电源外围（照转录：DCDC 6.8uH + 22uF；VDDS 0.1uF×3 + 22uF；DCOUPL 1uF）
s.place("L7", "Device", "L", 165.1, 127, {"1": "SUBG_SW", "2": "SUBG_VDDR"}, value="6.8uH",
        footprint="Inductor_SMD:L_0805_2012Metric")
s.place("C60", "Device", "C", 177.8, 127, {"1": "SUBG_VDDR", "2": "GND"}, value="22uF", footprint="Capacitor_SMD:C_0805_2012Metric")
s.place("C61", "Device", "C", 190.5, 127, {"1": "SUBG_VDDR", "2": "GND"}, value="100nF", footprint=C0402)
s.place("C62", "Device", "C", 203.2, 127, {"1": "SUBG_VDDR", "2": "GND"}, value="100nF", footprint=C0402)
s.place("C67", "Device", "C", 215.9, 127, {"1": "SUBG_DCOUPL", "2": "GND"}, value="1uF", footprint=C0402)
s.place("C63", "Device", "C", 165.1, 139.7, {"1": "3V3_DIG", "2": "GND"}, value="100nF", footprint=C0402)
s.place("C64", "Device", "C", 177.8, 139.7, {"1": "3V3_DIG", "2": "GND"}, value="100nF", footprint=C0402)
s.place("C65", "Device", "C", 190.5, 139.7, {"1": "3V3_DIG", "2": "GND"}, value="100nF", footprint=C0402)
s.place("C66", "Device", "C", 203.2, 139.7, {"1": "3V3_DIG", "2": "GND"}, value="22uF", footprint="Capacitor_SMD:C_0805_2012Metric")

# 晶振：48M 无外部负载电容（片内阵列）；32k 12/15pF 照抄
s.place("Y2", "Device", "Crystal_GND24", 165.1, 152.4,
        {"1": "SUBG_X48P", "3": "SUBG_X48N", "G": "GND"},
        value="48MHz ABM8W-7pF", footprint="Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm")
s.place("Y3", "Device", "Crystal", 190.5, 152.4, {"1": "SUBG_X32K1", "2": "SUBG_X32K2"},
        value="32.768kHz FC-135", footprint="Crystal:Crystal_SMD_3215-2Pin_3.2x1.5mm")
s.place("C68", "Device", "C", 203.2, 152.4, {"1": "SUBG_X32K1", "2": "GND"}, value="18pF", footprint=C0402)
s.place("C69", "Device", "C", 215.9, 152.4, {"1": "SUBG_X32K2", "2": "GND"}, value="18pF", footprint=C0402)

# VDDR 由片内 DCDC 经 L7 供电，网络无 power_out 引脚 → PWR_FLAG 声明
s.place("#FLG03", "power", "PWR_FLAG", 228.6, 152.4, {"1": "SUBG_VDDR"})

s.write()
