#!/usr/bin/env python3
"""Sensors_GNSS 页：BNO085 + BMP388 + QMC5883P（I2C 直连 P4）+ ATGM336H-6N-74。

接法依据：
- BNO085: DS 1000-3927 v1.16 — PS1/PS0=00→I2C；SA0=GND→0x4A；CAP=100nF；
  CLKSEL0=1+CLKSEL1(内部下拉)=0→内部振荡器；BOOTN 10k 上拉
- BMP388: DS001-07 — CSB=VDDIO→I2C 锁定；SDO=GND→0x76（与 baro_task.c:29 一致）
- QMC5883P: 13-52-19 RevA — C1 脚 4.7µF 储能；SET/RST 电容不需要（内置驱动）
- ATGM336H: 6N 手册 — ON/OFF 拉高常开；nRESET 可悬空；RF_IN 由板上偏置支路供电
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from gen_sch import Sheet

C0402 = "Capacitor_SMD:C_0402_1005Metric"
R0402 = "Resistor_SMD:R_0402_1005Metric"
L0402 = "Inductor_SMD:L_0402_1005Metric"
PRJ = "expansion-board-v3"

s = Sheet("sensors", "Expansion Board V1 - Sensors + GNSS")

# ---- BNO085 ----
s.place("U4", PRJ, "BNO085", 76.2, 76.2, {
    "3": "3V3_DIG", "28": "3V3_DIG", "9": "BNO_CAP",
    "2": "GND", "25": "GND",
    "11": "IMU_RST", "4": "BNO_BOOTN",
    "5": "GND", "6": "GND",                 # PS1/PS0 = 00 → I2C
    "10": "3V3_DIG",                          # CLKSEL0=1 → 内部振荡器
    "26": "NC", "27": "NC",                   # CLKSEL1 内部下拉=0；XIN32 不用
    "15": "BNO_ENV_SCL", "16": "BNO_ENV_SDA",
    "19": "I2C_SCL", "20": "I2C_SDA",
    "17": "GND",                               # SA0=0 → 0x4A
    "18": "3V3_DIG",                           # H_CSN 拉高（I2C 模式不用）
    "14": "IMU_INT",
    "1": "NC", "7": "NC", "8": "NC", "12": "NC", "13": "NC",
    "21": "NC", "22": "NC", "23": "NC", "24": "NC",
}, footprint=f"{PRJ}:BNO085_LGA-28")
s.place("R52", "Device", "R", 114.3, 76.2,
        {"1": "3V3_DIG", "2": "BNO_ENV_SCL"}, value="10k", footprint=R0402)
s.place("R53", "Device", "R", 127.0, 76.2,
        {"1": "3V3_DIG", "2": "BNO_ENV_SDA"}, value="10k", footprint=R0402)
s.place("C10", "Device", "C", 127, 50.8, {"1": "3V3_DIG", "2": "GND"}, value="100nF", footprint=C0402)
s.place("C11", "Device", "C", 139.7, 50.8, {"1": "3V3_DIG", "2": "GND"}, value="100nF", footprint=C0402)
s.place("C12", "Device", "C", 152.4, 50.8, {"1": "BNO_CAP", "2": "GND"}, value="100nF", footprint=C0402)
s.place("R1", "Device", "R", 165.1, 50.8, {"1": "3V3_DIG", "2": "BNO_BOOTN"}, value="10k", footprint=R0402)
# I2C 总线上拉（本板是唯一上拉点）
s.place("R2", "Device", "R", 177.8, 50.8, {"1": "3V3_DIG", "2": "I2C_SDA"}, value="4.7k", footprint=R0402)
s.place("R3", "Device", "R", 190.5, 50.8, {"1": "3V3_DIG", "2": "I2C_SCL"}, value="4.7k", footprint=R0402)

# ---- BMP388 ----
s.place("U5", PRJ, "BMP388", 76.2, 137.16, {
    "10": "3V3_DIG", "1": "3V3_DIG",
    "3": "GND", "8": "GND", "9": "GND",
    "2": "I2C_SCL", "4": "I2C_SDA",
    "5": "GND",            # SDO=0 → I2C 0x76
    "6": "3V3_DIG",        # CSB=VDDIO → 锁定 I2C
    "7": "BARO_INT",
}, footprint=f"{PRJ}:BMP388_LGA-10")
s.place("C13", "Device", "C", 127, 71.12, {"1": "3V3_DIG", "2": "GND"}, value="100nF", footprint=C0402)
s.place("C14", "Device", "C", 139.7, 71.12, {"1": "3V3_DIG", "2": "GND"}, value="100nF", footprint=C0402)

# ---- QMC5883P ----
s.place("U6", PRJ, "QMC5883P", 76.2, 185.42, {
    "2": "3V3_DIG", "9": "GND", "11": "GND",
    "10": "QMC_C1",
    "1": "I2C_SCL", "16": "I2C_SDA",
    "3": "NC", "4": "NC", "5": "NC", "6": "NC", "7": "NC", "8": "NC",
    "12": "NC", "13": "NC", "14": "NC", "15": "NC",
}, footprint="Package_LGA:LGA-16_3x3mm_P0.5mm")
s.place("C15", "Device", "C", 152.4, 71.12, {"1": "QMC_C1", "2": "GND"}, value="4.7uF", footprint="Capacitor_SMD:C_0603_1608Metric")
s.place("C16", "Device", "C", 165.1, 71.12, {"1": "3V3_DIG", "2": "GND"}, value="100nF", footprint=C0402)

# ---- ATGM336H-6N-74 ----
s.place("U7", PRJ, "ATGM336H", 190.5, 137.16, {
    "8": "3V3_GNSS", "6": "3V3_GNSS",
    "14": "NC", "11": "GNSS_RF_IN",
    "1": "GND", "10": "GND", "12": "GND",
    "2": "GNSS_TXD", "3": "GNSS_RXD", "4": "GNSS_PPS",
    "5": "3V3_GNSS",       # ON/OFF 拉高常开
    "9": "NC",             # nRESET 内部 POR，可悬空（手册）
    "16": "NC", "17": "NC", "7": "NC", "13": "NC", "15": "NC", "18": "NC",
}, value="ATGM336H-6N-74", footprint=f"{PRJ}:ATGM336H_LCC-18")
# ---- GNSS 天线切换：内置 patch(外壳顶边凸包) ↔ 外接 SMA，两路都是有源天线 ----
# 开关 RF 口不能过直流 → 每支路各一个偏置 Tee，且都在隔直电容外侧。
# AS179/XA17 真值表：V1低/V2高选J2，V1高/V2低选J3。
# 高边 PMOS 低有效，因此外接 J2 的 Q4 栅极接 V1/A，内置 J3 的 Q5 栅极接 V2/B。
s.place("C57", "Device", "C", 215.9, 71.12, {"1": "GNSS_RF_IN", "2": "SW2_J1"},
        value="100pF", footprint=C0402)
s.place("U17", "RF_Switch", "AS179-92LF", 241.3, 88.9, {
    "1": "SW2_J3",      # 内置 patch
    "3": "SW2_J2",      # 外接 SMA
    "5": "SW2_J1",      # 公共口 → GNSS 模块
    "2": "GND",
    "4": "ANT_SEL_GNSS_A", "6": "ANT_SEL_GNSS_B",
}, value="XA17-G4K", footprint="Package_TO_SOT_SMD:SOT-363_SC-70-6")
# 外接支路
s.place("C58", "Device", "C", 266.7, 71.12, {"1": "ANT_GNSS_EXT", "2": "SW2_J2"},
        value="100pF", footprint=C0402)
s.place("Q4", "Transistor_FET", "AO3401A", 279.4, 88.9,
        {"G": "ANT_SEL_GNSS_A", "S": "3V3_GNSS", "D": "GNSS_EXT_FUSE"},
        value="AO3401A", footprint="Package_TO_SOT_SMD:SOT-23")
s.place("F4", "Device", "Polyfuse", 292.1, 71.12, {"1": "GNSS_EXT_FUSE", "2": "GNSS_EXT_FEED"},
        value="6V/200mA", footprint="Fuse:Fuse_0805_2012Metric")
s.place("L2", "Device", "L", 304.8, 71.12, {"1": "GNSS_EXT_FEED", "2": "ANT_GNSS_EXT"},
        value="33nH", footprint=L0402)
# 内置支路
s.place("C59", "Device", "C", 266.7, 96.52, {"1": "ANT_GNSS_INT", "2": "SW2_J3"},
        value="100pF", footprint=C0402)
s.place("Q5", "Transistor_FET", "AO3401A", 279.4, 114.3,
        {"G": "ANT_SEL_GNSS_B", "S": "3V3_GNSS", "D": "GNSS_INT_FUSE"},
        value="AO3401A", footprint="Package_TO_SOT_SMD:SOT-23")
s.place("F5", "Device", "Polyfuse", 292.1, 96.52, {"1": "GNSS_INT_FUSE", "2": "GNSS_INT_FEED"},
        value="6V/200mA", footprint="Fuse:Fuse_0805_2012Metric")
s.place("L15", "Device", "L", 304.8, 96.52, {"1": "GNSS_INT_FEED", "2": "ANT_GNSS_INT"},
        value="33nH", footprint=L0402)
s.place("J8", "Connector", "Conn_Coaxial", 330.2, 96.52, {"1": "ANT_GNSS_INT", "2": "GND"},
        value="U.FL→内置patch", footprint="Connector_Coaxial:U.FL_Hirose_U.FL-R-SMT-1_Vertical")
# 上电默认两路 PMOS 都关断（栅极上拉），由固件选通
# 复位上拉。IMU_RST 只挂 U4.11 和 J1.16——后者经排针直连 ESP32-P4 的 GPIO，
# 主控复位期间是高阻，BNO085 会被随机拉复位。上拉到驱动侧的 3V3_DIG（低有效复位）。
s.place("R55", "Device", "R", 203.2, 50.8, {"1": "3V3_DIG", "2": "IMU_RST"},
        value="10k", footprint=R0402)
s.place("R26", "Device", "R", 215.9, 114.3, {"1": "3V3_DIG", "2": "ANT_SEL_GNSS_A"},
        value="10k", footprint=R0402)
s.place("R27", "Device", "R", 228.6, 114.3, {"1": "3V3_DIG", "2": "ANT_SEL_GNSS_B"},
        value="10k", footprint=R0402)
s.place("C17", "Device", "C", 228.6, 71.12, {"1": "3V3_GNSS", "2": "GND"}, value="100nF", footprint=C0402)
s.place("C18", "Device", "C", 241.3, 71.12, {"1": "3V3_GNSS", "2": "GND"}, value="10uF", footprint="Capacitor_SMD:C_0805_2012Metric")
s.place("J2", "Connector", "Conn_Coaxial", 330.2, 71.12, {"1": "ANT_GNSS_EXT", "2": "GND"},
        value="U.FL_GNSS_EXT(经尾线转SMA)", footprint="Connector_Coaxial:U.FL_Hirose_U.FL-R-SMT-1_Vertical")

s.write()
