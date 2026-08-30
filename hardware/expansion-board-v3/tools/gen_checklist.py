#!/usr/bin/env python3
"""生成按位号排序的 BOM 核对清单。

跟另外三份表的分工：

    BOM_PURCHASE.md      唯一权威源，按采购等级分组，给自己看
    BOM_采购清单.xlsx     给淘宝卖家的购物清单，合并同料、去掉位号
    BOM_嘉立创SMT.xlsx    给贴片机的，按 (值,封装) 分组、位号全展开
    CHECKLIST.md         ← 本文件。**按位号顺序**，给人拿着板子逐个核对

为什么单独出一份：前三份都是按「料」组织的，而人手工贴片时是按「位置」走的
——手里拿着板子，挨个位号找该贴什么。按料分组的表要来回翻，按位号排的表一路
往下扫就行。

## 排序

位号按 `字母前缀 + 数字` 排，C1 < C2 < C10（不是字典序的 C1 < C10 < C2）。
前缀之间按常规器件顺序（C/R/L 在前，U/Y 在后），而不是字母序——手工贴片
一般先贴 IC 再贴阻容，但核对时按类型聚在一起更好找。

## 标注

每一行会带上：贴片阶段、是否 DNP、有无极性、以及 V3.2 实物板的丝印错位警告。
这些信息散在 ASSEMBLY.md 各处，核对时来回翻很痛苦，这里一次性摊平。

用法：**KiCad 的 python3**（要读 .kicad_pcb） tools/gen_checklist.py
"""
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from board_meta import BOARD_REV, PCB_BASENAME          # noqa: E402
from gen_assembly import AS_BUILT_SILK_SWAPS, STAGES     # noqa: E402

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PCB = os.path.join(T, "kicad", f"{PCB_BASENAME}.kicad_pcb")
SCH = os.path.join(T, "kicad", f"{PCB_BASENAME}.kicad_sch")
NET = os.path.join(T, "build", "netlist-docs.xml")
# v3 已归档：根目录的 CHECKLIST.md 是冻结的公开快照，再生成产物落 internal/
OUT = os.path.join(T, "internal", "CHECKLIST.md")

newest_schematic = max(
    os.path.getmtime(path)
    for path in __import__("glob").glob(os.path.join(T, "kicad", "*.kicad_sch"))
)
if not os.path.exists(NET) or os.path.getmtime(NET) < newest_schematic:
    os.makedirs(os.path.dirname(NET), exist_ok=True)
    CLI = os.path.expanduser("~/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli")
    subprocess.run([CLI, "sch", "export", "netlist", "--format", "kicadxml",
                    "-o", NET, SCH], check=True, capture_output=True)

# 位号前缀的排列顺序。核对时同类聚在一起最好找，所以不用字母序。
PREFIX_ORDER = ["C", "R", "L", "D", "F", "Q", "U", "FL", "Y", "J",
                "ANT", "ZP", "ZS", "SW", "TP", "H"]

def polarity_note(prefix, fp):
    """有极性 / 有方向的器件。两脚的阻容感物理对称、随便贴，这些贴反就废了。

    ⚠️ **不能只看位号前缀**——同一个前缀底下可能混着不同封装：

        Y1/Y2  Crystal_SMD_3225-4Pin   pad2/pad4 接 GND，有方向
        Y3     Crystal_SMD_3215-2Pin   两脚都是信号，对称，**没有极性**

    第一版按前缀 "Y" 一刀切标成「认 pin1（4 脚晶振）」，把只有两个脚的 Y3 也
    标上了，是错的。凡是同前缀可能混封装的，一律下钻到封装再判。

    D2/D3 是这里最要紧的一条：0402 封装的 ESD 管，混在 22 个 0402 电容和
    9 个 0402 电感中间，外观几乎一样，但有极性。
    """
    if prefix == "D":
        return "⚠️ 有极性，认阴极标记带"
    if prefix == "Y":
        # 4 脚无源晶体**不用分方向**，这点反直觉但是查过接线的：
        #   pad1/pad3 成对角，都接晶体电极；pad2/pad4 成对角，都接地
        # 转 180° 后 pin1↔pin3、pin2↔pin4，而无源晶体两个电极物理对称
        # （石英片两面镀电极），GND 换 GND 更无影响 —— 电气上完全等价。
        # 转 90° 才会把地接到信号脚，但 3.2×2.5 的长方形焊盘塞不进去。
        #
        # 实物上那个斜切角在**底面**（焊接面），贴的时候朝下根本看不见，
        # 让人去认它是没有意义的要求。
        #
        # ⚠️ 这个结论只对**无源晶体**成立。有源振荡器（OSC）是 VCC/GND/OUT/EN，
        # 四个脚功能各异，绝对分方向。判据：两个脚都接 GND 就是无源晶体。
        return "无源晶体，转 180° 等价，长边对长边即可" if "4Pin" in fp else ""
    if prefix == "FL":
        return "⚠️ SAW 滤波器分输入输出，贴反不工作"
    if prefix == "U":
        return "认 pin1（缺角/圆点）"
    # Q（SOT-23 的 MOS）**故意不标**：三个引脚位置不对称，物理上就塞不反。
    # 把"贴不反"的东西放进注意清单，只会稀释 D2/D3 那种真会贴反的警告。
    return ""

