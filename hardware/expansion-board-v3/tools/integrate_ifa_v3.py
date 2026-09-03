#!/usr/bin/env python3
"""把V4.0逐刀实测定型的50mm IFA局部回灌到V3正式PCB。

⚠️ 2026-09-04：本值由49mm更正为50mm。49是记录错误——实板逐刀切到50.0就停了，
从来没到过49.0。依据与斜率见`gen_ifa_footprint.py`的ARM_OUT注释。
板号**不随本次更正递增**，仍为V3.10（产品裁定：同一批投板内的尺寸更正不占版本号）。

与从ROUTES.json整板重建不同，本脚本只替换ANT1库封装并更新板级版本文字，
保留正式板上的全部走线、过孔、覆铜、布局和人工修正。

环境变量：
  PK_BOARD_DIR   输出板目录，默认 ../kicad
  PK_SOURCE_PCB  可选输入板；省略时原位更新输出板
"""

from pathlib import Path
import os
import re

import pcbnew

from board_meta import BOARD_DATE, BOARD_REV
from ifa_geom import BBOX_LOCAL


TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
BOARD_DIR = Path(os.environ.get("PK_BOARD_DIR", ROOT / "kicad"))
TARGET = BOARD_DIR / "expansion-board-v3.kicad_pcb"
SOURCE = Path(os.environ.get("PK_SOURCE_PCB", TARGET))
LIB = BOARD_DIR / "expansion-board-v3.pretty"
REVISION = re.compile(r"\bV3\.\d+\b")

assert SOURCE.is_file(), f"输入PCB不存在: {SOURCE}"
assert LIB.is_dir(), f"IFA封装库不存在: {LIB}"
assert abs((BBOX_LOCAL[2] - BBOX_LOCAL[0]) - 50.0) < 0.001, BBOX_LOCAL

board = pcbnew.LoadBoard(str(SOURCE))
fps = {f.GetReference(): f for f in board.GetFootprints()}
assert "ANT1" in fps, "PCB缺少ANT1"
old = fps["ANT1"]
new = pcbnew.FootprintLoad(str(LIB), "ANT_IFA_1090MHz")
assert new is not None, f"无法加载 {LIB}:ANT_IFA_1090MHz"

old_value = old.GetValue()
old_locked = old.IsLocked()
old_path = old.GetPath().AsString()
old_pos = old.GetPosition()
old_angle = old.GetOrientationDegrees()
pad_nets = {p.GetNumber(): p.GetNetname() for p in old.Pads()}
assert pad_nets == {"1": "ANT1090_IFA", "2": "GND"}, pad_nets

board.Remove(old)
board.Add(new)
new.SetReference("ANT1")
new.SetValue(old_value)
new.SetLocked(old_locked)
new.SetPosition(old_pos)
new.SetOrientationDegrees(old_angle)
if old_path:
    new.SetPath(pcbnew.KIID_PATH(old_path))
for pad in new.Pads():
    netname = pad_nets.get(pad.GetNumber())
    assert netname, f"新ANT1出现未知焊盘 {pad.GetNumber()}"
    net = board.FindNet(netname)
    assert net, f"PCB缺少网络 {netname}"
    pad.SetNet(net)

changed_texts = 0
for drawing in board.GetDrawings():
    if drawing.GetClass() != "PCB_TEXT":
        continue
    text = drawing.GetText()
    if REVISION.search(text):
        drawing.SetText(REVISION.sub(BOARD_REV, text))
        changed_texts += 1
assert changed_texts == 1, f"板级版本文字数量异常: {changed_texts}"

