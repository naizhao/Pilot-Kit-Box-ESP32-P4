#!/usr/bin/env python3
"""把当前板上的全部布线导出成 ROUTES.json —— 布线的 PLACEMENT.py。

## 为什么必须有

`gen_pcb.py:194` 是 `pcbnew.NewBoard()`：**跑一次 run_route.sh 就从零重建板子**，
所有走线全没。元件位置有 PLACEMENT.py 兜着，布线一直裸奔。

代价是实打实的：2026-08-12 手工挪了 3 个过孔、拉通了 SUBG_VDDR / RP_1V1 /
SUBG_IRQ，把板子做到了未连通 0 / 铜层 DRC 0 —— 这些改动**没有任何地方存着**，
再跑一次整链就没了。而且事后也捞不回来：全板 IsLocked 都是 False，
手画的和 freerouting 布的混在一起，角度也区分不了
（freerouting 自己也会产生非 45° 走线，它的日志有 "90 traces not 45 degree"）。

所以不做"识别手工改动"这件不可能的事，改做**全量快照**：把当前这份达到 0/0 的
布线整体存下来当基线。以后 gen_pcb 重建布局之后原样贴回，手工修改自然就保住了。

## 为什么是 JSON 而不是 .py

PLACEMENT.py 存 152 个元件，写成 python 字面量还能读。布线有 1300+ 段，
写成 .py 只是个巨大的列表，没有可读性收益，反而每次 import 都要解析执行。
JSON + 稳定排序，git diff 能直接看出"哪几段变了"。

## 排序必须稳定

否则每次导出的行序都不同，git diff 全是噪音，也就没法核对改了什么
（export_dsn.py 踩过同一个坑：KiCad 导出 DSN 的顺序不可复现，
同一块板两次导出的布线结果能差 ±8）。

用法：export_routes.py [源板.kicad_pcb]      默认 kicad/ 下的当前板
"""
import json
import os
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(BDIR, "expansion-board-v3.kicad_pcb")
OUT = os.environ.get("PK_ROUTES_OUT") or os.path.join(T, "tools", "ROUTES.json")

board = pcbnew.LoadBoard(SRC)
assert board is not None, f"加载失败: {SRC}"
mm = pcbnew.ToMM
LN = {board.GetLayerID(n): n for n in
      ("F.Cu", "In1.Cu", "In2.Cu", "In3.Cu", "In4.Cu", "B.Cu")}


def r4(v):
    return round(mm(v), 4)


tracks, vias = [], []
for t in board.GetTracks():
    net = t.GetNetname()
    if isinstance(t, pcbnew.PCB_VIA):
        p = t.GetPosition()
        vias.append([net, r4(p.x), r4(p.y), r4(t.GetWidth()), r4(t.GetDrill())])
    else:
        ly = LN.get(t.GetLayer())
        assert ly, f"走线在非铜层 {t.GetLayer()}：{net}"
        s, e = t.GetStart(), t.GetEnd()
        tracks.append([net, ly, r4(s.x), r4(s.y), r4(e.x), r4(e.y), r4(t.GetWidth())])

tracks.sort(key=lambda x: (x[0], x[1], x[2], x[3], x[4], x[5]))
vias.sort(key=lambda x: (x[0], x[1], x[2]))

data = {
    "_comment": "布线快照。字段：tracks=[net,layer,x1,y1,x2,y2,width] "
                "vias=[net,x,y,width,drill]，单位 mm。由 export_routes.py 生成，"
                "import_routes.py 贴回。元件位置在 PLACEMENT.py，两者合起来才是完整的板。",
    "source": os.path.basename(SRC),
    "tracks": tracks,
    "vias": vias,
}
with open(OUT, "w") as f:
    json.dump(data, f, ensure_ascii=False, indent=0)
    f.write("\n")

import collections                                                # noqa: E402
byl = collections.Counter(t[1] for t in tracks)
print(f"导出 {len(tracks)} 段走线 / {len(vias)} 个过孔 → {OUT}")
print(f"  各层: {dict(byl)}")
print(f"  网络数: {len({t[0] for t in tracks} | {v[0] for v in vias})}")
