#!/usr/bin/env python3
"""验证 v3/v4 IFA 生成器输出同一套实测定型几何。"""

from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent


class IfaGenerationTest(unittest.TestCase):
    """冻结 2026-09-04 逐刀实测定型的 50.0 mm 外包络。

    ⚠️ **不要改回 49.0 mm。**那个值是 2026-09-03 记录错误：它被当成"实测定型"
    回灌进两版生成器、封装库和 PCB，但 V4.0 实板从 53.5 mm 逐刀切到
    **50.0 mm 就停了，从来没到过 49.0**。

    50.0 mm 的依据是同一块板的逐刀序列（装盒+电池：1082.5 MHz、SWR 1.09、
    45+j0.5Ω）。同序列还给出斜率 **24–25 MHz/mm**，据此 49.0 mm 会落到
    约 1107 MHz——正是当时复测到 1107.5 MHz 的原因。

    详见 `expansion-board-v3/internal/IFA_ANTENNA.md` §7。
    """

    GENERATORS = (
        ("v3", ROOT / "expansion-board-v3" / "tools" / "gen_ifa_footprint.py"),
        ("v4", ROOT / "expansion-board-v4" / "internal" / "tools" / "gen_ifa_footprint.py"),
    )

    def test_generators_emit_measured_50mm_outer_envelope(self):
        for version, source in self.GENERATORS:
            with self.subTest(version=version), tempfile.TemporaryDirectory() as tmp:
                project = Path(tmp) / f"expansion-board-{version}"
                tools = project / ("tools" if version == "v3" else "internal/tools")
                tools.mkdir(parents=True)
                copied = tools / source.name
                shutil.copy2(source, copied)

                result = subprocess.run(
                    [sys.executable, str(copied)],
                    cwd=tools,
                    check=True,
                    capture_output=True,
                    text=True,
                )

                geom = runpy.run_path(str(tools / "ifa_geom.py"))
                bbox = geom["BBOX_LOCAL"]
                self.assertAlmostEqual(bbox[2] - bbox[0], 50.0, places=4)
                self.assertAlmostEqual(geom["ARM_SPAN"], 48.5, places=4)
                self.assertIn("铜箔包络 50.000 × 7.500 mm", result.stdout)


if __name__ == "__main__":
    unittest.main()
