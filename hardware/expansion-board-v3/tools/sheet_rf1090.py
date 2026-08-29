#!/usr/bin/env python3
"""RF_1090 页：天线切换(板载IFA/外接SMA) → LNA①→SAW①→LNA②→SAW② → 双检波位 → Data Slicer。

拓扑依据：
- 接收链：PINMAP §5 + ADSBee 逐节点转录（R31=1k、无迟滞电阻、FAST/LEVEL 双时间常数）
- 天线切换：设计文档 §11.4。**射频开关 3 个 RF 口全部串 100pF 隔直**（手册硬要求）；
  **偏置 Tee 只挂外接 SMA 支路**——挂公共口会被板载 IFA 的接地馈电结构把 3.3V 短到地烧保险丝。
- 板载 IFA：复用现有载板拓扑（π 匹配：ANT 侧 Z_SH2 并联 → Z_SER 串联 → Z_SH1 并联）

V1 改动（相对初版）：删 MM8930 产测座（省运费+去掉射频不连续点）、删射频段测试点（1090MHz 短截线）。
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from gen_sch import Sheet

C0402 = "Capacitor_SMD:C_0402_1005Metric"
R0402 = "Resistor_SMD:R_0402_1005Metric"
L0402 = "Inductor_SMD:L_0402_1005Metric"
PRJ = "expansion-board-v3"

s = Sheet("rf1090", "Expansion Board V1 - 1090MHz: antenna switch + RX chain")

# ================= 天线①：外接 SMA（有源，带偏置 Tee）=================
s.place("J6", "Connector", "Conn_Coaxial", 38.1, 50.8, {"1": "ANT1090_EXT", "2": "GND"},
        value="U.FL_1090_EXT(经尾线转SMA)", footprint="Connector_Coaxial:U.FL_Hirose_U.FL-R-SMT-1_Vertical")
s.place("D2", "Device", "D_TVS", 55.9, 50.8, {"A1": "ANT1090_EXT", "A2": "GND"},
        value="ESD 3.3V/0.6pF", footprint="Diode_SMD:D_0402_1005Metric")
# 偏置 Tee（仅此支路！公共口绝不能挂）
s.place("Q3", "Transistor_FET", "AO3401A", 76.2, 44.45,
        {"G": "BIAS_EN_1090", "S": "3V3_GNSS", "D": "RF1090_FUSE"},
        value="AO3401A", footprint="Package_TO_SOT_SMD:SOT-23")
s.place("R18", "Device", "R", 95.25, 44.45, {"1": "3V3_GNSS", "2": "BIAS_EN_1090"},
        value="10k", footprint=R0402)
s.place("F3", "Device", "Polyfuse", 107.95, 44.45, {"1": "RF1090_FUSE", "2": "RF1090_FEED"},
        value="6V/200mA", footprint="Fuse:Fuse_0805_2012Metric")
s.place("L14", "Device", "L", 120.65, 44.45, {"1": "RF1090_FEED", "2": "ANT1090_EXT"},
        value="100nH", footprint=L0402)
s.place("C30", "Device", "C", 73.66, 50.8, {"1": "ANT1090_EXT", "2": "SW1_J2"},
        value="100pF", footprint=C0402)   # 隔直：开关 RF 口不能过直流

# ================= 天线②：板载 IFA（无源，π 匹配）=================
# 板载 IFA 本体。脚1=馈点、脚2=短路点接地——**两者在天线铜箔内部是直接导通的**，
# 这就是倒 F 的定义（也正因如此，设计文档 §11.4 警告偏置 Tee 挂公共口会把 3.3V 短到地）。
# 连接铜箔放在 footprint 内部，KiCad 才允许它连通两个不同网络的自家焊盘而不判短路。
# 几何原样复用 docs/jlc 里已调好的 1090_MHz_IFA_ANT，见 tools/gen_ifa_footprint.py。
s.place("ANT1", "Device", "Antenna_Shield", 38.1, 66.04,
        {"1": "ANT1090_IFA", "2": "GND"},
        value="IFA_1090", footprint="expansion-board-v3:ANT_IFA_1090MHz")
# π 匹配调试位。位号按**信号流向 + 串/并**自解释，摆放顺序也是天线→电台：
#     天线 ──┬── ZS1(串) ──┬── 去射频开关
#           ZP1(并)       ZP2(并)
#            GND           GND
# 与原 LCEDA 工程的对应（注意原命名的 SH1/SH2 编号方向与信号流向相反，易看错）：
#     ZP1 = Z_SH2（天线侧并联）   ZS1 = Z_SER（串联）   ZP2 = Z_SH1（电台侧并联）
# ⚠️ 待定（用户 2026-08-02）：新 4.3 PCB 到货实测确认后，这三个调试位可全部删除。
# 天线是靠几何本身调频的（原设计 Z_SER=0R、Z_SH1/Z_SH2=NC 空贴，等于直通），
# 保留它们只为万一要现场调匹配。删除前必须先拿到实测结论，别提前砍。
s.place("ZP1", "Device", "C", 48.26, 66.04, {"1": "ANT1090_IFA", "2": "GND"},
        value="DNP 并-天线侧", footprint="Capacitor_SMD:C_0603_1608Metric")
s.place("ZS1", "Device", "L", 60.96, 66.04, {"1": "ANT1090_IFA", "2": "IFA_MATCH"},
        value="0R 串", footprint="Inductor_SMD:L_0603_1608Metric")
s.place("ZP2", "Device", "C", 73.66, 66.04, {"1": "IFA_MATCH", "2": "GND"},
        value="DNP 并-电台侧", footprint="Capacitor_SMD:C_0603_1608Metric")
s.place("C53", "Device", "C", 86.36, 66.04, {"1": "IFA_MATCH", "2": "SW1_J3"},
        value="100pF", footprint=C0402)

# ── 板载 IFA 的调试口（V3.5 新增）──────────────────────────────────────
# 4.3 板上有 IFA_ANT_IPEX 挂在匹配网络之后（RF_IN），拿 NanoVNA 一插就能测；
# v3 一直没有这个口，只能在馈点上飞线，测出来的还不是完整链路。
#
# **挂在 IFA_MATCH 上**，也就是 π 网络之后、C53 之前：
#     天线 → ZP1(并) → ZS1(串) → IFA_MATCH → C53(隔直) → 开关
#                                    ↓
#                                   J7
# 这样测到的是"天线 + 完整 π 匹配"，正是要验收的东西。
#
# ⚠️ 测量时**不要焊 C53**。U.FL 是并联挂载（信号线本身照样通），若 C53 已焊，
# VNA 看到的是天线并联后级输入阻抗，读数偏低、没有意义。C53 天生就是这条链路的
# 隔离点，不必再加 0Ω。测完焊上 C53 即恢复正常工作。
#
# 未插线时 J7 是个约 0.3pF 的残桩（1090MHz 上约 -0.4dB）。原型板上拿这点损耗
# 换测试便利是划算的；量产版可以不贴。
s.place("J7", "Connector", "Conn_Coaxial", 86.36, 78.74, {"1": "IFA_MATCH", "2": "GND"},
        value="U.FL_IFA_TEST(π后调试口)",
        footprint="Connector_Coaxial:U.FL_Hirose_U.FL-R-SMT-1_Vertical")

# ================= 射频开关（XA17-G4K，与 AS179-92LF pin 兼容）=================
s.place("U16", "RF_Switch", "AS179-92LF", 111.76, 58.42, {
    "1": "SW1_J3",          # J3 = 板载 IFA
    "3": "SW1_J2",          # J2 = 外接 SMA
    "5": "SW1_J1",          # J1 = 公共口 → LNA
    "2": "GND",
    "4": "ANT_SEL_1090_V1",
    "6": "ANT_SEL_1090_V2",
}, value="XA17-G4K(或AS179-92LF)", footprint="Package_TO_SOT_SMD:SOT-363_SC-70-6")
s.place("C54", "Device", "C", 132.08, 58.42, {"1": "SW1_J1", "2": "LNA1_IN"},
        value="100pF", footprint=C0402)
# 控制线：各串 0Ω + 对地旁路，防 RF 经控制脚耦合出去
s.place("R22", "Device", "R", 111.76, 71.12, {"1": "ANT_SEL_1090_A", "2": "ANT_SEL_1090_V1"},
        value="0R", footprint=R0402)
s.place("R23", "Device", "R", 124.46, 71.12, {"1": "ANT_SEL_1090_B", "2": "ANT_SEL_1090_V2"},
        value="0R", footprint=R0402)
s.place("C55", "Device", "C", 137.16, 71.12, {"1": "ANT_SEL_1090_V1", "2": "GND"},
        value="100pF", footprint=C0402)
s.place("C56", "Device", "C", 149.86, 71.12, {"1": "ANT_SEL_1090_V2", "2": "GND"},
        value="100pF", footprint=C0402)
# 打样留一手：拆开关后用 0Ω 硬跳单路，做「有开关 vs 无开关」灵敏度对比（默认 DNP）
s.place("R24", "Device", "R", 162.56, 58.42, {"1": "SW1_J2", "2": "SW1_J1"},
        value="0R DNP(旁路外接)", footprint=R0402)
s.place("R25", "Device", "R", 175.26, 58.42, {"1": "SW1_J3", "2": "SW1_J1"},
        value="0R DNP(旁路板载)", footprint=R0402)

# ================= 接收链 =================
s.place("U11", "RF_Amplifier", "QPL9547", 63.5, 88.9, {
    "2": "LNA1_IN", "7": "LNA1_OUT", "1": "3V3_RF", "GND": "GND", "~{EN}": "GND",
}, value="QPL9547TR7", footprint="Package_DFN_QFN:DFN-8-1EP_2x2mm_P0.5mm_EP0.86x1.55mm")
s.place("C31", "Device", "C", 101.6, 76.2, {"1": "LNA1_OUT", "2": "SAW1_IN"}, value="12pF", footprint=C0402)
s.place("FL1", PRJ, "TA0970A", 127, 88.9, {"B": "SAW1_IN", "E": "SAW1_OUT",
        "A": "GND", "C": "GND", "D": "GND", "F": "GND"},
        footprint=f"{PRJ}:TA0970A_SMD3838-6")
s.place("C32", "Device", "C", 152.4, 76.2, {"1": "SAW1_OUT", "2": "LNA2_IN"}, value="12pF", footprint=C0402)
s.place("U12", "RF_Amplifier", "BGA2817", 177.8, 88.9, {
    "6": "LNA2_IN", "3": "LNA2_OUT", "1": "3V3_RF", "G": "GND",
}, value="BGA2817", footprint="Package_TO_SOT_SMD:SOT-363_SC-70-6")
s.place("C33", "Device", "C", 203.2, 76.2, {"1": "LNA2_OUT", "2": "SAW2_IN"}, value="12pF", footprint=C0402)
s.place("FL2", PRJ, "TA0970A", 228.6, 88.9, {"B": "SAW2_IN", "E": "SAW2_OUT",
        "A": "GND", "C": "GND", "D": "GND", "F": "GND"},
        footprint=f"{PRJ}:TA0970A_SMD3838-6")
s.place("C34", "Device", "C", 254, 76.2, {"1": "SAW2_OUT", "2": "DET_IN"}, value="3pF", footprint=C0402)

# ================= 双检波位（AD8319 主 / AD8313 备，二选一贴装）=================
s.place("R19", "Device", "R", 279.4, 101.6, {"1": "DET_IN", "2": "DET_INLO"}, value="52.3R", footprint=R0402)
s.place("C35", "Device", "C", 292.1, 101.6, {"1": "DET_INLO", "2": "GND"}, value="3pF", footprint=C0402)
s.place("U13", PRJ, "AD8319", 63.5, 152.4, {
    "1": "DET_IN", "8": "DET_INLO", "7": "3V3_RF", "2": "GND", "9": "GND",
    "5": "RF_DET_OUT", "4": "RF_DET_OUT",
    "3": "NC", "6": "DET_TADJ",
}, value="AD8319ACPZ-R7(主)", footprint=f"{PRJ}:AD8319_LFCSP-8-EP")
s.place("R20", "Device", "R", 101.6, 139.7, {"1": "DET_TADJ", "2": "GND"}, value="18k(1090实调)", footprint=R0402)
s.place("U14", "RF_Amplifier", "AD8313xRM", 63.5, 190.5, {
    "INHI": "DET_IN", "INLO": "DET_INLO", "VPOS": "3V3_RF",
    "PWDN": "GND", "COMM": "GND",
    "VSET": "AD8313_VOUT", "VOUT": "AD8313_VOUT",
}, value="AD8313ARMZ(备,二选一)", footprint="Package_SO:MSOP-8_3x3mm_P0.65mm")
s.place("R21", "Device", "R", 127, 152.4, {"1": "AD8313_VOUT", "2": "RF_DET_OUT"},
        value="0R DNP(选8313时贴)", footprint=R0402)
s.place("C37", "Device", "C", 101.6, 152.4, {"1": "3V3_RF", "2": "GND"}, value="100nF", footprint=C0402)
s.place("C38", "Device", "C", 114.3, 152.4, {"1": "3V3_RF", "2": "GND"}, value="100pF", footprint=C0402)

# ================= Data Slicer =================
s.place("R30", "Device", "R", 152.4, 139.7, {"1": "RF_DET_OUT", "2": "3V3_RF"}, value="1k", footprint=R0402)
s.place("R31", "Device", "R", 165.1, 139.7, {"1": "RF_DET_OUT", "2": "SLICER_FAST"}, value="1k", footprint=R0402)
s.place("C46", "Device", "C", 177.8, 139.7, {"1": "SLICER_FAST", "2": "GND"}, value="3pF", footprint=C0402)
s.place("R32", "Device", "R", 190.5, 139.7, {"1": "RF_DET_OUT", "2": "SLICER_LEVEL"}, value="10k", footprint=R0402)
s.place("C47", "Device", "C", 203.2, 139.7, {"1": "SLICER_LEVEL", "2": "GND"}, value="200pF", footprint=C0402)
s.place("R33", "Device", "R", 215.9, 139.7, {"1": "LEVEL_BIAS", "2": "SLICER_LEVEL"}, value="100k", footprint=R0402)
s.place("R34", "Device", "R", 228.6, 139.7, {"1": "TL_PWM", "2": "LEVEL_BIAS"}, value="10k", footprint=R0402)
s.place("C49", "Device", "C", 241.3, 139.7, {"1": "LEVEL_BIAS", "2": "GND"}, value="100nF", footprint=C0402)
s.place("R35", "Device", "R", 254, 139.7, {"1": "RF_DET_OUT", "2": "RSSI"}, value="10k", footprint=R0402)
s.place("C51", "Device", "C", 266.7, 139.7, {"1": "RSSI", "2": "GND"}, value="1nF", footprint=C0402)
s.place("U15", "Comparator", "TLV3501AIDBV", 152.4, 177.8, {
    "1": "SLICER_LEVEL", "3": "SLICER_FAST",
    "2": "GND", "4": "3V3_RF", "5": "PULSES", "6": "3V3_RF",
}, value="TLV3501(TOKMAS)", footprint="Package_TO_SOT_SMD:SOT-23-6")
s.place("R36", "Device", "R", 190.5, 177.8, {"1": "PULSES", "2": "3V3_DIG"}, value="1k", footprint=R0402)
s.place("C52", "Device", "C", 203.2, 177.8, {"1": "3V3_RF", "2": "GND"}, value="100nF", footprint=C0402)

s.write()
