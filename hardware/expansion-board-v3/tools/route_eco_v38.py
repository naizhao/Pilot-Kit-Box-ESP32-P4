#!/usr/bin/env python3
"""重放 V3.8 的 QPL9547 局部 ECO 布线。

ROUTES.json 冻结时 QPL9547 尚无完整偏置网络。本脚本只补 pin1 的 3.32k
偏置/100pF 旁路，以及 pin7 的 18nH 馈电和本地去耦，不改动已闭环的其余布线。
"""

import os

import pcbnew


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(ROOT, "kicad")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")


def pad(board, ref, number):
    footprint = board.FindFootprintByReference(ref)
    assert footprint, f"找不到 {ref}"
    hit = next((item for item in footprint.Pads()
                if item.GetNumber() == str(number)), None)
    assert hit, f"找不到 {ref}.{number}"
    point = hit.GetPosition()
    return (pcbnew.ToMM(point.x), pcbnew.ToMM(point.y)), hit.GetNetname()


def add_track(board, netname, layer, points, width=0.15):
    net = board.FindNet(netname)
    assert net, f"找不到网络 {netname}"
    for start, end in zip(points, points[1:]):
        track = pcbnew.PCB_TRACK(board)
        track.SetStart(pcbnew.VECTOR2I_MM(*start))
        track.SetEnd(pcbnew.VECTOR2I_MM(*end))
        track.SetWidth(pcbnew.FromMM(width))
        track.SetLayer(board.GetLayerID(layer))
        track.SetNet(net)
        board.Add(track)


def add_via(board, netname, point):
    net = board.FindNet(netname)
    assert net, f"找不到网络 {netname}"
    via = pcbnew.PCB_VIA(board)
    via.SetPosition(pcbnew.VECTOR2I_MM(*point))
    via.SetWidth(pcbnew.FromMM(0.45))
    via.SetDrill(pcbnew.FromMM(0.30))
    via.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
    via.SetNet(net)
    board.Add(via)


board = pcbnew.LoadBoard(PCB)

u11_1, nv_bias = pad(board, "U11", 1)
r11_2, nr11 = pad(board, "R11", 2)
c21_1, nc21 = pad(board, "C21", 1)
assert nv_bias == nr11 == nc21 == "LNA1_VBIAS"

# U11.1 已由冻结快照接到 100.975/69.8005 过孔；新增无源件从背面经 In3 接入。
vbias_anchor = (100.9750, 69.8005)
r11_fan = (97.25, 64.50)
c21_fan = (100.00, 65.60)
for point in (r11_fan, c21_fan):
    add_via(board, nv_bias, point)
add_track(board, nv_bias, "B.Cu", [r11_2, r11_fan])
add_track(board, nv_bias, "B.Cu", [c21_1, c21_fan])
add_track(board, nv_bias, "In2.Cu", [vbias_anchor, (100.975, 66.575),
                                      c21_fan, (98.35, 65.60), r11_fan])

# 三颗旁路电容的地端就近落完整地平面。
for ref in ("C21", "C36", "C48"):
    point, netname = pad(board, ref, 2)
    assert netname == "GND"
    fan = (point[0] + 0.65, point[1])
    add_via(board, netname, fan)
    add_track(board, netname, "B.Cu", [point, fan])

# 3V3_RF 端统一在 In3 汇到 L1 供电孔，避免依赖背面覆铜岛的形状。
l1_1, n3v3 = pad(board, "L1", 1)
l1_2, nout = pad(board, "L1", 2)
u11_7, nu11_out = pad(board, "U11", 7)
assert (n3v3, nout, nu11_out) == ("3V3_RF", "LNA1_OUT", "LNA1_OUT")
power_via = (106.20, 70.285)
add_via(board, n3v3, power_via)
power_fans = []
for ref, pin in (("R11", 1), ("C36", 1), ("C48", 1)):
    point, netname = pad(board, ref, pin)
    assert netname == n3v3
    fan = (point[0], point[1] - 0.80) if ref == "R11" else \
        (point[0] - 0.65, point[1])
    add_via(board, netname, fan)
    add_track(board, netname, "B.Cu", [point, fan])
    power_fans.append(fan)
assert len(power_fans) == 3
add_track(board, n3v3, "In3.Cu", [power_fans[0], (99.51, 63.70),
                                    (100.79, 64.98), power_fans[1],
                                    power_fans[2], (103.85, 64.98),
                                    (104.85, 65.98), (104.85, 68.935),
                                    power_via])
add_track(board, n3v3, "F.Cu", [l1_1, power_via], width=0.25)

# pin7 输出节点在 C31.1 汇合；绕开 L1.pin1 后以 45° 接回，保持射频段在 F.Cu。
add_track(board, nout, "F.Cu", [l1_2, (104.30, 69.315),
                                  (104.00, 69.615), (104.00, 70.20), (105.00, 71.20),
                                  (105.52, 71.20)], width=0.25)

pcbnew.ZONE_FILLER(board).Fill(board.Zones())
board.Save(PCB)
print("V3.8 ECO：QPL9547 偏置、馈电与本地去耦已重放")
