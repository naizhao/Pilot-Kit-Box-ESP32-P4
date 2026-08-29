#!/usr/bin/env python3
"""导入 freerouting 的 .ses，再把射频走线合并回来，最后重灌覆铜。

配套 export_dsn.py 的 keepout 方案（那边有完整的来龙去脉）：
射频由 KRT 先布在空板上 → 转成 keepout 喂给 freerouting → freerouting 只布数字段 →
本脚本把真实射频走线贴回去。

为什么不直接把 .ses 导进"带射频的板子"：ImportSpecctraSES 会按 session 的内容重排
走线，而 session 里根本没有射频网络（export_dsn.py 已把它们从网表摘掉）。
从"没有射频的板子"（/tmp/norf.kicad_pcb，export_dsn.py 顺手存的）导入、再贴回射频，
路径是确定的，不用赌 KiCad 的合并语义。

运行：~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 import_ses.py
"""
import collections
import json
import os
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")   # 副本上跑整条链用
BUILD = os.environ.get("PK_BUILD_DIR") or os.path.join(T, "build")   # 中间产物落在工程内，可核对、不会被系统清掉
os.makedirs(BUILD, exist_ok=True)
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")
NORF = os.path.join(BUILD, "norf.kicad_pcb")
RF_JSON = os.path.join(BUILD, "rf_tracks.json")
SES = os.path.join(BUILD, "exp.ses")

sys.path.insert(0, os.path.join(T, "tools"))
from gen_sch import RF50_NETS                                  # noqa: E402

# 空表 = export_dsn.py 跑的是 plain 模式，射频由 freerouting 自己布在 .ses 里，
# 不需要贴回。非空 = keepout 模式，射频是 KRT 先布好的，必须原样贴回来。
rf = json.load(open(RF_JSON))

board = pcbnew.LoadBoard(NORF)
n_before = len(list(board.GetTracks()))
assert pcbnew.ImportSpecctraSES(board, SES), "SES 导入失败"
n_ses = len(list(board.GetTracks()))
print(f"导入 .ses: {n_before} → {n_ses} 项")

# 射频走线原样贴回。坐标/线宽都是导出时的 KiCad 内部单位，没经过浮点往返。
for r in rf:
    net = board.FindNet(r["net"])
    assert net, f"板上找不到网络 {r['net']}"
    t = pcbnew.PCB_TRACK(board)
    t.SetStart(pcbnew.VECTOR2I(r["sx"], r["sy"]))
    t.SetEnd(pcbnew.VECTOR2I(r["ex"], r["ey"]))
    t.SetWidth(r["w"])
    t.SetLayer(board.GetLayerID(r["layer"]))
    t.SetNet(net)
    board.Add(t)
print(f"射频走线贴回: {len(rf)} 段" if rf else "plain 模式：射频由 freerouting 布，无需贴回")

pcbnew.ZONE_FILLER(board).Fill(board.Zones())
filled = sum(1 for z in board.Zones() if z.IsFilled())
assert filled == len(board.Zones()), f"覆铜填充 {filled}/{len(board.Zones())} 区未全填"
print(f"覆铜重灌: {filled}/{len(board.Zones())} 区")

# 四条硬约束当场核对，别等到出图才发现（check_route.py 里有同样的判据，
# 那边是给人看的报告，这里是流水线的闸门）。
lname = {board.GetLayerID(n): n for n in ("F.Cu", "In1.Cu", "In2.Cu", "In3.Cu", "In4.Cu", "B.Cu")}
lay = collections.Counter()
rf_lay = collections.Counter()
rf_via = collections.Counter()
for t in board.GetTracks():
    if isinstance(t, pcbnew.PCB_VIA):
        if t.GetNetname() in RF50_NETS:
            rf_via[t.GetNetname()] += 1
        continue
    lay[lname.get(t.GetLayer(), "?")] += 1
    if t.GetNetname() in RF50_NETS:
        rf_lay[lname.get(t.GetLayer(), "?")] += 1

assert lay["In1.Cu"] == 0, f"In1.Cu 上有 {lay['In1.Cu']} 段走线——射频参考面被开槽了"
off = {k: v for k, v in rf_lay.items() if k != "F.Cu"}
assert not off, f"射频段跑出 F.Cu: {off}"
assert not rf_via, f"射频网络上有过孔，会打穿 In1 参考面: {dict(rf_via)}"
if rf:
    assert sum(rf_lay.values()) == len(rf), \
        f"射频段数对不上：贴回 {len(rf)} 段，板上数到 {sum(rf_lay.values())} 段"
else:
    assert sum(rf_lay.values()) > 100, \
        f"plain 模式下射频只有 {sum(rf_lay.values())} 段，freerouting 大概没布"
print(f"各层走线: {dict(lay)}")
print(f"✓ In1 参考面完整 / 射频 {sum(rf_lay.values())} 段全在 F.Cu / 射频 0 过孔")

board.Save(PCB)
print("saved:", PCB)
