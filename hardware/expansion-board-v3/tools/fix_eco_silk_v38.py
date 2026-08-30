#!/usr/bin/env python3
"""处理 V3.8 新增 QPL9547 无源件附近的丝印净空。"""

import os

import pcbnew


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(ROOT, "kicad")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")

board = pcbnew.LoadBoard(PCB)
for ref in ("C31", "C21", "C36", "C48", "R11"):
    footprint = board.FindFootprintByReference(ref)
    assert footprint, f"找不到 {ref}"
    footprint.Reference().SetLayer(pcbnew.F_Fab if not footprint.IsFlipped()
                                   else pcbnew.B_Fab)
board.Save(PCB)
print("V3.8 丝印 ECO：C31/C21/C36/C48/R11 位号移到装配层")