# ── 顶部禁铜带必须通长贯穿整板，不许有缝 ─────────────────────────
# 🔴 2026-09-04 事故：这条带是**三块拼的**——两块板级区（`gen_pcb.py` 里
# 写死坐标）+ ANT1 封装自带的那块（跟着天线走）。主臂由 53.5 缩到 50.0mm 后
# 只有中间那块缩了，带子上开了 **3.5mm 的洞**（v3: 131.722..135.222），
# GND 当场灌进去，正落在倒 F 天线开路端外侧——电场最强的一头。
# 参考设计（4.3 原板）那条带是横跨整板的矩形，判据是**从板左沿到右沿连续**。
# 边界取 ANT1 禁铜区**在板上的实际包围盒**，不要用 KEEPOUT_LOCAL 现算：
# 它的 [3] 是脚末端 y，比多边形真实下沿少半个线宽（0.75mm），照它算会在
# 纵向再开一个洞。
BOARD_L, BOARD_R = 50.000, 150.000
_band = {z.GetZoneName(): z for z in board.Zones()
         if z.GetIsRuleArea() and z.GetZoneName() in
         ("ANT1090_short_end_keepout", "ANT1090_open_end_keepout")}
assert len(_band) == 2, f"顶部禁铜带缺块: 只找到 {sorted(_band)}"
_ant_fp = next(f for f in board.GetFootprints() if f.GetReference() == "ANT1")
_ant_ko = max((z for z in _ant_fp.Zones() if z.GetIsRuleArea()),
              key=lambda z: z.GetBoundingBox().GetWidth())
_kb = _ant_ko.GetBoundingBox()
_bx0, _bx1 = pcbnew.ToMM(_kb.GetLeft()), pcbnew.ToMM(_kb.GetRight())
_by0, _by1 = pcbnew.ToMM(_kb.GetTop()), pcbnew.ToMM(_kb.GetBottom())


def _set_rect(zone, x0, y0, x1, y1):
    """原地挪四个顶点。**不要用 SetOutline()**——它接管指针，传 Python 对象会段错误。"""
    poly = zone.Outline()
    assert poly.VertexCount() == 4, \
        f"{zone.GetZoneName()} 不是四点矩形（{poly.VertexCount()} 点）"
    for i, (px, py) in enumerate(((x0, y0), (x1, y0), (x1, y1), (x0, y1))):
        poly.SetVertex(i, pcbnew.VECTOR2I(pcbnew.FromMM(px), pcbnew.FromMM(py)))


_set_rect(_band["ANT1090_short_end_keepout"], BOARD_L, _by0, _bx0, _by1)
_set_rect(_band["ANT1090_open_end_keepout"], _bx1, _by0, BOARD_R, _by1)
for _a, _b in zip([(BOARD_L, _bx0), (_bx0, _bx1), (_bx1, BOARD_R)][:-1],
                  [(BOARD_L, _bx0), (_bx0, _bx1), (_bx1, BOARD_R)][1:]):
    assert abs(_a[1] - _b[0]) < 1e-6, \
        f"顶部禁铜带在 x={_a[1]:.3f}..{_b[0]:.3f} 断开——铺铜会灌进这个缝"

BOARD_DIR.mkdir(parents=True, exist_ok=True)
# 🔴 换 ANT1 封装 = 禁铜区几何变了，必须重灌覆铜再存盘。
# 已填好的铜不会自己跟着退，会停在**旧**禁铜区边界上。
# 而 `kicad-cli drc --refill-zones` 是内存重填，DRC 因此全绿；
# `export gerbers` 导出的却是文件里的旧填充——两者可以同时成立。
# 详见 v4 同名脚本里的完整说明（2026-09-04 实测踩到，1mm 铜压在开路端外侧）。
pcbnew.ZONE_FILLER(board).Fill(board.Zones())
board.Save(str(TARGET))

boxes = [p.GetBoundingBox() for p in new.Pads()]
left = min(pcbnew.ToMM(b.GetLeft()) for b in boxes)
right = max(pcbnew.ToMM(b.GetRight()) for b in boxes)
assert abs((right - left) - 50.0) < 0.002, f"ANT1外包络={right-left:.3f}mm"

print(f"V3 ANT1局部回灌完成: {SOURCE} → {TARGET}")
print(f"  铜箔外包络={right-left:.3f}mm，版本={BOARD_REV} {BOARD_DATE}")
print(f"  保留走线/过孔共 {len(board.GetTracks())} 项")
