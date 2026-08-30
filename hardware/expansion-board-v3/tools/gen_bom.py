#!/usr/bin/env python3
"""从网表生成唯一的采购清单 BOM_PURCHASE.md。

设计意图：此前的采购信息散落在多个文档章节、且有互相推翻的结论，用户无法核对。
本脚本从 netlist 自动导出**完整**清单，任何一行都不会漏；采购等级与料号在下方
PARTS 表里集中维护，改一处即可全表重生成。

采购等级：
  MUST  🔴 必须买指定料号（射频关键，替代会失效）
  SPEC  🟡 有具体推荐，但同规格可替代
  ANY   🟢 通用件，按价格挑 / 用现有元件本
  HAVE  ✅ 用户已有

用法: gen_bom.py   （读 build/netlist-docs.xml，写 internal/BOM_PURCHASE.md）
断言：网表里每个 (值,封装) 组合都必须在 PARTS 表里有归类，否则报错——防止漏项。
"""
import os
import subprocess
import xml.etree.ElementTree as ET
from collections import defaultdict

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NET = os.path.join(T, "build", "netlist-docs.xml")
# v3 已归档：采购清单属内部文档 → 落 internal/
OUT = os.path.join(T, "internal", "BOM_PURCHASE.md")

schematic = os.path.join(T, "kicad", "expansion-board-v3.kicad_sch")
newest_schematic = max(
    os.path.getmtime(path)
    for path in __import__("glob").glob(os.path.join(T, "kicad", "*.kicad_sch"))
)
if not os.path.exists(NET) or os.path.getmtime(NET) < newest_schematic:
    os.makedirs(os.path.dirname(NET), exist_ok=True)
    cli = os.path.expanduser("~/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli")
    subprocess.run([cli, "sch", "export", "netlist", "--format", "kicadxml",
                    "-o", NET, schematic], check=True, capture_output=True)

