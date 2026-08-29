#!/usr/bin/env python3
"""生成 expansion-board-v3.kicad_sym 自定义符号库。

引脚数据逐脚来自 datasheet（见 PINMAP.md 与调研报告）：
  BNO085   : CEVA 1000-3927 v1.16 p.10 Figure 1-6
  AD8319   : ADI Rev.D Table 3 p.6（EP=pin9, 内连 COMM）
  BMP388   : Bosch BST-BMP388-DS001-07 Table 50 p.46
  QMC5883P : QST 13-52-19 RevA Table 5 p.7
  ATGM336H : 中科微用户手册 §2.3（18 脚 LCC）
  MM8930   : Murata O30E p.11（R=Ant, C=RF, G=GND）
  TA0970A  : TST Rev3.0 p.5（6 焊盘：B=IN, E=OUT, A/C/D/F=GND）

规则：所有引脚 2.54 网格、长度 2.54；NC 脚显式给 no_connect 类型（不隐藏）。
生成后必须跑 kicad-cli sym export（解析断言），任何异常都要终止而不是静默。
"""
import sys

GRID = 2.54

# (number, name, etype, side)  etype: input/output/bidirectional/power_in/passive/no_connect
SYMBOLS = {
    "BNO085": {
        "ref": "U", "fp": "expansion-board-v3:BNO085_LGA-28",
        "desc": "CEVA 9-axis IMU, SH-2, LGA-28 3.8x5.2mm",
        "pins": [
            # 左列：电源与控制
            ("3",  "VDD",            "power_in", "L"),
            ("28", "VDDIO",          "power_in", "L"),
            ("9",  "CAP",            "passive",  "L"),
            ("2",  "GND",            "power_in", "L"),
            ("25", "GND",            "power_in", "L"),
            ("11", "NRST",           "input",    "L"),
            ("4",  "BOOTN",          "input",    "L"),
            ("5",  "PS1",            "input",    "L"),
            ("6",  "PS0/WAKE",       "input",    "L"),
            ("10", "CLKSEL0",        "input",    "L"),
            ("27", "XIN32",          "input",    "L"),
            ("26", "XOUT32/CLKSEL1", "output",   "L"),
            # 右列：主机接口
            ("19", "H_SCL/SCK/RX",   "bidirectional", "R"),
            ("20", "H_SDA/MISO/TX",  "bidirectional", "R"),
            ("17", "SA0/H_MOSI",     "input",         "R"),
            ("18", "H_CSN",          "input",         "R"),
            ("14", "H_INTN",         "output",        "R"),
            ("15", "ENV_SCL",        "bidirectional", "R"),
            ("16", "ENV_SDA",        "bidirectional", "R"),
            # NC（datasheet RESV_NC，全部显式 no_connect）
            ("1",  "NC", "no_connect", "R"), ("7",  "NC", "no_connect", "R"),
            ("8",  "NC", "no_connect", "R"), ("12", "NC", "no_connect", "R"),
            ("13", "NC", "no_connect", "R"), ("21", "NC", "no_connect", "R"),
            ("22", "NC", "no_connect", "R"), ("23", "NC", "no_connect", "R"),
            ("24", "NC", "no_connect", "R"),
        ],
    },
    "AD8319": {
        "ref": "U", "fp": "expansion-board-v3:AD8319_LFCSP-8-EP",
        "desc": "ADI 1M-10GHz 45dB log detector, LFCSP-8 2x3mm, EP=COMM",
        "pins": [
            ("1", "INHI", "input",    "L"),
            ("8", "INLO", "input",    "L"),
            ("7", "VPOS", "power_in", "L"),
            ("2", "COMM", "power_in", "L"),
            ("9", "EP",   "power_in", "L"),   # 裸焊盘，内连 COMM，必须接地
            ("5", "VOUT", "output",   "R"),
            ("4", "VSET", "input",    "R"),
            ("3", "CLPF", "passive",  "R"),
            ("6", "TADJ", "passive",  "R"),
        ],
    },
    "BMP388": {
        "ref": "U", "fp": "expansion-board-v3:BMP388_LGA-10",
        "desc": "Bosch barometric pressure sensor, LGA-10 2x2mm",
        "pins": [
            ("10", "VDD",   "power_in", "L"),
            ("1",  "VDDIO", "power_in", "L"),
            ("3",  "VSS",   "power_in", "L"),
            ("8",  "VSS",   "power_in", "L"),
            ("9",  "VSS",   "power_in", "L"),
            ("2",  "SCK",   "input",         "R"),
            ("4",  "SDI",   "bidirectional", "R"),
            ("5",  "SDO",   "bidirectional", "R"),  # I2C 地址位：GND=0x76
            ("6",  "CSB",   "input",         "R"),  # 接 VDDIO=I2C 模式
            ("7",  "INT",   "output",        "R"),
        ],
    },
    "QMC5883P": {
        "ref": "U", "fp": "expansion-board-v3:QMC5883P_LGA-16",
        "desc": "QST 3-axis magnetometer, LGA-16 3x3mm, I2C addr 0x2C",
        "pins": [
            ("2",  "VDD", "power_in", "L"),
            ("9",  "GND", "power_in", "L"),
            ("11", "GND", "power_in", "L"),
            ("10", "C1",  "passive",  "L"),   # 4.7uF 储能电容
            ("1",  "SCK", "input",         "R"),
            ("16", "SDA", "bidirectional", "R"),
            ("3",  "NC", "no_connect", "R"), ("4",  "NC", "no_connect", "R"),
            ("5",  "NC", "no_connect", "R"), ("6",  "NC", "no_connect", "R"),
            ("7",  "NC", "no_connect", "R"), ("8",  "NC", "no_connect", "R"),
            ("12", "NC", "no_connect", "R"), ("13", "NC", "no_connect", "R"),
            ("14", "NC", "no_connect", "R"), ("15", "NC", "no_connect", "R"),
        ],
    },
    "ATGM336H": {
        "ref": "U", "fp": "expansion-board-v3:ATGM336H_LCC-18",
        "desc": "ZKW GNSS module ATGM336H-6N-74, LCC-18 10.1x9.7mm",
        "pins": [
            ("8",  "VCC",    "power_in", "L"),
            ("6",  "VBAT",   "power_in", "L"),
            ("14", "VCC_RF", "output",   "L"),   # 有源天线馈电输出
            ("1",  "GND",    "power_in", "L"),
            ("10", "GND",    "power_in", "L"),
            ("12", "GND",    "power_in", "L"),
            ("11", "RF_IN",  "input",    "L"),
            ("2",  "TXD0",   "output",  "R"),
            ("3",  "RXD0",   "input",   "R"),
            ("4",  "1PPS",   "output",  "R"),
            ("5",  "ON/OFF", "input",   "R"),
            ("9",  "nRESET", "input",   "R"),
            ("16", "RXD1",   "input",   "R"),
            ("17", "TXD1",   "output",  "R"),
            ("7",  "NC", "no_connect", "R"), ("13", "NC", "no_connect", "R"),
            ("15", "NC", "no_connect", "R"), ("18", "NC", "no_connect", "R"),
        ],
    },
    "MM8930-2620": {
        "ref": "TJ", "fp": "expansion-board-v3:MM8930-2620",
        "desc": "Murata SWH-2Way RF switch test connector, series in feedline",
        "pins": [
            ("R", "ANT", "passive",  "L"),   # R Terminal = 天线侧
            ("C", "RF",  "passive",  "R"),   # C Terminal = 电路侧
            ("G", "GND", "power_in", "L"),
        ],
    },
    "TA0970A": {
        "ref": "FL", "fp": "expansion-board-v3:TA0970A_SMD3838-6",
        "desc": "TST 1090MHz SAW filter, 3.8x3.8mm 6-pad, 50ohm no matching",
        "pins": [
            ("B", "IN",  "input",    "L"),
            ("A", "GND", "power_in", "L"),
            ("C", "GND", "power_in", "L"),
            ("E", "OUT", "output",   "R"),
            ("D", "GND", "power_in", "R"),
            ("F", "GND", "power_in", "R"),
        ],
    },
}


