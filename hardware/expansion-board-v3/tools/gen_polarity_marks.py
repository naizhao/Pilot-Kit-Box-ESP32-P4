#!/usr/bin/env python3
"""审计全部二极管，并给缺少清晰标识的器件补阴极线。

## 起因

手工贴片前查极性时发现：D2/D3 是 0402 封装的 ESD 管，KiCad 官方封装
`D_0402_1005Metric` 只给了一个**半径 0.05mm 的圆点**标 pin1，加上 0.1mm 线宽，
实物上可见直径约 0.2mm。这个尺寸在放大镜下都难认，而 D2/D3 混在 22 个 0402
电容和 9 个 0402 电感中间，外观几乎一样——贴反了电路直接不工作。

D1 是 SOD-123，官方封装给了三条线组成的 `[` 形，阴极侧有竖线；脚本会按几何
确认它确实合格。D2/D3 没有合格横杠，因此补板级丝印。以后新增任何 D* 两脚器件
也会自动进入同一审计，不再依赖封装名称白名单。

## 方向是验证过的，不是照搬约定

KiCad 的 `D_*` 封装约定 pad1 = 阴极(K)、pad2 = 阳极(A)。这里按电路复核过：

    D1  pad1=VCC_5V(K) / pad2=USB_VBUS(A)   USB 5V 经肖特基流向 VCC_5V，防倒灌 ✓
    D2  pad1=ANT1090_EXT(K) / pad2=GND(A)   ESD 管阴极接信号、阳极接地 ✓
    D3  pad1=ANT_978(K) / pad2=GND(A)       同上 ✓

## 为什么画在板级而不是改封装

`D_0402_1005Metric` 是 KiCad 官方库的封装，改它会波及所有项目。加成板级丝印
（PCB_SHAPE）则只影响本板，代价是元件移动后要重跑本脚本——所以它进了
rebuild.sh，每次重建都会按当前坐标重新算。

## 几何

标记是一条垂直于两焊盘连线的短线，画在阴极那一端的外侧：

        ┌───┐ ┌───┐
    ║   │ K │ │ A │          ║ = 本脚本加的阴极标记线
        └───┘ └───┘

位置沿用官方封装那个小圆点的距离（距元件中心 1.09mm），已确认与焊盘留有
0.2mm 间隙。线宽 0.2mm、长 0.7mm——比原来的圆点醒目一个数量级。

用法：**KiCad 的 python3** tools/gen_polarity_marks.py
      幂等，重复跑不会叠加
"""
import math
import os
import sys

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from board_meta import PCB_BASENAME                     # noqa: E402
from polarity_geometry import (has_clear_cathode_bar, is_diode_target,
                               marker_segment)          # noqa: E402

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = os.path.join(BDIR, f"{PCB_BASENAME}.kicad_pcb")

OFFSET_MM = 1.09        # 元件中心 → 标记中心。沿用官方封装 pin1 圆点的距离
LEN_MM = 0.70           # 标记线长度
WIDTH_MM = 0.20         # 线宽。板规 min_text_thickness=0.08，这里取 0.2 求醒目

lck = os.path.join(BDIR, f"~{PCB_BASENAME}.kicad_pcb.lck")
if os.path.exists(lck):
    sys.exit(f"❌ KiCad 正开着这块板，它一保存就会盖掉改动。请先关闭再跑。")

board = pcbnew.LoadBoard(PCB)

