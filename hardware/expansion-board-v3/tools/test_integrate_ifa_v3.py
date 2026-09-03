#!/usr/bin/env python3
"""V3 IFA局部回灌必须保留正式板布线，只替换ANT1并升级板面版本。"""

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

import pcbnew


TOOLS = Path(__file__).resolve().parent
ROOT = TOOLS.parent
PCB_NAME = "expansion-board-v3.kicad_pcb"


class IntegrateIfaV3Test(unittest.TestCase):
    def test_preserves_routes_and_writes_measured_geometry(self):
        with tempfile.TemporaryDirectory() as td:
            board_dir = Path(td) / "kicad"
            board_dir.mkdir()
            shutil.copy2(ROOT / "kicad" / PCB_NAME, board_dir / PCB_NAME)
            shutil.copytree(ROOT / "kicad" / "expansion-board-v3.pretty",
                            board_dir / "expansion-board-v3.pretty")

            before = pcbnew.LoadBoard(str(board_dir / PCB_NAME))
            tracks_before = len(before.GetTracks())

            env = os.environ.copy()
            env["PK_BOARD_DIR"] = str(board_dir)
            subprocess.run([sys.executable, str(TOOLS / "integrate_ifa_v3.py")],
                           check=True, env=env, capture_output=True, text=True)

            board = pcbnew.LoadBoard(str(board_dir / PCB_NAME))
            self.assertEqual(len(board.GetTracks()), tracks_before)
            ant = next(f for f in board.GetFootprints() if f.GetReference() == "ANT1")
            boxes = [p.GetBoundingBox() for p in ant.Pads()]
            left = min(pcbnew.ToMM(b.GetLeft()) for b in boxes)
            right = max(pcbnew.ToMM(b.GetRight()) for b in boxes)
            self.assertAlmostEqual(right - left, 50.0, places=3)
            versions = [d.GetText() for d in board.GetDrawings()
                        if d.GetClass() == "PCB_TEXT" and "V3." in d.GetText()]
            self.assertEqual(versions, ["V3.10   2026-09"])


if __name__ == "__main__":
    unittest.main()