def emit_pin(num, name, etype, x, y, rot):
    x, y = round(x, 3), round(y, 3)   # 消除浮点累积残渣（3.55e-15 之类）
    return f"""\t\t\t(pin {etype} line
\t\t\t\t(at {x:g} {y:g} {rot})
\t\t\t\t(length {GRID:g})
\t\t\t\t(name "{name}" (effects (font (size 1.27 1.27))))
\t\t\t\t(number "{num}" (effects (font (size 1.27 1.27))))
\t\t\t)
"""


def emit_symbol(name, spec):
    left = [p for p in spec["pins"] if p[3] == "L"]
    right = [p for p in spec["pins"] if p[3] == "R"]
    rows = max(len(left), len(right))
    half_h = ((rows + 1) // 2 + 1) * GRID
    w = 15.24  # 半宽
    top = half_h
    out = []
    out.append(f'\t(symbol "{name}"\n')
    out.append('\t\t(pin_names (offset 1.016))\n\t\t(exclude_from_sim no)\n\t\t(in_bom yes)\n\t\t(on_board yes)\n')
    props = [
        ("Reference", spec["ref"], -w, top + 2.54, "left"),
        ("Value", name, -w, top + 5.08, "left"),
        ("Footprint", spec["fp"], 0, -top - 2.54, "left"),
        ("Datasheet", "~", 0, -top - 5.08, "left"),
        ("Description", spec["desc"], 0, -top - 7.62, "left"),
    ]
    for i, (k, v, px, py, just) in enumerate(props):
        hide = "" if k in ("Reference", "Value") else "\n\t\t\t\t(hide yes)"
        out.append(f'''\t\t(property "{k}" "{v}"
\t\t\t(at {px:g} {py:g} 0)
\t\t\t(effects (font (size 1.27 1.27)) (justify {just}){hide})
\t\t)
''')
    # 图形体
    out.append(f'\t\t(symbol "{name}_0_1"\n')
    out.append(f'\t\t\t(rectangle (start {-w + GRID:g} {top:g}) (end {w - GRID:g} {-top:g})\n')
    out.append('\t\t\t\t(stroke (width 0.254) (type default))\n\t\t\t\t(fill (type background))\n\t\t\t)\n')
    out.append('\t\t)\n')
    # 引脚
    out.append(f'\t\t(symbol "{name}_1_1"\n')
    y = top - GRID * 2
    for num, pname, etype, _ in left:
        out.append(emit_pin(num, pname, etype, -w - GRID, y, 0))
        y -= GRID
    y = top - GRID * 2
    for num, pname, etype, _ in right:
        out.append(emit_pin(num, pname, etype, w + GRID, y, 180))
        y -= GRID
    out.append('\t\t)\n')
    out.append('\t)\n')
    return "".join(out)


def main():
    parts = ['(kicad_symbol_lib\n\t(version 20231120)\n\t(generator "gen_symbols.py")\n\t(generator_version "8.0")\n']
    for name, spec in SYMBOLS.items():
        # 断言：引脚号不得重复
        nums = [p[0] for p in spec["pins"]]
        assert len(nums) == len(set(nums)), f"{name}: 引脚号重复 {nums}"
        parts.append(emit_symbol(name, spec))
    parts.append(')\n')
    out = "".join(parts)
    path = sys.argv[1]
    with open(path, "w") as f:
        f.write(out)
    # 断言：引脚总数与数据表一致
    total = sum(len(s["pins"]) for s in SYMBOLS.values())
    emitted = out.count("(pin ")
    assert emitted == total, f"引脚发射数不符: emitted={emitted} expected={total}"
    print(f"OK: {len(SYMBOLS)} symbols, {total} pins → {path}")


if __name__ == "__main__":
    main()
