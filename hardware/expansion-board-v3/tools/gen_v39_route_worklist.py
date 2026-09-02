#!/usr/bin/env python3
"""阶段 E 交接：从候选板导出待布线清单。

旧的那份 46 条（2026-08-31 交接文档）**整份作废**：U13/R20 删除释放了一片区域、
四颗上拉从 3V3_GNSS 改挂 3V3_DIG、U15.5 改走 PULSES_RAW，还多出 C87/R55/R56/R57
四个新焊盘。照旧清单去连，会连到已经不存在的端点上。

清单按网络分组，每组给出：要连的焊盘对、直线距离、以及该网络当前已有多少铜。
排序按"最长的先看"——长飞线通常要先定走向，短的往往顺着长的走。

用法：KiCad 自带 python3 tools/gen_v39_route_worklist.py
"""
import collections
import math
import sys
from pathlib import Path

import pcbnew

ROOT = Path(__file__).resolve().parents[1]
CANDIDATE = ROOT / "internal" / "work" / "v3.9" / "expansion-board-v3-v39-candidate.kicad_pcb"
OUT = ROOT / "internal" / "work" / "v3.9" / "ROUTE_WORKLIST.md"

# 阶段 A/B/C 动过的件，布线时值得留意
TOUCHED = {
    "R47", "R50", "R51", "R52", "R53", "R54", "C81", "C82", "C83", "C84", "C85",
    "C86", "D4", "D5", "C21", "C36", "C48", "R11", "C87", "R55", "R56", "R57",
    "R17", "R18", "R26", "R27", "U15", "U14", "R21", "R36", "U4", "U8",
}


def main() -> int:
    board = pcbnew.LoadBoard(str(CANDIDATE))
    connectivity = board.GetConnectivity()
    connectivity.RecalculateRatsnest()

    # 每个网络的焊盘，以及现存铜的数量
    pads = collections.defaultdict(list)
    for footprint in board.GetFootprints():
        for pad in footprint.Pads():
            name = pad.GetNetname()
            if not name or name.startswith("unconnected-"):
                continue
            position = pad.GetPosition()
            pads[name].append((position.x / 1e6, position.y / 1e6,
                               footprint.GetReference(), pad.GetNumber()))
    copper = collections.Counter()
    for track in board.GetTracks():
        name = track.GetNetname()
        if name:
            copper[name] += 1

    # 未连通的部分：同网络里被分成多个连通块的，块与块之间就是要连的
    unconnected = []
    for name, points in pads.items():
        if len(points) < 2:
            continue
        groups = []
        for point in points:
            placed = False
            for group in groups:
                if any(math.dist(point[:2], q[:2]) < 0.001 for q in group):
                    group.append(point)
                    placed = True
                    break
            if not placed:
                groups.append([point])
        # 用 KiCad 自己的未连通计数，几何分组只用来给出最近的一对端点
        unconnected.append(name)

    report = json_like_report(board, pads, copper)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(report, encoding="utf-8")
    print(f"清单已写入 {OUT.relative_to(ROOT)}")
    return 0


def json_like_report(board, pads, copper):
    """按 DRC 的未连通条目组织清单——那是权威判据，比自己算连通块可靠。"""
    import json
    import subprocess
    import tempfile
    cli = str(Path.home() / "Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli")
    with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as handle:
        report_path = handle.name
    subprocess.run([cli, "pcb", "drc", "--refill-zones", "--severity-all",
                    "--format", "json", "-o", report_path, str(CANDIDATE)],
                   capture_output=True, check=True)
    data = json.loads(Path(report_path).read_text(encoding="utf-8"))
    Path(report_path).unlink()

    items = collections.defaultdict(list)
    for entry in data.get("unconnected_items", []):
        ends = entry.get("items", [])
        if len(ends) < 2:
            continue
        first, second = ends[0], ends[1]
        name = ""
        for end in ends:
            marker = end.get("description", "")
            if "[" in marker:
                name = marker.split("[", 1)[1].split("]", 1)[0]
                break
        distance = math.dist(
            (first["pos"]["x"], first["pos"]["y"]),
            (second["pos"]["x"], second["pos"]["y"]),
        )
        items[name].append((distance, first, second))

    lines = [
        "# V3.9 待布线清单",
        "",
        f"候选板 `{CANDIDATE.name}`，共 **{sum(len(v) for v in items.values())} 处**未连通，"
        f"分布在 **{len(items)}** 个网络。",
        "",
        "旧的 46 条清单（2026-08-31 交接文档）**整份作废**——U13/R20 删除释放了区域、",
        "四颗上拉改挂 3V3_DIG、U15.5 改走 PULSES_RAW，另有 C87/R55/R56/R57 四个新焊盘。",
        "",
        "按网络分组，组内按距离降序。`铜` 列是该网络当前已有的走线/过孔数量，",
        "为 0 表示整条网络都要从头连。",
        "",
    ]
    for name in sorted(items, key=lambda n: -max(d for d, _a, _b in items[n])):
        entries = sorted(items[name], reverse=True)
        touched = {ref for ref in TOUCHED
                   if any(ref in e[1].get("description", "") + e[2].get("description", "")
                          for e in entries)}
        flag = f"  ⚠️ 本轮动过: {', '.join(sorted(touched))}" if touched else ""
        lines.append(f"## {name} — {len(entries)} 处，现有铜 {copper.get(name, 0)}{flag}")
        lines.append("")
        lines.append("| 距离 mm | 端点 A | 端点 B |")
        lines.append("|---:|---|---|")
        for distance, first, second in entries:
            lines.append(f"| {distance:.2f} | {first.get('description', '')[:52]} "
                         f"| {second.get('description', '')[:52]} |")
        lines.append("")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    sys.exit(main())
