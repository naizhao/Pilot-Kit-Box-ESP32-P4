#!/usr/bin/env python3
"""调位号丝印字号——只改大小，不动位置。

2026-08-13 评审："元件丝印字号 size 是否可以再小一些？当前我觉得还是大，挡住了。
其实能看清楚就行。"

## 为什么当前是 1.0mm

`gen_pcb.py` 从零建板时给位号的默认字高就是 1.0mm。本该由 `gen_silk.py` 统一
重排成 0.8mm，但它在行对齐机检处断言失败、没跑完，所以 156 个位号全停在 1.0mm。

## 为什么下限是 0.8mm，不能再小

    板规 min_text_height = 0.8mm
    板规 min_text_thickness = 0.08mm，但**实际受厂家限制**：
    嘉立创丝印线宽下限 0.15mm，低于这个印出来是糊的、或者干脆印不上

字高 1.0 → 0.8 是降 20%，但位号占的**矩形面积降 36%**（宽高同时缩），
对"挡住东西"这件事的改善比听起来大。再往下没有合规空间了。

笔画宽保持 0.15mm 不动——那是厂家下限，细了印不清，跟字高不是一回事。

用法：silk_size.py [字高mm]     默认 0.8
"""
import collections
import os
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")

H = float(sys.argv[1]) if len(sys.argv) > 1 else 0.8
MIN_H = 0.8        # 板规 min_text_height，同时也是嘉立创丝印可印下限
THICK = 0.15       # 嘉立创丝印线宽下限，不随字高缩
assert H >= MIN_H, f"字高 {H} 低于下限 {MIN_H}mm——印不出来或糊成一团"

board = pcbnew.LoadBoard(PCB)
mm = pcbnew.ToMM
before = collections.Counter()
n = 0
for f in board.GetFootprints():
    t = f.Reference()
    if t.GetLayer() not in (pcbnew.F_SilkS, pcbnew.B_SilkS):
        continue
    before[round(mm(t.GetTextHeight()), 2)] += 1
    t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(H), pcbnew.FromMM(H)))
    t.SetTextThickness(pcbnew.FromMM(THICK))
    n += 1
board.Save(PCB)
print(f"{n} 个位号字高 {dict(before)} → {H}mm（笔画 {THICK}mm 保持不变）")
print(f"占地面积变为原来的 {(H / max(before)) ** 2 * 100:.0f}%")
