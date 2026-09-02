#!/usr/bin/env python3
"""阶段 D：把 VALUE / DNP / FOOTPRINT 三个维度从网表同步到候选板。

PAD_NET 由 eco_v39_bind_nets.py 负责，这里管剩下三维。

**VALUE 是重头：174 个封装全错。** V3 的 `gen_pcb.py:392` 那句
`for ref, fpid, _ in comps:` 只解包三元组、没取 value，后面也没有 `SetValue()`，
于是板上每个元件的 Value 属性都是封装名——`C10` 写着 `C_0603_1608Metric` 而不是
`100nF`。两个后果：装配图印出来的是封装名，跟 BOM 对不上；
`fanout_channel.py:56` 的 `if "DNP" in f.GetValue()` 判据**永远为假、静默失效**。
V4 早修好了（四元组 + SetValue），V3 一直没享受到。本脚本先把候选板补齐，
gen_pcb.py 那边同步修，重建才不会又退回去。

FOOTPRINT 只有三处，都是 C7 那批 0402/0603 口径调整的连带：
  · C87 新加的时候借了 C21 的 0402 几何，但它不在 LOCAL_0402_REFS 里，应是 0603
  · R52/R53 刚被纳入 LOCAL_0402_REFS，板上还是 0603，应改 0402
换封装用"从板上找一个同封装的件复制几何"，不走库加载——库路径在不同机器上不一定在。

用法：KiCad 自带 python3 tools/eco_v39_sync_metadata.py
"""
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import pcbnew

ROOT = Path(__file__).resolve().parents[1]
CANDIDATE = ROOT / "internal" / "work" / "v3.9" / "expansion-board-v3-v39-candidate.kicad_pcb"
NETLIST = ROOT / "build" / "netlist.xml"
HOLES = {"H1", "H2", "H3", "H4"}


def main() -> int:
    root = ET.parse(NETLIST).getroot()
    comps = {c.attrib["ref"]: c for c in root.findall("./components/comp")}
    board = pcbnew.LoadBoard(str(CANDIDATE))
    refs = {fp.GetReference(): fp for fp in board.GetFootprints()}

    # ── FOOTPRINT：先换封装，因为换完之后 value/dnp 要重新写一遍 ──
    swapped = []
    for reference, comp in comps.items():
        footprint = refs.get(reference)
        if footprint is None:
            continue
        want = (comp.findtext("footprint") or "").split(":")[-1]
        have = footprint.GetFPIDAsString().split(":")[-1]
        if not want or want == have:
            continue
        donor = next((fp for fp in board.GetFootprints()
                      if fp.GetFPIDAsString().split(":")[-1] == want
                      and fp.GetReference() != reference), None)
        if donor is None:
            raise SystemExit(f"{reference} 要换成 {want}，但板上找不到同封装的件可借几何")
        position, orientation = footprint.GetPosition(), footprint.GetOrientation()
        nets = {p.GetNumber(): p.GetNet() for p in footprint.Pads()}
        board.Remove(footprint)
        try:
            clone = donor.Duplicate(False)
        except TypeError:
            clone = donor.Duplicate()
        clone = pcbnew.Cast_to_FOOTPRINT(clone)
        clone.SetParent(board)
        board.Add(clone)
        clone.SetReference(reference)
        clone.SetPosition(position)
        clone.SetOrientation(orientation)
        for pad in clone.Pads():                 # 网络跟着旧焊盘走，别丢
            if pad.GetNumber() in nets:
                pad.SetNet(nets[pad.GetNumber()])
        swapped.append(f"{reference} {have} → {want}")
        refs = {fp.GetReference(): fp for fp in board.GetFootprints()}

    # ── VALUE / DNP ──
    value_fixed, dnp_fixed = [], []
    for reference, comp in comps.items():
        footprint = refs.get(reference)
        if footprint is None:
            continue
        want_value = comp.findtext("value") or ""
        if footprint.GetValue() != want_value:
            value_fixed.append(reference)
            footprint.SetValue(want_value)
        want_dnp = (comp.find('property[@name="dnp"]') is not None
                    or "DNP" in want_value)
        if bool(footprint.IsDNP()) != want_dnp:
            dnp_fixed.append(f"{reference}→{want_dnp}")
            footprint.SetDNP(want_dnp)

    board.Save(str(CANDIDATE))
    print(f"FOOTPRINT 换封装 {len(swapped)}: {swapped}")
    print(f"VALUE 修正 {len(value_fixed)} 个（此前全是封装名）")
    print(f"DNP 修正 {len(dnp_fixed)}: {dnp_fixed}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
