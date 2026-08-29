#!/usr/bin/env python3
"""只补背面品牌丝印，不动任何位号。

## 为什么单独拆出来

2026-08-13 评审问："为什么除了元件以外的其他丝网（比如背后的版权）没有了？"

查出来是条流程漏洞：
  · `gen_pcb.py:194` 是 `pcbnew.NewBoard()`——**从零重建板子**，只放元件、覆铜、
    板框，板级丝印（gr_text）它不生成
  · 生成品牌丝印的 `gen_silk.py` **从来没进过 run_route.sh / rebuild.sh**
所以每跑一次布线流程，版权那几行就没了。实测板上 gr_text = 0 条、fp_text = 147 条
（元件位号在，板级文字全丢）。

## 为什么不直接跑 gen_silk.py

它一并要**重排 147 个位号**，而刚手工调过布局走线和丝印，重排会覆盖这些调整。
而且它当前在行对齐机检处断言失败：

    AssertionError: 位号未与同行对齐：R21(y=63.233 vs 同行同高82.71)

63.233 是 R21 在 zero 版的旧位置（55.6, 63.2），而它现在元件在 (115.15,85.17)、
位号在 (115.15,86.61)——**脚本内部的行分组状态和板子实际状态对不上**。
更麻烦的是它**先改后验**：断言失败时修改已经落盘，板子停在改了一半的状态。

位号重排那件事（当前丝印 DRC 43 重叠 + 42 压铜）留着单独修 gen_silk 的 bug 再做。
这里只做评审问的那件事，风险最小：**只加板级文字，一个位号都不碰**。

文字内容、版本号、板框中线全部来自 board_meta.py，两个脚本 import 同一份，
不存在"记得同步"这回事了。

用法：gen_brand_silk.py            幂等，重复跑不会叠加
"""
import os
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")

# 板级元数据的唯一来源。改版本号只改 board_meta.py，别在这里复制一份——
# 「改了一处漏了另一处」这块板已经栽过 5 次。
from board_meta import (BOARD_NAME, BOARD_REV, BOARD_DATE,   # noqa: E402
                        COPYRIGHT, WEBSITE, SUBTITLE, CX)

# (文字, y, 字高)。y 和字高是本脚本独有的排版，其余全来自 board_meta
LINES = [
    (BOARD_NAME, 74.0, 3.0),
    (f"{BOARD_REV}   {BOARD_DATE}", 80.0, 2.0),
    (COPYRIGHT, 85.0, 1.6),
    (SUBTITLE, 90.0, 1.4),
    (WEBSITE, 95.0, 1.8),
]

board = pcbnew.LoadBoard(PCB)
CENTER = pcbnew.GR_TEXT_H_ALIGN_CENTER

# 幂等：先删掉本脚本上次加的文字，不然重复跑会一层层叠上去。
#
# ⚠️ 判据必须是**位置**，不能是文字内容。原来写的是「文字等于 LINES 里的某一条
# 才删」——版本号一改（V3.2 → V3.3），板上那行旧的 "V3.2   2026-08" 就不在
# LINES 里了，删不掉，然后新的 V3.3 又加上去，两行**重叠印在同一个坐标上**。
# 丝印糊成一团，而且 Gerber 预览里两行叠着很难看出来。
#
# 按位置删，而且**只按 y，不带 x**：历史上 gen_silk 用 CX=100.00、
# gen_brand_silk 写死 100.05（算错了），带 x 的判据谁也删不掉谁，
# 实测板上这 5 行叠了两层共 10 条。y + 层已经足够唯一。
SLOTS = {round(y, 2) for _, y, _ in LINES}
old = 0
for d in list(board.GetDrawings()):
    if d.GetClass() not in ("PCB_TEXT", "PCB_TEXTBOX"):
        continue
    if d.GetLayer() != pcbnew.B_SilkS:
        continue
    py = round(d.GetPosition().y / 1e6, 2)
    if py in SLOTS:
        print(f"    清掉旧的: {d.GetText()!r} @ y={py} x={d.GetPosition().x/1e6:.3f}")
        board.Remove(d)
        old += 1

for s, y, h in LINES:
    t = pcbnew.PCB_TEXT(board)
    t.SetText(s)
    t.SetPosition(pcbnew.VECTOR2I_MM(round(CX, 3), round(y, 3)))
    t.SetLayer(pcbnew.B_SilkS)
    t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(h), pcbnew.FromMM(h)))
    t.SetTextThickness(pcbnew.FromMM(h * 0.17))
    t.SetHorizJustify(CENTER)
    t.SetMirrored(True)          # 背面文字必须镜像，否则从背面看是反的
    board.Add(t)

board.Save(PCB)
print(f"清掉旧的 {old} 条，加了 {len(LINES)} 条背面品牌丝印 → {PCB}")
for s, y, h in LINES:
    print(f"    B.SilkS  y={y:5.1f}  h={h}mm  {s}")
