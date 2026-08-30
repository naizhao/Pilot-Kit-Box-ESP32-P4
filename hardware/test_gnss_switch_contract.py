#!/usr/bin/env python3
"""v3/v4 GNSS RF 选择与有源天线 bias 的结构回归测试。

依据 AS179-92LF 与实际装配 XA17-G4K 的真值表：

* V1=Low、V2=High：J1-J2 导通，因此外接 J2 的低有效 PMOS Q4 gate 必须接 V1。
* V1=High、V2=Low：J1-J3 导通，因此内置 J3 的低有效 PMOS Q5 gate 必须接 V2。

测试通过 KiCad CLI 从最终层级原理图临时导出 XML 网表，不读取生成脚本字符串，
避免出现“脚本改了但忘记重新生成原理图”的假通过。

运行：python3 hardware/test_gnss_switch_contract.py
"""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
SUPPORTED_BOARDS = ("expansion-board-v3", "expansion-board-v4")
BOARDS = tuple(
    board
    for board in SUPPORTED_BOARDS
    if (ROOT / "hardware" / board / "kicad" / f"{board}.kicad_sch").is_file()
)
if not BOARDS:
    raise RuntimeError("当前分支没有 v3/v4 层级原理图")


def find_kicad_cli() -> str:
    configured = os.environ.get("KICAD_CLI")
    candidates = (
        configured,
        shutil.which("kicad-cli"),
        "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli",
        str(Path.home() / "Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"),
    )
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    raise RuntimeError("找不到 kicad-cli；可用 KICAD_CLI=/absolute/path 指定")


def export_pin_nets(board: str) -> dict[tuple[str, str], str]:
    schematic = ROOT / "hardware" / board / "kicad" / f"{board}.kicad_sch"
    with tempfile.TemporaryDirectory(prefix=f"{board}-gnss-contract-") as tmp:
        output = Path(tmp) / "netlist.xml"
        subprocess.run(
            [
                find_kicad_cli(),
                "sch",
                "export",
                "netlist",
                "--format",
                "kicadxml",
                "-o",
                str(output),
                str(schematic),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        root = ET.parse(output).getroot()

    result: dict[tuple[str, str], str] = {}
    for net in root.findall("./nets/net"):
        name = net.attrib["name"]
        for node in net.findall("node"):
            result[(node.attrib["ref"], node.attrib["pin"])] = name
    return result


class GnssSwitchContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pin_nets = {board: export_pin_nets(board) for board in BOARDS}

    def test_actual_spdt_pin_contract(self):
        expected = {
            ("U17", "1"): "SW2_J3",
            ("U17", "3"): "SW2_J2",
            ("U17", "4"): "ANT_SEL_GNSS_A",
            ("U17", "5"): "SW2_J1",
            ("U17", "6"): "ANT_SEL_GNSS_B",
        }
        for board, pin_nets in self.pin_nets.items():
            with self.subTest(board=board):
                self.assertEqual(
                    {key: pin_nets[key] for key in expected},
                    expected,
                )

    def test_bias_gate_tracks_selected_rf_path(self):
        expected = {
            ("Q4", "1"): "ANT_SEL_GNSS_A",  # 外接 J2：V1 Low 时导通
            ("Q5", "1"): "ANT_SEL_GNSS_B",  # 内置 J3：V2 Low 时导通
            ("Q4", "3"): "GNSS_EXT_FUSE",
            ("Q5", "3"): "GNSS_INT_FUSE",
        }
        for board, pin_nets in self.pin_nets.items():
            with self.subTest(board=board):
                self.assertEqual(
                    {key: pin_nets[key] for key in expected},
                    expected,
                )


if __name__ == "__main__":
    unittest.main()