# key = (value, footprint后缀) → (类别, 采购等级, 推荐型号, 立创料号, 备注)
P = {
    # ============ 射频关键件（必须指定料号）============
    ("100nH", "L_0402_1005Metric"): ("射频-偏置扼流", "MUST", "DDY WI0402IFR10KST-HF（绕线）", "C18221115",
        "⚠️SRF 1.4GHz。常见的叠层款 SRF 仅 600-700MHz，在 1090MHz 已变电容会把射频短到电源"),
    ("18nH 0402CS-18NXGRW", "L_0402_1005Metric"): ("射频-LNA馈电扼流", "MUST",
        "Coilcraft 0402CS-18NXGRW 或同等高 SRF 绕线 18nH", "—",
        "QPL9547 pin7 VDD 馈电；替代料必须核对 1090MHz 时仍为感性"),
    ("33nH", "L_0402_1005Metric"): ("射频-偏置扼流", "MUST", "APV AHW1005C-33NJTF（绕线）", "C6807986",
        "⚠️SRF 2.35GHz。GNSS 1575MHz 要求 SRF≥2GHz，叠层款不合格"),
    ("ESD 3.3V/0.6pF", "D_0402_1005Metric"): ("射频-天线口ESD", "MUST", "TECH PUBLIC TPESD8L3.3CT5G", "C2830293",
        "⚠️结电容 Cj 0.3pF typ/0.5pF max。普通 5V ESD 管 Cj 20-50pF 直接吃掉射频信号"),
    ("7.5nH", "L_0402_1005Metric"): ("射频-978匹配", "MUST", "muRata LQW15AN7N5G00D（绕线高Q）", "C82918",
        "国产替代 Sunltech SCW1005C7N5JST C330848"),
    ("6.8nH", "L_0402_1005Metric"): ("射频-978匹配", "MUST", "muRata LQW15AN6N8G00D（绕线高Q）", "C82919",
        "国产替代 Sunltech SCW1005C6N8JST C330847"),
    ("27nH", "L_0402_1005Metric"): ("射频-978匹配", "MUST", "muRata LQW15AN27NG0ZD（绕线 Q=25）", "C7519921",
        "国产替代 APV AHW1005C-27NJTF C6364690"),
    ("3.6pF", "C_0402_1005Metric"): ("射频-978匹配", "MUST", "YAGEO AC0402CRNPO9BN3R6（C0G）", "C5552081",
        "⚠️必须 C0G/NP0，X7R 调不准匹配"),
    ("2.7pF", "C_0402_1005Metric"): ("射频-978匹配", "MUST", "风华 0402CG2R7C500NT（C0G）", "C1561", "⚠️必须 C0G"),
    ("6.2pF", "C_0402_1005Metric"): ("射频-978匹配", "MUST", "风华 0402CG6R2C500NT（C0G）", "C41744", "⚠️必须 C0G"),
    ("12pF", "C_0402_1005Metric"): ("射频-级间耦合", "MUST", "三环 TCC0402COG120J500AT（C0G）", "C696888", "⚠️必须 C0G"),
    ("3pF", "C_0402_1005Metric"): ("射频-检波耦合", "MUST", "三环 TCC0402COG3R0C500AT（C0G）", "C696883", "⚠️必须 C0G"),
    ("52.3R", "R_0402_1005Metric"): ("射频-端接", "MUST", "YAGEO RC0402FR-0752R3L ±1%", "C273696",
        "必须 ±1%，不可用 51R±5% 代"),
    # ============ 主动器件 ============
    ("XA17-G4K(或AS179-92LF)", "SOT-363_SC-70-6"): ("射频开关", "SPEC", "信路达 XA17-G4K", "C513494",
        "pin 兼容 AS179-92LF(C83422)，一个封装双源。SOT-363 很小，建议多备 3 颗"),
    ("AO3401A", "SOT-23"): ("PMOS 高边开关", "SPEC", "UMW(友台) AO3401A", "C347476",
        "立创无 AOS 原厂『AO3401』(不带A)。原厂正片 AOS C15127。避坑：ElecSuper C5224202 是 SOT-23-3L 非标准；GOODWORK C2938368 描述误写 N-Channel"),
    ("6V/200mA", "Fuse_0805_2012Metric"): ("自恢复保险丝", "SPEC", "金瑞 JK-SMD0805-020-30V", "C516070",
        "Ih=200mA，30V 耐压，$0.031，库存 60 万"),
    ("B5819W", "D_SOD-123"): ("肖特基防倒灌", "ANY", "JSMSEMI 1N5819W", "C963381",
        "VBUS<200mA 够用；真跑 1A 才值得换低 Vf 的 DIODES C82544"),
    ("6.8uH", "L_0805_2012Metric"): ("CC1312R DCDC", "SPEC", "TDK MLZ2012N6R8LT000", "C82157",
        "⚠️全站 0805 里只有它 DCR<300mΩ 且 Isat≥100mA（250mΩ/110mA，刚压线）"),
    # ============ 时钟 ============
    ("12MHz", "Crystal_SMD_3225-4Pin_3.2x2.5mm"): ("晶振", "SPEC", "YXC X322512MSB4SI", "C9002", "RP2040 主晶振"),
    ("48MHz ABM8W-7pF", "Crystal_SMD_3225-4Pin_3.2x2.5mm"): ("晶振", "SPEC", "Abracon ABM8W-48.0000MHZ-7-D1X-T3", "C6732653",
        "CC1312R 主晶振，7pF 负载"),
    ("32.768kHz FC-135", "Crystal_SMD_3215-2Pin_3.2x1.5mm"): ("晶振", "SPEC", "EPSON Q13FC13500004", "C32346", ""),
    # ============ 普通被动件（可用现有元件本）============
    # ---- 板载天线：铜箔本身，无需采购 ----
    ("IFA_1090", "ANT_IFA_1090MHz"): ("板载1090天线", "HAVE", "PCB 铜箔（无器件）", "—",
        "当前工程为旧占位几何，不是已调好天线；2026-08-24预研见 IFA_HFSS_2026-08-24.md；不占BOM"),
    # ---- 0603：非射频位置（不接触 RF_NETS）改用用户手上的 0603 电阻/电容本 ----
    # 判据见 gen_sch.py 的 RF_NETS / resolve_fp()。这些位置全部走元件本，不用采购。
    ("100nF", "C_0603_1608Metric"): ("去耦", "HAVE", "0603 电容本 100nF X7R", "—", "用现有元件本"),
    ("1uF", "C_0603_1608Metric"): ("去耦", "HAVE", "0603 电容本 1uF X5R", "—", "用现有元件本"),
    ("100pF", "C_0603_1608Metric"): ("旁路", "HAVE", "0603 电容本 100pF", "—", "用现有元件本"),
    ("1nF", "C_0603_1608Metric"): ("滤波", "HAVE", "0603 电容本 1nF", "—", "用现有元件本"),
    ("3pF", "C_0603_1608Metric"): ("滤波", "HAVE", "0603 电容本 3pF", "—", "用现有元件本"),
    ("12pF", "C_0603_1608Metric"): ("晶振负载", "HAVE", "0603 电容本 12pF", "—",
        "⚠️晶振负载电容建议 C0G；本子若是 X7R，频偏可接受但请实测"),
    ("15pF", "C_0603_1608Metric"): ("晶振负载", "HAVE", "0603 电容本 15pF", "—",
        "⚠️同上：建议 C0G，本子货请实测频偏"),
    ("200pF", "C_0603_1608Metric"): ("滤波", "HAVE", "0603 电容本 200pF", "—", "用现有元件本"),
    ("0R", "R_0603_1608Metric"): ("跳线", "HAVE", "0603 电阻本 0R", "—", "用现有元件本"),
    ("0R DNP(选8313时贴)", "R_0603_1608Metric"): ("选配跳线", "HAVE", "0603 电阻本 0R", "—",
        "默认不贴；选 AD8313 方案时才贴"),
    ("27R", "R_0603_1608Metric"): ("USB 串阻", "HAVE", "0603 电阻本 27R", "—", "用现有元件本"),
    ("1k", "R_0603_1608Metric"): ("限流/偏置", "HAVE", "0603 电阻本 1k", "—", "用现有元件本"),
    ("4.7k", "R_0603_1608Metric"): ("上拉", "HAVE", "0603 电阻本 4.7k", "—", "用现有元件本"),
    ("5.1k", "R_0603_1608Metric"): ("USB-C CC 下拉", "HAVE", "0603 电阻本 5.1k", "—",
        "两颗都要，缺一台机不认 USB"),
    ("10k", "R_0603_1608Metric"): ("上/下拉", "HAVE", "0603 电阻本 10k", "—", "用现有元件本"),
    ("100k", "R_0603_1608Metric"): ("分压/下拉", "HAVE", "0603 电阻本 100k", "—", "用现有元件本"),
    ("18k(1090实调)", "R_0603_1608Metric"): ("门限分压", "HAVE", "0603 电阻本 18k", "—",
        "⚠️需按实测灵敏度调值，多备几档（10k/15k/18k/22k/33k）"),
    ("100pF", "C_0402_1005Metric"): ("隔直/旁路", "ANY", "任意 100pF（X7R 即可）", "—",
        "✅实算：X7R vs C0G 全路径只差 0.13dB，不值得单买 NP0。可用现有元件本"),
    ("100nF", "C_0402_1005Metric"): ("去耦", "ANY", "任意 100nF X7R", "—", "用元件本"),
    ("1uF", "C_0402_1005Metric"): ("去耦", "ANY", "任意 1uF X5R", "—", "用元件本"),
    ("15pF", "C_0402_1005Metric"): ("晶振负载", "SPEC", "风华 0402CG150J500NT（C0G）", "C1548", "晶振负载建议 C0G，温漂影响频率"),
    ("1nF", "C_0402_1005Metric"): ("滤波", "ANY", "任意 1nF", "—", "用元件本"),
    ("200pF", "C_0402_1005Metric"): ("滤波", "ANY", "任意 200pF（C0G 优先）", "C5448830",
        "⚠️0402 C0G 200pF 全站库存都<1万；用元件本或改 220pF"),
    ("10uF", "C_0805_2012Metric"): ("储能", "ANY", "任意 10uF X5R 0805", "—", "用元件本"),
    ("22uF", "C_0805_2012Metric"): ("储能", "ANY", "任意 22uF X5R 0805", "—", "用元件本"),
    ("4.7uF", "C_0603_1608Metric"): ("磁力计储能", "SPEC", "Samsung CL10A475KO8NNNC", "C19666", "低 ESR，QMC5883P 要求"),
    # 电阻
    ("10k", "R_0402_1005Metric"): ("电阻", "ANY", "任意 10k ±1%", "—", "用元件本"),
    ("1k", "R_0402_1005Metric"): ("电阻", "ANY", "任意 1k ±1%", "—", "用元件本"),
    ("0R", "R_0402_1005Metric"): ("跳线", "ANY", "任意 0R", "—", "用元件本"),
    ("27R", "R_0402_1005Metric"): ("USB 串阻", "ANY", "任意 27R ±1%", "—", "用元件本"),
    ("4.7k", "R_0402_1005Metric"): ("I2C 上拉", "ANY", "任意 4.7k", "—", "用元件本"),
    ("5.1k", "R_0402_1005Metric"): ("USB CC 下拉", "ANY", "任意 5.1k ±1%", "—", "用元件本"),
    ("100k", "R_0402_1005Metric"): ("电阻", "ANY", "任意 100k", "—", "用元件本"),
    ("3.32k", "R_0402_1005Metric"): ("射频-LNA偏置", "MUST", "0402 3.32k ±1%", "—",
        "QPL9547 pin1 Vbias；按原厂评估板值，禁止用 0Ω 直连 3V3_RF"),
    ("18k(1090实调)", "R_0402_1005Metric"): ("AD8319 温补", "ANY", "任意 18k", "—", "1090MHz 实调，备 8k/12k/18k"),
    # ============ IFA 匹配调试位（0603，配合用户元件本手调）============
    ("DNP 并-天线侧", "C_0603_1608Metric"): ("IFA 匹配-并联", "ANY", "调试用，值上板实测确定", "—",
        "0603便于换值；备C0G 0.5-12pF。默认DNP，HFSS首轮也保持DNP"),
    ("DNP 并-电台侧", "C_0603_1608Metric"): ("IFA 匹配-并联", "ANY", "调试用，值上板实测确定", "—",
        "默认DNP；按完整六层rev2 HFSS目标处约20.97Ω结果，首轮可从3.3pF起扫，并备3.6/3.9pF，最终以装盒VNA为准"),
    ("0R 串", "L_0603_1608Metric"): ("IFA 匹配-串联", "ANY", "默认0R直通；调试时换值", "—",
        "备1-30nH；HFSS首轮可从3.6nH起扫。⚠️量产件Q变化后必须复测"),
    # ============ DNP 实验跳线（默认不贴，PCB 留位）============
    ("0R DNP(旁路外接)", "R_0402_1005Metric"): ("实验跳线", "ANY", "0R（默认不贴）", "—",
        "拆开关后硬跳外接支路，做「有开关 vs 无开关」灵敏度对比"),
    ("0R DNP(旁路板载)", "R_0402_1005Metric"): ("实验跳线", "ANY", "0R（默认不贴）", "—", "同上，硬跳板载 IFA"),
    ("0R DNP(选8313时贴)", "R_0402_1005Metric"): ("实验跳线", "ANY", "0R（默认不贴）", "—",
        "双检波位二选一：贴 AD8313 时才焊此跳阻"),
    # ============ 连接器 / 结构 ============
    ("J3_HAT_2x20_SMD排针", "PinHeader_2x20_P2.54mm_Vertical_SMD"): ("对插排针", "HAVE", "2×20 2.54mm 贴片排针", "—",
        "✅用户已有（实测 51mm 长 / 贴片脚 2.85 / 配合板间距 7.0）"),
    ("USB-C_16P", "USB_C_Receptacle_HRO_TYPE-C-31-M-12"): ("USB-C", "SPEC", "HRO TYPE-C-31-M-12", "—", "RP2040 刷机口"),
    ("IFA_FEED", "TestPoint_Pad_2.0x2.0mm"): ("IFA 馈点", "HAVE", "PCB 焊盘，无器件", "—", "板载走线，不采购"),
    ("RESET短接焊盘", "SolderJumper-2_P1.3mm_Open_Pad1.0x1.5mm"): ("短接焊盘", "HAVE", "PCB 焊盘，无器件", "—", "镊子短接，不采购"),
    ("BOOTSEL短接焊盘", "SolderJumper-2_P1.3mm_Open_Pad1.0x1.5mm"): ("短接焊盘", "HAVE", "PCB 焊盘，无器件", "—", "同上"),
    ("TestPoint", "TestPoint_Pad_1.0x1.0mm"): ("测试点", "HAVE", "PCB 焊盘，无器件", "—", "不采购"),
}
# 主芯片（§7 已下单，此处仅列出以保证清单完整）
ORDERED = {
    "CC1312R1F3RGZR", "RP2040", "W25Q128JVSIQ", "QPL9547TR7", "BGA2817", "TA0970A",
    "AD8319ACPZ-R7(主)", "AD8313ARMZ(备,二选一)", "TLV3501(TOKMAS)", "BNO085", "BMP388",
    "QMC5883P", "ATGM336H-6N-74", "ME6211C33M5", "TPS7A2033PDBVR",
}
UFL = "U.FL_Hirose_U.FL-R-SMT-1_Vertical"

