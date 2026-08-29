#!/usr/bin/env python3
"""Power 页：VCC_5V → 3 路 LDO（3V3_DIG / 3V3_RF / 3V3_GNSS）。

依据 PINMAP.md §4 电源树。ME6211 引脚号：1=VIN 2=VSS 3=CE 4=NC 5=VOUT（官方符号实测）。
TPS7A20xxxDBV 展平自 LP5907（SOT-23-5 同 pinout：1=IN 2=GND 3=EN 4=NC 5=OUT）。
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from gen_sch import Sheet

C0402 = "Capacitor_SMD:C_0402_1005Metric"
C0805 = "Capacitor_SMD:C_0805_2012Metric"
SOT235 = "Package_TO_SOT_SMD:SOT-23-5"

s = Sheet("power", "Expansion Board V1 - Power: 5V in, 3x LDO")

# 主 LDO：数字域
s.place("U1", "Regulator_Linear", "ME6211C33M5", 76.2, 63.5,
        {"1": "VCC_5V", "2": "GND", "3": "VCC_5V", "4": "NC", "5": "3V3_DIG"},
        footprint=SOT235)
# RF 域低噪声 LDO
s.place("U2", "Regulator_Linear", "TPS7A20xxxDBV", 76.2, 96.52,
        {"1": "VCC_5V", "2": "GND", "3": "VCC_5V", "4": "NC", "5": "3V3_RF"},
        value="TPS7A2033PDBVR", footprint=SOT235)
# GNSS 域低噪声 LDO（含天线偏置源）
s.place("U3", "Regulator_Linear", "TPS7A20xxxDBV", 76.2, 129.54,
        {"1": "VCC_5V", "2": "GND", "3": "VCC_5V", "4": "NC", "5": "3V3_GNSS"},
        value="TPS7A2033PDBVR", footprint=SOT235)

# 去耦/储能
caps = [
    ("C1", "10uF", C0805, "VCC_5V"),
    ("C2", "1uF", C0402, "VCC_5V"),
    ("C3", "1uF", C0402, "VCC_5V"),
    ("C4", "1uF", C0402, "3V3_DIG"),
    ("C5", "1uF", C0402, "3V3_RF"),
    ("C6", "1uF", C0402, "3V3_GNSS"),
]
for i, (ref, val, fp, net) in enumerate(caps):
    s.place(ref, "Device", "C", 139.7 + (i % 3) * 25.4, 63.5 + (i // 3) * 33.02,
            {"1": net, "2": "GND"}, value=val, footprint=fp)

# 电源域驱动标记（ERC：VCC_5V 来自板外、GND 参考）
s.place("#FLG01", "power", "PWR_FLAG", 215.9, 63.5, {"1": "VCC_5V"})
s.place("#FLG02", "power", "PWR_FLAG", 215.9, 96.52, {"1": "GND"})

s.write()