tree = ET.parse(NET)
sheet, value = {}, {}
for c in tree.iter("comp"):
    r = c.get("ref")
    sp, v = c.find("sheetpath"), c.find("value")
    sheet[r] = sp.get("names") if sp is not None else ""
    value[r] = v.text if v is not None else "?"

stage_of = {sh: (code, name) for code, name, sh, *_ in STAGES}

# V3.2 实物板上位号丝印被印反的那几对：位号 → 板上实际印着的字
silk_wrong = {}
for rev, pairs in AS_BUILT_SILK_SWAPS.items():
    for a, c, fit in pairs:
        silk_wrong[a] = (c, fit, rev)
        silk_wrong[c] = (a, fit, rev)

board = pcbnew.LoadBoard(PCB)
rows = []
for f in board.GetFootprints():
    ref = f.GetReference().strip()
    fp = f.GetFPIDAsString().split(":")[-1]
    m = re.match(r"^([A-Za-z]+)(\d+)$", ref)
    prefix, num = (m.group(1), int(m.group(2))) if m else (ref, 0)
    rows.append({
        "ref": ref, "prefix": prefix, "num": num,
        "val": value.get(ref, "—"), "fp": fp,
        "x": f.GetPosition().x / 1e6, "y": f.GetPosition().y / 1e6,
        "sheet": sheet.get(ref, ""),
    })


def sort_key(r):
    p = r["prefix"]
    return (PREFIX_ORDER.index(p) if p in PREFIX_ORDER else 99, p, r["num"])


rows.sort(key=sort_key)

# 位号跳号统计。动态算而不是写死，否则改板后这段说明就骗人了。
_gap = []
_by_prefix = {}
for _r in rows:
    _by_prefix.setdefault(_r["prefix"], []).append(_r["num"])
for _p in PREFIX_ORDER:
    _ns = sorted(n for n in _by_prefix.get(_p, []) if n)
    if not _ns:
        continue
    _miss = [n for n in range(1, max(_ns) + 1) if n not in _ns]
    if _miss:
        _gap.append(f"`{_p}` 缺 {', '.join(f'{_p}{n}' for n in _miss)}")
_GAPS_PLACEHOLDER = (
    "\n当前跳号：" + "；".join(_gap) + "。\n"
    "\n以 Q/F 为例核对过：Q2+F2(978 UAT)、Q3+F3(1090)、Q4+F4(GNSS 外接)、"
    "Q5+F5(GNSS 内置 patch) 是**四组配对**的天线偏置电路（PMOS 高边开关 + "
    "自恢复保险丝），每组共享一个 *_FUSE 网络，无落单。Q1/F1 在 git 全历史里"
    "从未出现过——是编号习惯，不是删过东西。\n"
) if _gap else ""