tree = ET.parse(NET)
groups = defaultdict(list)
for c in tree.iter("comp"):
    v = c.find("value"); f = c.find("footprint")
    val = v.text if v is not None else "?"
    fp = (f.text if f is not None else "?").split(":")[-1]
    groups[(val, fp)].append(c.get("ref"))

rows, missing = [], []
for (val, fp), refs in groups.items():
    if fp == UFL:
        info = ("U.FL 座", "SPEC", "广濑 U.FL-R-SMT-1(80)", "C88374",
                "4 个：1090外接/978/GNSS外接/GNSS内置patch。外接的两个另配 U.FL→SMA 尾线")
    elif val in ORDERED or val.split("(")[0] in ORDERED:
        info = ("主芯片", "HAVE", val, "—", "§7 清单已下单")
    elif (val, fp) in P:
        info = P[(val, fp)]
    else:
        missing.append((val, fp)); continue
    rows.append((info[0], info[1], val, fp, len(refs), sorted(refs), info[2], info[3], info[4]))

assert not missing, f"以下 (值,封装) 未在 PARTS 表归类，清单会漏项：{missing}"

ORDER = {"MUST": 0, "SPEC": 1, "ANY": 2, "HAVE": 3}
ICON = {"MUST": "🔴必买指定", "SPEC": "🟡有推荐", "ANY": "🟢通用/元件本", "HAVE": "✅已有"}
rows.sort(key=lambda r: (ORDER[r[1]], r[0], r[2]))

