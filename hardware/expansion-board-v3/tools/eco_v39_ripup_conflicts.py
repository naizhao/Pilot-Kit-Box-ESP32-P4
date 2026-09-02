#!/usr/bin/env python3
"""阶段 A 第二步：拆掉与新增/换面元件冲突的母版铜，把冲突网络交给阶段 E 重布。

背景。V3.8 母版的布线是干净的（DRC 0 违规 / 0 未连接），但它是**在没有这 18 个件的
前提下**布出来的。把件放进去，必然有焊盘落在既有铜上——实测 13/18 件冲突，压到 15 个
网络的 24 段走线 + 8 个过孔。

为什么是拆线而不是给这 13 件重新找位置：
  · 拆线代价实测 24/1620 段 = **1.5%**，而且阶段 E 本来就要人工布线，合并进去几乎不增量。
  · 重新找位置的代价被低估了——C82-C86 是 RP2040 的本地去耦，必须贴着 U8；在一块已经
    布满线的板上给它们找"既不压线又靠近芯片"的位置，就是 V4.3 那轮踩过的坑（一度被迫
    推到 30mm 外，最后还是搬了回来）。用布线换位置是划算的，反过来不划算。

两类要拆的铜：
  ① 与新件焊盘几何冲突的段/孔（间距按最严的 0.15mm 圈，宁可多拆一点也不留隐患）
  ② C21/C36/C48/R11 从 B.Cu 翻到 F.Cu 之后，原来在背面接它们的走线——那些线现在
     连着空气，是 dangling。不清掉的话 DRC 会一直红，而且看起来像"还没布完"。

幂等：每次都从 seed 脚本产出的候选板重新算，拆完即存。

用法：KiCad 自带 python3 tools/eco_v39_ripup_conflicts.py
"""
import collections
import json
import re
import subprocess
import sys
from pathlib import Path

import pcbnew

KICAD_CLI = str(Path.home() /
                "Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli")
ROOT = Path(__file__).resolve().parents[1]
CANDIDATE = ROOT / "internal" / "work" / "v3.9" / "expansion-board-v3-v39-candidate.kicad_pcb"

AUDIT = ("R47", "R50", "R51", "R52", "R53", "R54",
         "C81", "C82", "C83", "C84", "C85", "C86", "D4", "D5")
MOVED = ("C21", "C36", "C48", "R11")
NEW = set(AUDIT) | set(MOVED)

# 圈选半径用最严 netclass 的 0.15mm。DRC 真正判的是逐网络的 clearance，
# 这里统一从严：多拆一段线的成本，远低于漏一处短路到打样之后才发现。
CLEARANCE = int(0.15 * 1e6)


def conflicting_copper(board):
    """返回与新件焊盘冲突的走线/过孔对象。"""
    tracks = list(board.GetTracks())
    doomed = {}
    per_ref = collections.defaultdict(set)
    for footprint in board.GetFootprints():
        reference = footprint.GetReference()
        if reference not in NEW:
            continue
        for pad in footprint.Pads():
            pad_net = pad.GetNetname()
            for layer in pad.GetLayerSet().CuStack():
                try:
                    shape = pad.GetEffectiveShape(layer)   # KiCad 10 要求带层参数
                except Exception:
                    continue
                for track in tracks:
                    if track.GetNetname() == pad_net or not track.IsOnLayer(layer):
                        continue
                    try:
                        if shape.Collide(track.GetEffectiveShape(layer), CLEARANCE):
                            doomed[track.m_Uuid.AsString()] = track
                            per_ref[reference].add(track.GetNetname())
                    except Exception:
                        continue
    return doomed, per_ref


def drc_dangling_uuids(pcb: Path) -> set:
    """跑一次 DRC，取回它认定悬空的走线/过孔 uuid。

    **不要用 `TestTrackEndpointDangling()`**。实测在一块布线完整、DRC 报 0 违规的板上，
    该函数带 True 判出 2 段、带 False 判出 8 段悬空——两个参数都在误报，拿它当删除依据
    会吃掉正常的线。DRC 才是权威判据，而且报告里带 uuid，可以精确定位。
    """
    report = pcb.parent / f"{pcb.stem}-dangling.json"
    subprocess.run(
        [KICAD_CLI, "pcb", "drc", "--refill-zones", "--severity-all",
         "--format", "json", "-o", str(report), str(pcb)],
        capture_output=True, check=True,
    )
    data = json.loads(report.read_text(encoding="utf-8"))
    report.unlink()
    return {
        item["uuid"]
        for violation in data.get("violations", [])
        if violation["type"] in ("track_dangling", "via_dangling")
        for item in violation.get("items", [])
        if "uuid" in item
    }


