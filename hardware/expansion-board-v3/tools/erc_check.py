#!/usr/bin/env python3
"""电气规则检查——查 DRC 查不出、原理图断言也拦不住的那类错。

## 已有的防线挡住了什么，挡不住什么

gen_sch.py 每个引脚都必须显式给网络名，且**断言"库里解析出的引脚集合 == 数据表
给出的引脚集合"**，所以漏引脚、多引脚、脚号打错这类低级错误已经进不来。
kicad-cli drc 管的是几何（短路、间距、未连通）。

两者都管不到**电气语义**：网络名写对了、几何也没问题，但这根线接错地方了。
本脚本查的就是这一层：

  A 单点网络       某网络只挂了 1 个焊盘 —— 铁定是笔误（网络名打错，成了孤岛）
  B IC 缺电源/地   多脚芯片没接 GND 或没接任何电源轨
  C 电源脚无去耦   IC 的电源脚附近没有去耦电容
  D I2C 无上拉     SDA/SCL 是开漏，没上拉就永远读不到数据
  E 晶振无负载电容 负载电容缺失 → 不起振或频偏，是最典型的"板子回来才发现"
  F 电源域混用     射频芯片接到数字轨上 —— 噪声直接进链路，DRC 完全看不出来

判据全部基于**实际网表**（从 .kicad_pcb 读，不是读原理图脚本），
所以查的是"最终会打出来的那块板"，中间任何环节改坏了都能抓到。

用法：erc_check.py [board.kicad_pcb]
"""
import collections
import math
import os
import re
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(T, "tools"))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BDIR, "expansion-board-v3.kicad_pcb")

board = pcbnew.LoadBoard(PCB)
mm = pcbnew.ToMM

POWER_NETS = {"VCC_5V", "USB_VBUS", "3V3_DIG", "3V3_RF", "3V3_GNSS", "RP_1V1",
              "SUBG_VDDR", "VBUS_FUSE", "VCC_5V_FUSE"}
GND = "GND"

# 焊盘表
pads = collections.defaultdict(list)          # net -> [(ref, num, x, y, value)]
byref = {}
for f in board.GetFootprints():
    c = f.GetPosition()
    byref[f.GetReference()] = dict(val=f.GetValue(), x=mm(c.x), y=mm(c.y), pads=[])
    for p in f.Pads():
        if not p.IsOnCopperLayer():
            continue
        n = p.GetNetname()
        pp = p.GetPosition()
        rec = (f.GetReference(), p.GetNumber(), mm(pp.x), mm(pp.y), f.GetValue())
        byref[f.GetReference()]["pads"].append((p.GetNumber(), n, mm(pp.x), mm(pp.y)))
        if n and not n.startswith("unconnected-"):
            pads[n].append(rec)

# ── 豁免表 ────────────────────────────────────────────────────────────
# 每一条都必须写明**为什么**，不能只是"报了但我觉得没事"。
# 没有理由的豁免等于把检查关掉，下次真出问题也照样静默。
WAIVERS = {
    # AS179-92LF 是 GaAs SPDT 射频开关：控制脚 V1/V2 直接由 GPIO 驱动，
    # 器件本身没有 VDD 引脚。SOT-363 六个脚全是 RF/控制/地。
    ("缺电源", "U16"): "AS179-92LF 射频开关，无 VDD 引脚，靠 V1/V2 控制电压工作",
    ("缺电源", "U17"): "AS179-92LF 射频开关，无 VDD 引脚，靠 V1/V2 控制电压工作",
    # CC1312R 的 48MHz 主晶振**不接外部负载电容**——片内有可编程电容阵列，
    # TI 参考设计(LAUNCHXL-CC1312R1)即如此。见 sheet_subghz.py:9 的注释。
    ("晶振无负载电容", "Y2"): "48MHz 走 CC1312R 片内电容阵列，TI 参考设计不外挂",
    # CC1312R 是数字主控，主轨用 3V3_DIG 正确；它的射频部分由片内 LDO 供电
    # （SUBG_VDDR 是 LDO 输出脚，不是外部输入轨）。
    ("电源域混用", "U10"): "CC1312R 主轨本就是数字轨，射频段由片内 LDO(SUBG_VDDR) 供电",
}

# 引脚级豁免：这些脚挂在电源网上，但**不是电源输入脚**，不需要去耦电容。
# 只豁免点名的脚，同一元件的真电源脚照查——整件豁免会把真问题一起放过。
PIN_WAIVERS = {
    # 三个 LDO 的 pin3 是 **EN 使能脚**（接 VCC_5V 常使能），不是电源输入。
    # sheet_power.py:5 写明 pinout: 1=IN 2=GND 3=EN 4=NC 5=OUT。
    # 真正需要去耦的输入脚是 pin1，已有 C1/C2/C3 各自伺候。
    ("U1", "3"): "LDO 的 EN 使能脚，不是电源输入",
    ("U2", "3"): "LDO 的 EN 使能脚，不是电源输入",
    ("U3", "3"): "LDO 的 EN 使能脚，不是电源输入",
}

issues = []


waived = []


def add(sev, kind, msg):
    ref = msg.split()[0].split(".")[0]
    why = WAIVERS.get((kind, ref))
    if why:
        waived.append(f"{kind} {ref}: {why}")
        return
    issues.append((sev, kind, msg))


