#!/usr/bin/env python3
"""ROUTES.json 贴回链的真实 KiCad 幂等回归测试。"""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import pcbnew

from route_cleanup import cleanup_records
from route_quality import normalize_record


TOOLS = Path(__file__).resolve().parent
PROJECT = TOOLS.parent
SOURCE_BOARD = PROJECT / "kicad" / "expansion-board-v3.kicad_pcb"
SNAPSHOT = TOOLS / "ROUTES.json"


def board_counts(path):
    board = pcbnew.LoadBoard(str(path))
    tracks = list(board.GetTracks())
    return (
        sum(not isinstance(item, pcbnew.PCB_VIA) for item in tracks),
        sum(isinstance(item, pcbnew.PCB_VIA) for item in tracks),
    )


def board_records(path):
    board = pcbnew.LoadBoard(str(path))
    layer_names = {board.GetLayerID(name): name for name in
                   ("F.Cu", "In1.Cu", "In2.Cu", "In3.Cu", "In4.Cu", "B.Cu")}
    tracks, vias = [], []
    for item in board.GetTracks():
        if isinstance(item, pcbnew.PCB_VIA):
            position = item.GetPosition()
            vias.append((
                round(pcbnew.ToMM(position.x), 4),
                round(pcbnew.ToMM(position.y), 4),
                round(pcbnew.ToMM(item.GetWidth(pcbnew.F_Cu)), 4),
                round(pcbnew.ToMM(item.GetDrill()), 4),
            ))
        else:
            start, end = item.GetStart(), item.GetEnd()
            a = (round(pcbnew.ToMM(start.x), 4), round(pcbnew.ToMM(start.y), 4))
            b = (round(pcbnew.ToMM(end.x), 4), round(pcbnew.ToMM(end.y), 4))
            if b < a:
                a, b = b, a
            tracks.append((
                item.GetNetname(), layer_names[item.GetLayer()], *a, *b,
                round(pcbnew.ToMM(item.GetWidth()), 4),
            ))
    return sorted(tracks), sorted(vias)


class ImportRoutesIdempotenceTest(unittest.TestCase):
    def test_importing_twice_keeps_the_first_import_exactly_stable(self):
        with SNAPSHOT.open(encoding="utf-8") as handle:
            data = json.load(handle)
        records = [normalize_record(net, layer, (x1, y1), (x2, y2), width)
                   for net, layer, x1, y1, x2, y2, width in data["tracks"]]
        anchors = {(net, layer, (round(x, 4), round(y, 4)))
                   for net, x, y, _diameter, _drill in data["vias"]
                   for layer in ("F.Cu", "In1.Cu", "In2.Cu", "In3.Cu", "In4.Cu", "B.Cu")}
        cleaned = cleanup_records(records, anchors)
        data["tracks"] = [[item.net, item.layer, *item.start, *item.end, item.width]
                          for item in cleaned]
        expected = (len(data["tracks"]), len(data["vias"]))

        with tempfile.TemporaryDirectory(prefix="v3-import-routes-") as temp:
            temp_dir = Path(temp)
            board_dir = temp_dir / "kicad"
            build_dir = temp_dir / "build"
            board_dir.mkdir()
            build_dir.mkdir()
            board_path = board_dir / SOURCE_BOARD.name
            shutil.copy2(PROJECT / "kicad" / "expansion-board-v3.kicad_pro", board_dir)
            shutil.copy2(PROJECT / "build" / "expansion.net.xml", build_dir)
            env = os.environ.copy()
            env["PK_BOARD_DIR"] = str(board_dir)
            env["PK_BUILD_DIR"] = str(build_dir)
            generated = subprocess.run(
                [sys.executable, str(TOOLS / "gen_pcb.py")],
                env=env,
                capture_output=True,
                text=True,
            )
            self.assertEqual(generated.returncode, 0, generated.stdout + generated.stderr)
            ifa = subprocess.run(
                [sys.executable, str(TOOLS / "route_ifa_feed.py")],
                env=env,
                capture_output=True,
                text=True,
            )
            self.assertEqual(ifa.returncode, 0, ifa.stdout + ifa.stderr)
            stress_snapshot = temp_dir / "ROUTES.stress.json"
            stress_snapshot.write_text(json.dumps(data), encoding="utf-8")
            command = [sys.executable, str(TOOLS / "import_routes.py"), str(stress_snapshot)]

            result = subprocess.run(command, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            first_counts = board_counts(board_path)
            first_records = board_records(board_path)
            # V3.8 的 QPL9547 拓扑与冻结于旧原理图的 ROUTES.json 不同；导入器会
            # 有意丢弃不再属于当前网络的旧端点段，因此不能再断言“等于原始快照”。
            # 幂等契约是：第一次完成拓扑清理后，第二次不得再变化。
            self.assertLessEqual(first_counts[0], expected[0])
            self.assertLessEqual(first_counts[1], expected[1])

            result = subprocess.run(command, env=env, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(board_counts(board_path), first_counts)
            self.assertEqual(board_records(board_path), first_records)


if __name__ == "__main__":
    unittest.main()
