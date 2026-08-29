#!/usr/bin/env python3
"""修位号丝印互换——两个元件的位号文字被印到了对方身上。

2026-08-20 评审发现 C25/R33 丝印错位，全板一查有 4 对。

## 为什么这种错法特别阴

不是缺字漏字那种一眼能看见的问题：**每个元件旁边都有一个位号，排版看着完全
正常**，只是内容张冠李戴。做板前的目检、DRC、Gerber 预览全都发现不了，
一直到有人拿着板子对着 BOM 找件才会发现。

而且后果分两档：

    C46(3pF) ↔ C47(200pF)    两个都是 0603 电容，外观一模一样 → 必然贴错
    L14(100nH) ↔ C30(100pF)  都是 0402，都在 1090 射频通路上 → 必然贴错
    R33(100k) ↔ C25(100nF)   都是 0603，电阻黑色电容米色，能分辨但位置会混
    C18(10uF) ↔ C6(1uF)      0805 vs 0603，尺寸不同塞不进去 → 贴不错，只是读着错

## 判据方向：站在元件旁边找字，不是拿着字找元件

第一版反着写，漏掉了 C46/C47——它俩的位号都被挤到 U10 那颗大 QFN 边上，
「离文字最近的元件」都算成 U10，于是"看起来没跟谁对调"。人的实际用法是
看着元件找旁边的字，所以要对每个元件找最近的文字。

另外**不能用封装名判断能不能贴错**：`R_0603_1608Metric` 和 `C_0603_1608Metric`
名字不同，物理尺寸都是 1.6×0.8mm，照样塞得进去。必须比实际包围盒。

## 会误报的两种情况（已排除，不要去"修"）

  · **超大包围盒**——ANT1 是 IFA 天线，包围盒 46×8.5mm 是一条长走线。
    ZP1 的位号正常地在自己右边 1.4mm，却落在天线的盒子里
  · **统一偏移排版**——C10/C11/C16 的位号都在元件正上方 1.43mm，是排版规则。
    只是 C11 头顶恰好是 U5 的底边

只处理**双向互换**（A 的字更贴 B、同时 B 的字更贴 A），这两种都不满足。

## 幂等

修完就检测不到互换了，重复跑不会再换回去。

用法：fix_silk_refs.py [--dry]      改完记得跑 export_placement.py 固化
"""
import math
import os
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")
DRY = "--dry" in sys.argv

lck = os.path.join(BDIR, "~expansion-board-v3.kicad_pcb.lck")
if os.path.exists(lck) and not DRY:
    sys.exit(f"❌ KiCad 正开着这块板（{lck}）。\n"
             f"   现在改文件，KiCad 里的内存版本还是旧的，它一保存就把改动盖掉。\n"
             f"   请先在 KiCad 里关掉这块 PCB，再跑一次。")

board = pcbnew.LoadBoard(PCB)
fps = {}
for f in board.GetFootprints():
    nb = f.GetBoundingBox(False, False)          # 不含文字，否则位号自己会把盒子撑大
    fps[f.GetReference().strip()] = {
        "f": f,
        "bx": (nb.GetLeft() / 1e6, nb.GetTop() / 1e6,
               nb.GetRight() / 1e6, nb.GetBottom() / 1e6),
        "t": (f.Reference().GetPosition().x / 1e6,
              f.Reference().GetPosition().y / 1e6),
        "w": nb.GetWidth() / 1e6, "h": nb.GetHeight() / 1e6,
        "vis": f.Reference().IsVisible(),
    }


def d2box(p, bx):
    l, t, r, bo = bx
    return math.hypot(max(l - p[0], 0, p[0] - r), max(t - p[1], 0, p[1] - bo))


def find_swaps(d):
    ks = [k for k in d if d[k]["vis"]]
    out = []
    for i in range(len(ks)):
        for j in range(i + 1, len(ks)):
            a, c = d[ks[i]], d[ks[j]]
            if (d2box(a["t"], c["bx"]) < d2box(a["t"], a["bx"]) and
                    d2box(c["t"], a["bx"]) < d2box(c["t"], c["bx"])):
                out.append((ks[i], ks[j]))
    return out


swaps = find_swaps(fps)
if not swaps:
    print("✅ 没有位号互换，不用改")
    sys.exit(0)

print(f"发现 {len(swaps)} 对位号互换：\n")
for a, c in swaps:
    A, C = fps[a], fps[c]
    fit = abs(A["w"] - C["w"]) < 0.3 and abs(A["h"] - C["h"]) < 0.3
    print(f"  {a} ↔ {c}   {'🔴 尺寸相同，会真贴错' if fit else '🟢 尺寸不同，贴不错'}")
    for r in (a, c):
        p = fps[r]
        print(f"      {r:5s} 本体在 ({p['f'].GetPosition().x/1e6:7.2f},"
              f"{p['f'].GetPosition().y/1e6:7.2f})  文字却在 "
              f"({p['t'][0]:7.2f},{p['t'][1]:7.2f})")

if DRY:
    print("\n（--dry，没有改文件）")
    sys.exit(0)

for a, c in swaps:
    ta, tc = fps[a]["f"].Reference(), fps[c]["f"].Reference()
    pa, pc = ta.GetPosition(), tc.GetPosition()
    aa, ac = ta.GetTextAngle(), tc.GetTextAngle()
    ta.SetPosition(pc)
    tc.SetPosition(pa)
    # 角度也要跟着换：两个元件旋转不同时，文字角度是配着各自本体排的，
    # 只换位置不换角度会出现一个横排一个竖排挤在一起
    ta.SetTextAngle(ac)
    tc.SetTextAngle(aa)

board.Save(PCB)

# 落盘后重新读一遍验证——不能只信内存里改过了
chk = pcbnew.LoadBoard(PCB)
d2 = {}
for f in chk.GetFootprints():
    nb = f.GetBoundingBox(False, False)
    d2[f.GetReference().strip()] = {
        "bx": (nb.GetLeft() / 1e6, nb.GetTop() / 1e6,
               nb.GetRight() / 1e6, nb.GetBottom() / 1e6),
        "t": (f.Reference().GetPosition().x / 1e6,
              f.Reference().GetPosition().y / 1e6),
        "vis": f.Reference().IsVisible()}
left = find_swaps(d2)
assert not left, f"❌ 修完还剩 {left}，交换逻辑有问题"

print(f"\n✅ 已交换 {len(swaps)} 对位号文字，重新读盘验证：0 对互换")
for a, c in swaps:
    print(f"    {a:5s} 文字 → ({d2[a]['t'][0]:7.2f},{d2[a]['t'][1]:7.2f})   "
          f"{c:5s} 文字 → ({d2[c]['t'][0]:7.2f},{d2[c]['t'][1]:7.2f})")
print("\n⚠️ 接着跑 export_placement.py 把新位置固化进 PLACEMENT.py，"
      "否则下次 rebuild 会退回错位状态")