NO_PART = ("TestPoint_Pad", "SolderJumper", "ANT_IFA", "MountingHole")
out = [f"# {BOARD_REV} BOM 核对清单（按位号顺序）\n",
       "> 由 `tools/gen_checklist.py` 生成。手工贴片时拿着这份逐行核对；",
       "> 按料分组的表见 `BOM_PURCHASE.md` / `BOM_嘉立创SMT.xlsx`。\n",
       f"\n共 {len(rows)} 个位号。**「贴」列打勾即可**。\n",
       "\n## 图例\n",
       "\n- **阶段** —— 建议的贴片批次，见 `ASSEMBLY.md`。同一阶段贴完就能上电验证",
       "\n- **不贴** —— DNP（设计上默认不贴）或本来就没有器件的焊盘",
       "\n- 🔴 —— 板上丝印印错了，**别信板上的字，认坐标**",
       "\n- ⚠️ —— 有极性或有方向，贴反了不工作\n",
       "\n## 位号为什么不连续\n",
       "\n位号是手工写在 `tools/sheet_*.py` 里的，不是自动编号，所以有跳号。",
       "**这不是漏件**——每次改板都会重新核对一遍，结果附在下面。\n",
       _GAPS_PLACEHOLDER,
       "\n| ✓ | 位号 | 值 / 型号 | 封装 | 位置 (x, y) | 阶段 | 备注 |",
       "|---|---|---|---|---|---|---|"]

n_place = 0
export_rows = []          # 给打印版（xlsx / pdf）用的结构化数据
for r in rows:
    ref, fp, val = r["ref"], r["fp"], r["val"]
    notes = []
    skip = None
    if any(s in fp for s in NO_PART):
        skip = {"MountingHole": "安装孔", "TestPoint": "测试点焊盘",
                "SolderJumper": "短接焊盘", "ANT_IFA": "板载天线"}[
            next(k for k in ("MountingHole", "TestPoint", "SolderJumper", "ANT_IFA")
                 if k in fp)]
    elif re.search(r"DNP", val):
        skip = "DNP"
    elif ref == "U14":
        skip = "备用检波位（默认贴 U13）"

    if ref in silk_wrong:
        other, fit, rev = silk_wrong[ref]
        notes.append(f"🔴 板上印着 `{other}`" + ("，**尺寸相同会真贴错**" if fit else ""))
    pol = polarity_note(r["prefix"], fp) if not skip else ""
    if pol:
        notes.append(pol)
    # 警示色只给「贴反了真会坏」的。晶振那条说的是"不用分方向"，
    # 是宽心话不是警告，标黄反而误导。
    warn = bool(pol) and not pol.startswith("无源晶体")

    if skip:
        stage, box = f"**不贴**（{skip}）", "—"
    else:
        code, name = stage_of.get(r["sheet"], ("?", "?"))
        stage, box = f"{code} {name}", "☐"
        n_place += 1
    out.append(f"| {box} | **{ref}** | {val} | {fp[:22]} | "
               f"({r['x']:.1f}, {r['y']:.1f}) | {stage} | {'；'.join(notes)} |")
    export_rows.append({
        "ref": ref, "val": val, "fp": fp,
        "pos": f"{r['x']:.1f}, {r['y']:.1f}",
        "stage": stage.replace("**", ""), "place": skip is None,
        # 打印版按这两个标记上色：红=丝印印错、黄=有极性
        "silk_wrong": ref in silk_wrong,
        "polarity": warn,
        "note": "；".join(n.replace("🔴 ", "").replace("⚠️ ", "").replace("**", "")
                          for n in notes),
    })

open(OUT, "w").write("\n".join(out) + "\n")

# 同时吐一份结构化数据给打印版用。
#
# 为什么要中间文件：读 .kicad_pcb 必须用 KiCad 自带的 python（只有它有 pcbnew），
# 而生成 xlsx 要 openpyxl、在系统 python 里。两个解释器凑不到一起，所以这里
# 只负责把数据捞出来，排版交给 gen_checklist_print.py。
import json                                            # noqa: E402
JSON_OUT = os.path.join(T, "build", "checklist.json")
os.makedirs(os.path.dirname(JSON_OUT), exist_ok=True)
json.dump({"rev": BOARD_REV, "rows": export_rows},
          open(JSON_OUT, "w"), ensure_ascii=False, indent=1)

assert n_place + sum(1 for r in rows
                     if any(s in r["fp"] for s in NO_PART)
                     or re.search(r"DNP", r["val"]) or r["ref"] == "U14") == len(rows), \
    "要贴的 + 不贴的 ≠ 总位号数，有行被漏掉"

print(f"OK: {len(rows)} 个位号（{n_place} 个要贴）→ {OUT}")
by_prefix = {}
for r in rows:
    by_prefix.setdefault(r["prefix"], []).append(r["ref"])
for p in PREFIX_ORDER:
    if p in by_prefix:
        print(f"    {p:4s} ×{len(by_prefix[p]):3d}   {by_prefix[p][0]} … {by_prefix[p][-1]}")
