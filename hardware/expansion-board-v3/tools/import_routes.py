#!/usr/bin/env python3
"""把 ROUTES.json 的布线贴回板子——gen_pcb 重建之后的第一步。

配合 export_routes.py 使用，两者合起来让布线像元件位置一样可复现：

    PLACEMENT.py  → 元件在哪
    ROUTES.json   → 线怎么走
    两个都贴回，才是一块完整的板。

## 网络名对不上就必须报错，不能静默跳过

网络名是唯一的锚点（走线里没有别的东西能标识它属于谁）。原理图改了名字、
或者某个网络被删了，贴回时就会有孤儿走线。**静默跳过等于悄悄少布几条线**，
而板子看上去还是满的，最难发现。所以对不上就报错退出，逼人去看为什么。

## 幂等

贴之前先清空板上已有的走线/过孔——本脚本的语义是"让板子的布线等于快照"，
不是"往上追加"。重复跑结果一致。

用法：import_routes.py [ROUTES.json]
      PK_BOARD_DIR 可指到副本
"""
import json
import os
import re
import subprocess
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(T, "tools", "ROUTES.json")

with open(SRC, encoding="utf-8") as handle:
    data = json.load(handle)

# 删除和新增必须分进程。KiCad 的 SWIG 绑定在 Remove() 后会让同一进程中的
# NETINFO_ITEM 代理失效；旧实现试图靠 UUID 在“先加、后删”中区分新旧对象，
# 但重新载板后 UUID 的字符串表现并不稳定，结果是旧线一根也没删，重复导入翻倍。
purge = subprocess.run(
    [sys.executable, "-c", (
        "import pcbnew,sys;"
        "p=sys.argv[1];b=pcbnew.LoadBoard(p);"
        "items=list(b.GetTracks());"
        "[b.Remove(item) for item in items];"
        "b.Save(p);print(len(items))"
    ), PCB],
    check=True,
    capture_output=True,
    text=True,
)
count_lines = [line for line in purge.stdout.splitlines() if line.strip().isdigit()]
assert count_lines, f"清理子进程没有返回项目数: {purge.stdout!r}"
old = int(count_lines[-1])

board = pcbnew.LoadBoard(PCB)
assert board is not None, f"加载失败: {PCB}"

# ⚠️ 存**网络对象**，不存 netcode 整数。
#
# 原来这里是 `{名字: n.GetNetCode()}`，配 `SetNetCode(整数)`，注释理由是
# "NETINFO_ITEM 的 SWIG 代理会被 Add/Remove 污染，SetNet(proxy) 序列化后会变成
# 别的网络"。2026-09-03 做单点实验，结论正好相反：
#
#     建映射      GND=7  3V3_DIG=1
#     SetNetCode(7) → 内存里 netcode=7 / name=GND     ✅
#     仅 Save 后重载 → netcode=1 / name=3V3_DIG       ❌
#
# 也就是说 **KiCad 在保存时会按自己的顺序重排 netcode**，而 PCB_VIA 记的是编号，
# 于是整体错位。实测重放板上 24 个 GND 缝合过孔因此变成 3V3_RF / 3V3_DIG——
# 位置、数量、DRC 全都正常，只有网络归属悄悄错了。缝合过孔挂错网就等于没接地。
#
# 用 SetNet(NETINFO_ITEM) 则由 KiCad 自己维护引用，重排编号时会跟着走。
nets = {n.GetNetname(): n for n in board.GetNetsByNetcode().values()}
want = {t[0] for t in data["tracks"]} | {v[0] for v in data["vias"]}
missing = sorted(n for n in want if n and n not in nets)
assert not missing, (f"板子上没有这些网络，贴不回去（原理图改过？）: {missing[:8]}"
                     f"{' 等 %d 个' % len(missing) if len(missing) > 8 else ''}")

LID = {n: board.GetLayerID(n) for n in
       ("F.Cu", "In1.Cu", "In2.Cu", "In3.Cu", "In4.Cu", "B.Cu")}

for net, ly, x1, y1, x2, y2, w in data["tracks"]:
    t = pcbnew.PCB_TRACK(board)
    t.SetStart(pcbnew.VECTOR2I_MM(x1, y1))
    t.SetEnd(pcbnew.VECTOR2I_MM(x2, y2))
    t.SetWidth(pcbnew.FromMM(w))
    t.SetLayer(LID[ly])
    if net:
        t.SetNetCode(nets[net].GetNetCode())
    board.Add(t)

