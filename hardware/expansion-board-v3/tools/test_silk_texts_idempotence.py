#!/usr/bin/env python3
"""板级文字/丝印图形快照的真实 KiCad 幂等测试。"""

import json
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


def silk_records(path):
    board = pcbnew.LoadBoard(str(path))
    texts = []
    graphics = []
    for drawing in board.GetDrawings():
        layer = board.GetLayerName(drawing.GetLayer())
        if drawing.GetClass() in ("PCB_TEXT", "PCB_TEXTBOX"):
            if layer in ("F.Silkscreen", "B.Silkscreen", "F.Fab", "B.Fab",
                         "Cmts.User", "Dwgs.User"):
                position = drawing.GetPosition()
                texts.append((drawing.GetText(), layer,
                              round(pcbnew.ToMM(position.x), 4),
                              round(pcbnew.ToMM(position.y), 4)))
        elif drawing.GetClass() == "PCB_SHAPE" and layer in ("F.Silkscreen", "B.Silkscreen"):
            start, end = drawing.GetStart(), drawing.GetEnd()
            graphics.append((layer, int(drawing.GetShape()),
                             round(pcbnew.ToMM(start.x), 4),
                             round(pcbnew.ToMM(start.y), 4),
                             round(pcbnew.ToMM(end.x), 4),
                             round(pcbnew.ToMM(end.y), 4),
                             round(pcbnew.ToMM(drawing.GetWidth()), 4)))
    return sorted(texts), sorted(graphics)


class SilkTextsIdempotenceTest(unittest.TestCase):
    def test_export_and_two_imports_preserve_board_text_and_silk_segments(self):
        expected = silk_records(SOURCE_BOARD)
        self.assertGreater(len(expected[1]), 0, "源板应至少包含极性标记丝印线")

        with tempfile.TemporaryDirectory(prefix="v3-silk-snapshot-") as temp:
            temp_dir = Path(temp)
            board_dir = temp_dir / "kicad"
            board_dir.mkdir()
            board_path = board_dir / SOURCE_BOARD.name
            snapshot = temp_dir / "SILK.json"
            shutil.copy2(SOURCE_BOARD, board_path)
            env = os.environ.copy()
            env["PK_BOARD_DIR"] = str(board_dir)
            env["PK_SILK_SNAPSHOT"] = str(snapshot)

            command = [sys.executable, str(TOOLS / "silk_texts.py")]
            exported = subprocess.run(command + ["export"], env=env,
                                      capture_output=True, text=True)
            self.assertEqual(exported.returncode, 0, exported.stdout + exported.stderr)
            data = json.loads(snapshot.read_text(encoding="utf-8"))
            self.assertEqual(len(data["graphics"]), len(expected[1]))

            for _ in range(2):
                imported = subprocess.run(command + ["import"], env=env,
                                          capture_output=True, text=True)
                self.assertEqual(imported.returncode, 0, imported.stdout + imported.stderr)
                self.assertEqual(silk_records(board_path), expected)


if __name__ == "__main__":
    unittest.main()
