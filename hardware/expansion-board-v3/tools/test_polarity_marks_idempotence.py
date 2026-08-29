#!/usr/bin/env python3
"""实际 PCB 上的二极管极性标记幂等回归测试。"""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import pcbnew


TOOLS = Path(__file__).resolve().parent
PROJECT = TOOLS.parent
SOURCE_BOARD = PROJECT / "kicad" / "expansion-board-v3.kicad_pcb"


def board_marker_records(path):
    board = pcbnew.LoadBoard(str(path))
    records = []
    for drawing in board.GetDrawings():
        if (drawing.GetClass() != "PCB_SHAPE"
                or drawing.GetLayer() != pcbnew.F_SilkS
                or drawing.GetShape() != pcbnew.SHAPE_T_SEGMENT):
            continue
        start, end = drawing.GetStart(), drawing.GetEnd()
        records.append((
            round(pcbnew.ToMM(start.x), 3), round(pcbnew.ToMM(start.y), 3),
            round(pcbnew.ToMM(end.x), 3), round(pcbnew.ToMM(end.y), 3),
            round(pcbnew.ToMM(drawing.GetWidth()), 3),
        ))
    return sorted(records)


class PolarityMarksIdempotenceTest(unittest.TestCase):
    def test_all_diodes_are_audited_and_repeated_runs_do_not_stack_marks(self):
        with tempfile.TemporaryDirectory(prefix="v3-polarity-") as temp:
            board_dir = Path(temp)
            board_path = board_dir / SOURCE_BOARD.name
            shutil.copy2(SOURCE_BOARD, board_path)
            env = os.environ.copy()
            env["PK_BOARD_DIR"] = str(board_dir)
            command = [sys.executable, str(TOOLS / "gen_polarity_marks.py")]

            first = subprocess.run(command, env=env, capture_output=True, text=True)
            self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
            self.assertIn("D1", first.stdout)
            self.assertIn("D2", first.stdout)
            self.assertIn("D3", first.stdout)
            records = board_marker_records(board_path)
            self.assertEqual(len(records), 2, records)

            second = subprocess.run(command, env=env, capture_output=True, text=True)
            self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
            self.assertEqual(board_marker_records(board_path), records)


if __name__ == "__main__":
    unittest.main()