def clean_once() -> int:
    """删掉当前 DRC 认定的所有断头，返回删除个数。供父进程循环调用。"""
    doomed_uuids = drc_dangling_uuids(CANDIDATE)
    if not doomed_uuids:
        print("CLEANED 0")
        return 0
    board = pcbnew.LoadBoard(str(CANDIDATE))
    removed = 0
    for track in list(board.GetTracks()):
        if track.m_Uuid.AsString() in doomed_uuids:
            board.Remove(track)
            removed += 1
    if removed:
        board.BuildConnectivity()
        board.Save(str(CANDIDATE))
    print(f"CLEANED {removed}")
    return removed


def main() -> int:
    board = pcbnew.LoadBoard(str(CANDIDATE))
    before_t = sum(1 for t in board.GetTracks() if t.Type() == pcbnew.PCB_TRACE_T)
    before_v = sum(1 for t in board.GetTracks() if t.Type() == pcbnew.PCB_VIA_T)

    doomed, per_ref = conflicting_copper(board)
    seg = sum(1 for t in doomed.values() if t.Type() == pcbnew.PCB_TRACE_T)
    via = sum(1 for t in doomed.values() if t.Type() == pcbnew.PCB_VIA_T)
    nets = sorted({t.GetNetname() for t in doomed.values()})
    for track in doomed.values():
        board.Remove(track)
    board.BuildConnectivity()
    board.Save(str(CANDIDATE))

    # 拆掉冲突段会让同一条链上余下的段变成断头，断头删掉又可能露出新的断头，
    # 所以要迭代到收敛。上限 12 轮纯粹是防死循环。
    # ⚠️ 每轮清理都起一个**独立子进程**。pcbnew 的 BOARD 对象在 `Save()` 之后，
    # Python 侧引用就废了（再调 GetTracks 会拿到裸 SwigPyObject，报 not iterable），
    # 重新 LoadBoard 同一个文件也救不回来。一轮一进程最省心。
    stray_total = 0
    rounds = 0
    for rounds in range(1, 13):
        done = subprocess.run(
            [sys.executable, __file__, "--clean-once"],
            capture_output=True, text=True,
        )
        # pcbnew 会往 stdout 吐 memory-leak 之类的噪音，不能直接取最后一行
        marker = re.search(r"CLEANED (\d+)", done.stdout)
        removed = int(marker.group(1)) if marker else 0
        stray_total += removed
        if removed == 0:
            break
    else:
        raise SystemExit("断头清理 12 轮仍未收敛，停下来人工看")

    # board 对象在上面 Save() 之后已失效，末尾统计改成读盘
    final = CANDIDATE.read_text(encoding="utf-8")
    # KiCad 10 写的是 "(segment\n\t\t(start ...)"，token 后面直接换行没有空格，
    # 所以模式里不能带尾随空格，用 \b 收边。
    after_t = len(re.findall(r"\(segment\b", final))
    after_v = len(re.findall(r"\(via\b", final))
    print(f"① 冲突铜: 拆走线 {seg} 段 / 过孔 {via} 个，涉及 {len(nets)} 个网络")
    for reference in sorted(per_ref, key=lambda r: -len(per_ref[r])):
        print(f"     {reference:<5} {sorted(per_ref[reference])}")
    print(f"② 拆完露出的断头（DRC 判据，{rounds} 轮收敛）: {stray_total} 个对象")
    print(f"走线 {before_t} → {after_t}   过孔 {before_v} → {after_v}")
    print(f"待重布网络: {nets}")
    return 0


if __name__ == "__main__":
    if "--clean-once" in sys.argv:
        clean_once()
        sys.exit(0)
    sys.exit(main())
