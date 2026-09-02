#!/usr/bin/env python3
"""阶段 C 收尾：按新网表把候选板的焊盘网络同步过来，并拆掉因此失效的铜。

KiCad 的"从原理图更新 PCB"在 GUI 里，`kicad-cli pcb` 只有 drc/export/import/render/
upgrade，没有 update。所以这一步得自己做。

要同步的不只是新增的四个件：
  · C2 把 R17/R18/R26/R27 的上拉端从 3V3_GNSS 改到了 3V3_DIG
  · C5 把 U15.5 从 PULSES 改成 PULSES_RAW
  · C87/R55/R56/R57 是新件，焊盘网络还是从 donor 封装复制来的（借的是 C21/R17 的）

**焊盘换网络之后，原来接它的走线就成了错的线**——线还标着旧网络，一头却接到了新网络
的焊盘上，DRC 会报短路。所以凡是网络变了的焊盘，与它相接的走线一律拆掉，
交给阶段 E 重布。拆的量在这里如实打印出来，不藏。

用法：KiCad 自带 python3 tools/eco_v39_bind_nets.py
"""
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import pcbnew

ROOT = Path(__file__).resolve().parents[1]
CANDIDATE = ROOT / "internal" / "work" / "v3.9" / "expansion-board-v3-v39-candidate.kicad_pcb"
NETLIST = ROOT / "build" / "netlist.xml"
CLEANER = Path(__file__).resolve().parent / "eco_v39_ripup_conflicts.py"


def netlist_pads() -> dict:
    root = ET.parse(NETLIST).getroot()
    wanted = {}
    for net in root.findall("./nets/net"):
        name = net.attrib["name"]
        for node in net.findall("node"):
            wanted[(node.attrib["ref"], node.attrib["pin"])] = name
    if not wanted:
        raise SystemExit("网表里一个节点都没解析出来")
    return wanted


def main() -> int:
    wanted = netlist_pads()
    board = pcbnew.LoadBoard(str(CANDIDATE))

    existing = {net.GetNetname(): net for net in board.GetNetInfo().NetsByName().values()}
    changed = []
    for footprint in board.GetFootprints():
        reference = footprint.GetReference()
        for pad in footprint.Pads():
            key = (reference, pad.GetNumber())
            if key not in wanted:
                continue
            target = wanted[key]
            if pad.GetNetname() == target:
                continue
            net = existing.get(target)
            if net is None:
                net = pcbnew.NETINFO_ITEM(board, target)
                board.Add(net)
                existing[target] = net
            changed.append((reference, pad.GetNumber(), pad.GetNetname(), target))
            pad.SetNet(net)

    # 网络变了的焊盘，脚下那些线现在指向错误的网络，全部拆掉
    doomed = {}
    for reference, number, _old, _new in changed:
        footprint = board.FindFootprintByReference(reference)
        pad = {p.GetNumber(): p for p in footprint.Pads()}.get(number)
        if pad is None:
            continue
        for layer in pad.GetLayerSet().CuStack():
            try:
                shape = pad.GetEffectiveShape(layer)
            except Exception:
                continue
            for track in board.GetTracks():
                if not track.IsOnLayer(layer):
                    continue
                try:
                    if shape.Collide(track.GetEffectiveShape(layer), 0):
                        doomed[track.m_Uuid.AsString()] = track
                except Exception:
                    continue
    for track in doomed.values():
        board.Remove(track)
    board.BuildConnectivity()
    board.Save(str(CANDIDATE))

    stray = 0
    for _round in range(12):
        done = subprocess.run([sys.executable, str(CLEANER), "--clean-once"],
                              capture_output=True, text=True)
        import re
        marker = re.search(r"CLEANED (\d+)", done.stdout)
        removed = int(marker.group(1)) if marker else 0
        stray += removed
        if removed == 0:
            break

    print(f"焊盘网络变更 {len(changed)} 处:")
    for reference, number, old, new in changed:
        print(f"  {reference}.{number}  {old or '(无)'} → {new}")
    print(f"因此拆掉的铜 {len(doomed)} 个对象，随后清理断头 {stray} 个")
    return 0


if __name__ == "__main__":
    sys.exit(main())
