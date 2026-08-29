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

nets = {n.GetNetname(): n.GetNetCode() for n in board.GetNetsByNetcode().values()}
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
        # 只传纯整数 netcode。NETINFO_ITEM 的 SWIG 代理会被此前脚本的 Add/Remove
        # 操作污染，SetNet(proxy) 在内存中看似正确，序列化后却会悄悄变成别的网络。
        t.SetNetCode(nets[net])
    board.Add(t)

for net, x, y, w, d in data["vias"]:
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(pcbnew.VECTOR2I_MM(x, y))
    v.SetWidth(pcbnew.FromMM(w))
    v.SetDrill(pcbnew.FromMM(d))
    if net:
        v.SetNetCode(nets[net])
    board.Add(v)

board.Save(PCB)
subprocess.run([sys.executable, "-c",
                f"import pcbnew;b=pcbnew.LoadBoard({PCB!r});"
                f"pcbnew.ZONE_FILLER(b).Fill(b.Zones());b.Save({PCB!r})"],
               check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print(f"清掉原有 {old} 项，贴回 {len(data['tracks'])} 段走线 / {len(data['vias'])} 个过孔")
print(f"来源: {data.get('source', '?')} → {PCB}")
print("⚠️ 贴回后务必跑一次 kicad-cli drc 核对未连通/DRC 与快照当时一致")
