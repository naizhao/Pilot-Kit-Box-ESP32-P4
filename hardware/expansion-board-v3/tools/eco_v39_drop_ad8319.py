#!/usr/bin/env python3
"""阶段 B：把 AD8319 实验支路从候选 PCB 上删掉。

删的范围严格限定在三样：封装 U13、封装 R20、网络 DET_TADJ 的全部铜。
DET_TADJ 是 R20 与 U13.6 之间的温度补偿网，除这两个器件外没有第三方连接，
所以整网可以直接清掉。

**不删** R19 / C34 / C35 / R54 / C37 / C38 —— 那些是保留下来的 AD8313 通路。
C34 原本接 U13.1(INHI)，现在改接 U14 的 INHI，走的是同一个 DET_IN 网络，
所以 DET_IN 上的铜一段都不能动。

删完还要清一遍断头：U13/R20 的焊盘一没，原来连过去的线就吊在半空。
这里复用 `eco_v39_ripup_conflicts.py --clean-once`，判据同样交给 DRC，
不用 `TestTrackEndpointDangling()`（它在干净板上都会误报）。

用法：KiCad 自带 python3 tools/eco_v39_drop_ad8319.py
"""
import subprocess
import sys
from pathlib import Path

import pcbnew

ROOT = Path(__file__).resolve().parents[1]
CANDIDATE = ROOT / "internal" / "work" / "v3.9" / "expansion-board-v3-v39-candidate.kicad_pcb"
CLEANER = Path(__file__).resolve().parent / "eco_v39_ripup_conflicts.py"

DROP_REFS = ("U13", "R20")
DROP_NETS = ("DET_TADJ",)
# 这几个必须活下来。AD8319 一删，R21 从"二选一跳线"变成唯一通路，
# 要是连它一起删了，检波器输出就到不了 RP2040 的 ADC。
KEEP_REFS = ("R19", "C34", "C35", "R54", "C37", "C38", "U14", "R21")


def main() -> int:
    board = pcbnew.LoadBoard(str(CANDIDATE))
    refs = {fp.GetReference(): fp for fp in board.GetFootprints()}

    for reference in KEEP_REFS:
        if reference not in refs:
            raise SystemExit(f"{reference} 本来就不在板上，先查清楚再删 U13")

    dropped_fp = []
    for reference in DROP_REFS:
        if reference in refs:
            board.Remove(refs[reference])
            dropped_fp.append(reference)

    dropped_cu = 0
    for track in list(board.GetTracks()):
        if track.GetNetname() in DROP_NETS:
            board.Remove(track)
            dropped_cu += 1

    board.BuildConnectivity()
    board.Save(str(CANDIDATE))

    stray = 0
    for _round in range(12):
        done = subprocess.run(
            [sys.executable, str(CLEANER), "--clean-once"],
            capture_output=True, text=True,
        )
        import re
        marker = re.search(r"CLEANED (\d+)", done.stdout)
        removed = int(marker.group(1)) if marker else 0
        stray += removed
        if removed == 0:
            break

    # 验证只能读文本：pcbnew 的 BOARD 在 Save() 之后 Python 引用就废了，
    # 重新 LoadBoard 同一个文件拿到的是裸 SwigPyObject（连 GetFootprints 都没有）。
    import re
    final = set(re.findall(r'\(property "Reference" "([^"]*)"',
                           CANDIDATE.read_text(encoding="utf-8")))
    for reference in DROP_REFS:
        if reference in final:
            raise SystemExit(f"{reference} 没删掉")
    for reference in KEEP_REFS:
        if reference not in final:
            raise SystemExit(f"{reference} 被误删了——它属于保留的 AD8313 通路")

    print(f"删除封装 {dropped_fp}")
    print(f"删除 DET_TADJ 铜 {dropped_cu} 个对象 / 随后清理断头 {stray} 个")
    print(f"保留件全部在板: {list(KEEP_REFS)}")
    print(f"候选板封装数 {len(final)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
