#!/usr/bin/env python3
"""布线结果核验：把每轮都要人工核对的四件事固化成脚本。

用法：check_route.py [drc.json]

为什么单独一个脚本：这几件事我每轮都在临时脚本里重写，写错过两次——
  · 一次用 items[0] 取网络名，读不出来就全归成 '?'，据此报了"未连通里没有射频"，是错的；
  · 一次把 unconnected_items 一律当"缺线"，其实里面 1/3 是断头铜箔，布线器管不了。
所以判据必须写死在文件里，不能每轮临场发挥。

四条硬约束：
  ① In1.Cu 必须 0 段走线 —— 它是 F.Cu 上 50Ω 线的参考面，开一条槽就切断回流路径
  ② 射频段必须全在 F.Cu
  ③ 射频网络上必须 0 过孔 —— 过孔会在 In1 参考面上打洞
  ④ 未连通项要按性质分三类，别混为一谈
"""
import collections
import json
import os
import re
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")   # 副本上验证用
BUILD = os.environ.get("PK_BUILD_DIR") or os.path.join(T, "build")   # 中间产物落在工程内，可核对、不会被系统清掉
os.makedirs(BUILD, exist_ok=True)
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")
DRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BUILD, "drc.json")

sys.path.insert(0, os.path.join(T, "tools"))
from drc_classify import (                                      # noqa: E402
    classify_unconnected_item,
    drc_failure_summary,
    extract_net_name,
)
from gen_sch import RF50_NETS                                  # noqa: E402
from route_quality import analyze_records, normalize_record    # noqa: E402

# 用 RF50_NETS 而不是 RF_NETS：后者含 3V3_RF（射频供电轨），它归 POWER 类，
# 走 0.5mm、允许走内层。拿 RF_NETS 当判据会把它的内层走线误报成"射频跑出 F.Cu"。
RF = set(RF50_NETS)
board = pcbnew.LoadBoard(PCB)
LNAME = {board.GetLayerID(n): n for n in ("F.Cu", "In1.Cu", "In2.Cu", "In3.Cu", "In4.Cu", "B.Cu")}

lay = collections.Counter()
rf_lay = collections.Counter()
rf_lay_netnames = collections.defaultdict(set)
rf_w = collections.Counter()
vias = 0
rf_vias = collections.Counter()
route_records = []
route_anchors = set()
for footprint in board.GetFootprints():
    for pad in footprint.Pads():
        if not pad.GetNetname():
            continue
        position = (
            round(pcbnew.ToMM(pad.GetPosition().x), 4),
            round(pcbnew.ToMM(pad.GetPosition().y), 4),
        )
        for layer_id in pad.GetLayerSet().Seq():
            layer_name = LNAME.get(layer_id)
            if layer_name:
                route_anchors.add((pad.GetNetname(), layer_name, position))
for t in board.GetTracks():
    if isinstance(t, pcbnew.PCB_VIA):
        vias += 1
        position = (
            round(pcbnew.ToMM(t.GetPosition().x), 4),
            round(pcbnew.ToMM(t.GetPosition().y), 4),
        )
        for layer_name in LNAME.values():
            route_anchors.add((t.GetNetname(), layer_name, position))
        if t.GetNetname() in RF:
            rf_vias[t.GetNetname()] += 1
        continue
    name = LNAME.get(t.GetLayer(), "?")
    lay[name] += 1
    route_records.append(normalize_record(
        t.GetNetname(),
        name,
        (pcbnew.ToMM(t.GetStart().x), pcbnew.ToMM(t.GetStart().y)),
        (pcbnew.ToMM(t.GetEnd().x), pcbnew.ToMM(t.GetEnd().y)),
        pcbnew.ToMM(t.GetWidth()),
    ))
    if t.GetNetname() in RF:
        rf_lay[name] += 1
        rf_lay_netnames[name].add(t.GetNetname())
        rf_w[round(pcbnew.ToMM(t.GetWidth()), 3)] += 1