out = ["# 采购清单（唯一权威版，由 tools/gen_bom.py 从网表自动导出）\n",
       "> ⚠️ **本文件取代此前散落在设计文档 §7/§12/§13 的所有采购表格。**\n",
       "> 网表变更后重跑 `gen_bom.py` 即可；脚本带断言，任何未归类的元件都会报错，不会漏项。\n",
       "> **IFA**：默认0R/DNP只是直通，不是已匹配；3.6nH/3.3pF仅为完整六层rev2 HFSS首轮扫值起点。\n",
       f"\n共 {sum(r[4] for r in rows)} 个元件 / {len(rows)} 个料号。\n",
       "\n| 采购 | 类别 | 值/规格 | 封装 | 数量 | 位号 | 推荐型号 | 立创料号 | 说明 |",
       "|---|---|---|---|---|---|---|---|---|"]
for cat, lvl, val, fp, n, refs, part, lcsc, note in rows:
    r = ",".join(refs) if len(refs) <= 6 else ",".join(refs[:6]) + f"…({n})"
    out.append(f"| {ICON[lvl]} | {cat} | {val} | {fp} | {n} | {r} | {part} | {lcsc} | {note} |")
open(OUT, "w").write("\n".join(out) + "\n")
print(f"OK: {sum(r[4] for r in rows)} 元件 / {len(rows)} 料号 → {OUT}")
for lvl in ("MUST", "SPEC", "ANY", "HAVE"):
    print(f"  {ICON[lvl]}: {sum(1 for r in rows if r[1]==lvl)} 个料号")