targets, existing, dropped = [], [], []
pending_drop = []
# ⚠️ 必须 list() 快照。下面会在循环体内调 f.Remove(g) 删封装圆点，那个删除会让
# board.GetFootprints() 的 SWIG 迭代器失效——后续拿到的 f 退化成裸 SwigPyObject，
# 连 Pads() 都不能迭代。内层的 list(f.GraphicalItems()) 只保护了内层，保护不了这里。
#
# 原来没暴露，是因为当时 D4/D5 被跳过、删除只发生在遍历后期。2026-09-03 取消
# 那个例外后，D4 成了第一个触发删除的器件，重建链当场炸在 f.Pads()。
for f in list(board.GetFootprints()):
    ref = f.GetReference().strip()
    fp = f.GetFPIDAsString().split(":")[-1]
    pads = {p.GetNumber(): p.GetPosition() for p in f.Pads()}
    # D4/D5 曾经被排除在外，理由是"双向 TVS 没有安装极性，画阴极会误导装配"。
    # 2026-09-03 取消这个例外，四颗 TPESD8L3.3 统一处理：
    #
    #   · D2/D3 与 D4/D5 是**同一颗料**，都是双向管。既然 D2/D3 画了而 D4/D5 不画，
    #     板上四颗一模一样的器件长得却不一样，装配的人反而要停下来想为什么。
    #   · 这条线的实际作用不是"指示电流方向"，而是"这个位置是二极管不是电容"——
    #     0402 的 ESD 管混在 22 颗 0402 电容和 9 颗 0402 电感中间，外观完全一样。
    #     对双向管来说按标记摆放不会摆错，不标反而更容易被当成电容漏贴。
    #   · 本文件开头就写着"以后新增任何 D* 两脚器件也会自动进入同一审计，
    #     不再依赖封装名称白名单"，这个例外与那个设计意图是冲突的。
    #
    # 旧注释的另一条理由"删封装圆点会触发 SWIG 迭代器失效"**是真的**，取消例外
    # 之后当场复现了（D4 成了第一个触发删除的器件，重建链炸在 f.Pads()）。
    # 处理方式不是继续回避，而是修掉根因：外层遍历加 list() 快照 + 删除延后到
    # 遍历结束。见文件上方那段说明。
    if not is_diode_target(ref, pads):
        continue
    c = f.GetPosition()
    k, a = pads["1"], pads["2"]                 # pad1=阴极, pad2=阳极
    center = (c.x / 1e6, c.y / 1e6)
    cathode = (k.x / 1e6, k.y / 1e6)
    anode = (a.x / 1e6, a.y / 1e6)
    footprint_segments = []
    for graphic in f.GraphicalItems():
        if (graphic.GetClass() == "PCB_SHAPE"
                and graphic.GetLayer() == pcbnew.F_SilkS
                and graphic.GetShape() == pcbnew.SHAPE_T_SEGMENT):
            start, end = graphic.GetStart(), graphic.GetEnd()
            footprint_segments.append((
                (start.x / 1e6, start.y / 1e6),
                (end.x / 1e6, end.y / 1e6),
                graphic.GetWidth() / 1e6,
            ))
    if has_clear_cathode_bar(center, cathode, anode, footprint_segments):
        existing.append((ref, fp, "封装已有清晰阴极线"))
        continue
    cathode_distance = math.hypot(cathode[0] - center[0], cathode[1] - center[1])
    offset = max(OFFSET_MM, cathode_distance + 0.60)
    length = max(LEN_MM, min(2.0, cathode_distance * 1.2))
    start, end = marker_segment(center, cathode, anode, offset, length)
    targets.append((ref, fp, start, end))

    # 官方封装那个半径 0.05mm 的 pin1 圆点要删掉。
    #
    # 不删的话它正好落在新标记线的正中间，两者重叠——实测 DRC 的 silk_overlap
    # 从 1 涨到 3。把线挪开躲它也不行：0402 焊盘外侧总共就那么点地方，挪远了
    # 又会顶到别的元件。而这个圆点本来就是被取代的对象，留着没有意义。
    #
    # 它来自封装库，rebuild 会重新带回来，所以这里每次跑都删一遍（幂等）。
    for g in list(f.GraphicalItems()):
        if (g.GetClass() == "PCB_SHAPE"
                and g.GetLayer() == pcbnew.F_SilkS
                and g.GetShape() == pcbnew.SHAPE_T_CIRCLE):
            rad = math.hypot(g.GetEnd().x - g.GetStart().x,
                             g.GetEnd().y - g.GetStart().y) / 1e6
            if rad < 0.15:                      # 只删这种当标记用的小点
                # 收集不删。真正的 Remove 放到遍历结束后统一做——
                # 在循环里删会让外层迭代器失效（见上面那段说明）。
                pending_drop.append((f, g, ref, rad))

# 圆点统一在这里删——遍历已经结束，不会再动 GetFootprints() 的迭代器。
for _f, _g, _ref, _rad in pending_drop:
    _f.Remove(_g)
    dropped.append(f"{_ref} 的 pin1 圆点(r={_rad:.2f}mm)")

# 幂等：先删掉上一轮画的。判据是「线宽等于本脚本的 WIDTH_MM，且两端点与这次
# 要画的位置吻合」——只按线宽会误删别的丝印，只按位置又漏掉线宽改过的情况。
want = {(round(s[0], 2), round(s[1], 2), round(e[0], 2), round(e[1], 2))
        for _, _, s, e in targets}
removed = 0
for d in list(board.GetDrawings()):
    if d.GetClass() != "PCB_SHAPE" or d.GetLayer() != pcbnew.F_SilkS:
        continue
    if abs(d.GetWidth() / 1e6 - WIDTH_MM) > 0.001:
        continue
    s, e = d.GetStart(), d.GetEnd()
    key = (round(s.x / 1e6, 2), round(s.y / 1e6, 2),
           round(e.x / 1e6, 2), round(e.y / 1e6, 2))
    if key in want:
        board.Remove(d)
        removed += 1

for ref, fp, s, e in targets:
    sh = pcbnew.PCB_SHAPE(board)
    sh.SetShape(pcbnew.SHAPE_T_SEGMENT)
    sh.SetStart(pcbnew.VECTOR2I_MM(round(s[0], 3), round(s[1], 3)))
    sh.SetEnd(pcbnew.VECTOR2I_MM(round(e[0], 3), round(e[1], 3)))
    sh.SetLayer(pcbnew.F_SilkS)
    sh.SetWidth(pcbnew.FromMM(WIDTH_MM))
    board.Add(sh)

board.Save(PCB)
print(f"清掉旧的 {removed} 条，画了 {len(targets)} 条阴极标记 → {PCB}")
for ref, fp, reason in existing:
    print(f"    {ref:4s} {fp:22s} {reason}")
for d in dropped:
    print(f"    删除 {d}")
for ref, fp, s, e in targets:
    print(f"    {ref:4s} {fp:22s} ({s[0]:7.2f},{s[1]:6.2f}) → ({e[0]:7.2f},{e[1]:6.2f})")
if not targets:
    print("    （没有需要补标记的封装）")