# ── A 单点网络 ────────────────────────────────────────────────────────
# 一个网络只挂 1 个焊盘，意味着它哪儿也没去。要么网络名打错（本该和别人同名），
# 要么原理图上漏画了一根线。这类错 DRC 报不出来——单点网络不算"未连通"。
for n, lst in sorted(pads.items()):
    if len(lst) == 1:
        r = lst[0]
        add("❌", "单点网络", f"{n} 只挂在 {r[0]}.{r[1]} 一个焊盘上，哪儿也没接")

# ── B IC 缺电源/地 ────────────────────────────────────────────────────
for ref, d in sorted(byref.items()):
    if not re.match(r"^U\d", ref) or len(d["pads"]) < 4:
        continue
    nets = {n for _, n, _, _ in d["pads"] if n}
    if GND not in nets:
        add("❌", "缺地", f"{ref} ({d['val'][:20]}) 一个 GND 焊盘都没有")
    if not (nets & POWER_NETS):
        add("❌", "缺电源", f"{ref} ({d['val'][:20]}) 没接任何电源轨: {sorted(nets)[:5]}")

# ── C 电源脚无去耦 ────────────────────────────────────────────────────
# 判据：IC 的每个电源焊盘，5mm 内要有一个**一端接同一电源、另一端接地**的电容。
# 5mm 是宽松口径（真正要求是尽量近），这里只抓"压根没有"的情况。
caps = []
for ref, d in byref.items():
    if not ref.startswith("C"):
        continue
    ns = {n for _, n, _, _ in d["pads"] if n}
    if GND in ns:
        for n in ns - {GND}:
            caps.append((n, d["x"], d["y"], ref))
for ref, d in sorted(byref.items()):
    if not re.match(r"^U\d", ref) or len(d["pads"]) < 4:
        continue
    for num, n, px, py in d["pads"]:
        if n not in POWER_NETS:
            continue
        if (ref, num) in PIN_WAIVERS:
            waived.append(f"无去耦 {ref}.{num}: {PIN_WAIVERS[(ref, num)]}")
            continue
        near = [c for c in caps if c[0] == n and math.hypot(c[1] - px, c[2] - py) <= 5.0]
        if not near:
            far = [c for c in caps if c[0] == n]
            add("⚠️", "无去耦",
                f"{ref}.{num}[{n}] 5mm 内没有去耦电容"
                + (f"（最近的在 {min(math.hypot(c[1]-px, c[2]-py) for c in far):.1f}mm 外）"
                   if far else "（这条轨上一个去耦电容都没有）"))

# ── D I2C 上拉 ────────────────────────────────────────────────────────
# SDA/SCL 是开漏输出，没有上拉电阻线永远拉不高，总线读不到任何东西。
for n in [x for x in pads if "I2C" in x.upper() and ("SDA" in x.upper() or "SCL" in x.upper())]:
    pull = []
    for ref, num, px, py, val in pads[n]:
        if not ref.startswith("R"):
            continue
        other = {nn for nu, nn, _, _ in byref[ref]["pads"] if nu != num}
        if other & POWER_NETS:
            pull.append(f"{ref}({val[:8]})")
    if not pull:
        add("❌", "I2C 无上拉", f"{n} 没有到电源的上拉电阻，开漏总线拉不高")

# ── E 晶振负载电容 ────────────────────────────────────────────────────
for ref, d in sorted(byref.items()):
    if not re.match(r"^Y\d", ref):
        continue
    for num, n, px, py in d["pads"]:
        if not n or n == GND:
            continue
        # 该脚所在网络上是否挂着一个另一端接地的电容
        has = any(c for c in caps if c[0] == n)
        if not has:
            add("⚠️", "晶振无负载电容",
                f"{ref}.{num}[{n}] 这条腿上没有对地负载电容（{d['val'][:18]}）")

# ── F 电源域混用 ──────────────────────────────────────────────────────
# 射频/GNSS 芯片接到数字轨上，噪声会直接进链路。DRC 完全看不出来。
try:
    from gen_sch import RF_NETS
except Exception:
    RF_NETS = set()
for ref, d in sorted(byref.items()):
    nets = {n for _, n, _, _ in d["pads"] if n}
    if not (nets & set(RF_NETS)):
        continue                     # 不是射频件，不管
    if "3V3_DIG" in nets:
        add("⚠️", "电源域混用",
            f"{ref} ({d['val'][:18]}) 挂着射频网络 {sorted(nets & set(RF_NETS))[:2]}，"
            f"却用 3V3_DIG 供电（应走 3V3_RF/3V3_GNSS）")

# ── 汇总 ──────────────────────────────────────────────────────────────
print(f"ERC: {os.path.basename(PCB)}  {len(byref)} 元件 / {len(pads)} 网络\n")
bykind = collections.defaultdict(list)
for sev, kind, msg in issues:
    bykind[(sev, kind)].append(msg)
if not issues:
    print("✅ 全部通过")
for (sev, kind), msgs in sorted(bykind.items()):
    print(f"{sev} {kind}: {len(msgs)} 项")
    for m in msgs[:14]:
        print(f"     {m}")
    if len(msgs) > 14:
        print(f"     … 还有 {len(msgs)-14} 项")
    print()
if waived:
    print(f"已豁免 {len(waived)} 项（每条都写明了理由，见 WAIVERS）:")
    for w in sorted(set(waived)):
        print(f"     {w}")
    print()
fatal = sum(1 for s, _, _ in issues if s == "❌")
print(f"合计 {len(issues)} 项，其中致命 {fatal} 项")