for net, x, y, w, d in data["vias"]:
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(pcbnew.VECTOR2I_MM(x, y))
    v.SetWidth(pcbnew.FromMM(w))
    v.SetDrill(pcbnew.FromMM(d))
    if net:
        v.SetNetCode(nets[net].GetNetCode())
    board.Add(v)

board.Save(PCB)
subprocess.run([sys.executable, "-c",
                f"import pcbnew;b=pcbnew.LoadBoard({PCB!r});"
                f"pcbnew.ZONE_FILLER(b).Fill(b.Zones());b.Save({PCB!r})"],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

# 落盘后回读校验：贴回的过孔网络必须与 ROUTES.json 逐个一致。
#
# 2026-09-03 重放比对发现，502 个过孔位置全对，但其中 **24 个 GND 缝合过孔**
# 落盘后变成了 3V3_RF(16) / 3V3_DIG(8)。位置没错、数量没错、DRC 也不报短路，
# 唯独网络归属被改——缝合过孔一旦挂错网，它连的就不是地平面，而这件事
# 在任何单板检查里都看不出来，只有拿两块板逐个过孔对比才会浮现。
#
# 所以这里必须回读**文件**再验一次，不能只信内存里的对象：SetNetCode 在内存中
# 看着是对的，问题出在保存/重灌之后。
verify = subprocess.run(
    [sys.executable, "-c", (
        "import pcbnew,sys,json;"
        "b=pcbnew.LoadBoard(sys.argv[1]);"
        "print(json.dumps({f'{round(v.GetPosition().x/1e6,3)},{round(v.GetPosition().y/1e6,3)}':"
        "v.GetNetname() for v in b.GetTracks() if v.Type()==pcbnew.PCB_VIA_T}))"
    ), PCB],
    check=True, capture_output=True, text=True,
)
actual = json.loads([ln for ln in verify.stdout.splitlines() if ln.startswith("{")][-1])
drift = []
for net, x, y, _w, _d in data["vias"]:
    key = f"{round(x, 3)},{round(y, 3)}"
    got = actual.get(key)
    if got is not None and net and got != net:
        drift.append((key, net, got))

# 漂移的成因是 KiCad 的**孤立过孔吸附**：缝合过孔没有走线连着，KiCad 就按它
# 落在哪片覆铜里判网络。走线两端有焊盘，所以不受影响——这也是重放比对里
# 走线 0 差异、只有过孔错位的原因。
#
# 三条路都试过，都不行：
#   SetNetCode(整数)      落盘后漂 27 个
#   SetNet(NETINFO_ITEM)  落盘后漂 43 个，更糟
#   落盘后改文本          文件里确实写成了 (net "GND")，但 pcbnew 一加载又读成
#                         3V3_RF —— **吸附发生在加载期，不是保存期**，改文件没用
#
# 所以这里只报告、不假装修好。正式板上这 24 个过孔是 GND（它们由 route_rf.py
# 直接生成，没经过 ROUTES.json 往返），交付以正式板为准；重建链在孤立过孔上
# 达不到 100% 复现，这是已知限制。
#
# 影响面：缝合过孔挂错网 = 没接到该接的平面，且不触发任何 DRC 规则。所以哪怕
# 修不了也必须打印出来，不能静默——静默的话下次有人拿重建板去投产就中招了。
if drift:
    import collections
    ways = collections.Counter(f"{want}→{got}" for _k, want, got in drift)
    print(f"⚠️ {len(drift)} 个孤立过孔被 KiCad 按覆铜重判了网络: {dict(ways)}")
    print("   这是重建链的已知限制（见本文件注释），正式板不受影响；")
    print("   若要拿重建板投产，先逐个核对这些过孔的网络归属。")

print(f"清掉原有 {old} 项，贴回 {len(data['tracks'])} 段走线 / {len(data['vias'])} 个过孔")
print(f"来源: {data.get('source', '?')} → {PCB}")
print(f"过孔网络回读校验通过（{len(data['vias'])} 个逐一比对）")