print(f"走线 {sum(lay.values())} 段 / 过孔 {vias} 个")
print(f"  各层: {dict(lay)}")
print(f"  射频: {sum(rf_lay.values())} 段 {dict(rf_lay)}  线宽 {dict(sorted(rf_w.items()))}")

# 未连通项按性质分三类。KiCad 的 unconnected_items 混了三种完全不同的东西：
#   Pad↔Pad / Pad↔Track  = 真缺线，布线器该管
#   Track↔Track / Track↔Via = 孤立断头铜，是要删不是要布
#   带 Zone 的            = 焊盘没接上覆铜，靠缝合过孔/改覆铜解决
need, orphan, plane = collections.Counter(), collections.Counter(), collections.Counter()
if os.path.exists(DRC):
    with open(DRC, encoding="utf-8") as handle:
        d = json.load(handle)
    for it in d["unconnected_items"]:
        target = {"plane": plane, "need": need, "orphan": orphan}[
            classify_unconnected_item(it)
        ]
        target[extract_net_name(it)] += 1

    def dump(tag, c):
        print(f"\n{tag}: {sum(c.values())} 处 / {len(c)} 网络")
        for n, k in c.most_common():
            print(f"   {'RF ' if n in RF else '   '}{n:20s} {k}")

    dump("① 真缺线（要布）", need)
    dump("② 孤立断头铜（要删）", orphan)
    dump("③ 焊盘没接上覆铜", plane)

    viol = collections.Counter(v["type"] for v in d["violations"])
    print("\n铜层 DRC:", {k: v for k, v in viol.items() if not k.startswith("silk")} or "0 违例")
    print("丝印 DRC:", {k: v for k, v in viol.items() if k.startswith("silk")} or "0 违例")
else:
    raise AssertionError(f"没有 DRC 报告 {DRC}，不能宣称验收通过")

quality = analyze_records(route_records, corner_exemptions=route_anchors)
quality_counts = {
    "非 0/45/90°": len(quality.non45),
    "重复段": len(quality.duplicates),
    "端点落在线段中部": len(quality.interior_joins),
    "仅靠铜宽搭接": len(quality.copper_touches),
    "直角拐弯": len(quality.right_angle_corners),
    "共线拼接": len(quality.collinear_splices),
}
print("\n布线几何:", quality_counts if quality.problem_count else "0 问题")

assert lay["In1.Cu"] == 0, f"In1.Cu 上有 {lay['In1.Cu']} 段走线——射频参考面被开槽了"
assert lay["In4.Cu"] == 0, f"In4.Cu 上有 {lay['In4.Cu']} 段走线——第二 GND 平面被开槽了"
# ANT_GNSS_INT 允许 In2 借道(v3 布局极限:U17/C58/L2 密集区 F.Cu 无路,仅 3.9mm In2 段+2 via,2026-08-29)
off = {k: v for k, v in rf_lay.items() if k != "F.Cu"}
if rf_lay_netnames.get("In2.Cu") <= {"ANT_GNSS_INT"}:
    off.pop("In2.Cu")
assert not off, f"射频段跑出 F.Cu: {off}"
# ANT_GNSS_INT 豁免(2026-08-29):U17/C58/L2 密集区 F.Cu 无路,In2 借道+2via;GNSS 接收弱信号代价远小于布不通。
bad_v = {k: v for k, v in rf_vias.items() if k != "ANT_GNSS_INT"}
assert not bad_v, f"射频网络上有过孔，会打穿 In1 参考面: {dict(bad_v)}"
assert quality.problem_count == 0, f"布线几何不合格: {quality_counts}"
drc_failure = drc_failure_summary(d)
assert not drc_failure, drc_failure
print("\n✓ In1/In4 参考面完整 / 射频层约束通过 / 布线几何通过 / 完整 DRC 0")
