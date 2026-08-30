#!/usr/bin/env python3
"""v3/v4 关键器件引脚、外围网络与最终 PCB 的结构回归测试。"""

from __future__ import annotations

from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
ALL_BOARDS = ("expansion-board-v3", "expansion-board-v4")
AVAILABLE_BOARDS = tuple(
    board for board in ALL_BOARDS
    if (ROOT / "hardware" / board / "kicad" / f"{board}.kicad_sch").is_file()
)
REQUESTED_BOARDS = tuple(filter(None, os.environ.get("PK_TEST_BOARDS", "").split(",")))
BOARDS = REQUESTED_BOARDS or AVAILABLE_BOARDS


def find_kicad_cli() -> str:
    candidates = (
        os.environ.get("KICAD_CLI"),
        shutil.which("kicad-cli"),
        "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli",
        str(Path.home() / "Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"),
    )
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    raise RuntimeError("找不到 kicad-cli")


def export_schematic(board: str) -> tuple[dict[tuple[str, str], str], dict[str, str]]:
    schematic = ROOT / "hardware" / board / "kicad" / f"{board}.kicad_sch"
    with tempfile.TemporaryDirectory(prefix=f"{board}-component-contract-") as tmp:
        output = Path(tmp) / "netlist.xml"
        subprocess.run(
            [find_kicad_cli(), "sch", "export", "netlist", "--format", "kicadxml",
             "-o", str(output), str(schematic)],
            check=True,
            capture_output=True,
            text=True,
        )
        root = ET.parse(output).getroot()

    pin_nets: dict[tuple[str, str], str] = {}
    for net in root.findall("./nets/net"):
        for node in net.findall("node"):
            pin_nets[(node.attrib["ref"], node.attrib["pin"])] = net.attrib["name"]
    values = {
        comp.attrib["ref"]: comp.findtext("value", "")
        for comp in root.findall("./components/comp")
    }
    return pin_nets, values


def extract_sexpr(text: str, start: int) -> str:
    depth = 0
    quoted = False
    escaped = False
    for index in range(start, len(text)):
        char = text[index]
        if quoted:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
        elif char == '"':
            quoted = True
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    raise ValueError(f"未闭合的 S-expression，起点 {start}")


def pcb_footprint(board: str, reference: str) -> str:
    pcb = ROOT / "hardware" / board / "kicad" / f"{board}.kicad_pcb"
    text = pcb.read_text(encoding="utf-8")
    marker_at = text.index(f'(property "Reference" "{reference}"')
    footprint_at = text.rfind("\n\t(footprint ", 0, marker_at) + 2
    return extract_sexpr(text, footprint_at)


def pcb_pad_nets(board: str, reference: str) -> dict[str, str]:
    footprint = pcb_footprint(board, reference)
    result = {}
    for match in re.finditer(r'\(pad "([^"]+)" ', footprint):
        pad = extract_sexpr(footprint, match.start())
        net = re.search(r'\(net "([^"]+)"\)', pad)
        if net:
            result[match.group(1)] = net.group(1)
    return result


def pcb_rotation(board: str, reference: str) -> float:
    footprint = pcb_footprint(board, reference)
    match = re.search(r'\n\t\t\(at [-0-9.]+ [-0-9.]+(?: ([-0-9.]+))?\)', footprint)
    if not match:
        raise AssertionError(f"{board} {reference} 缺少 footprint at")
    return float(match.group(1) or 0.0) % 360.0


class ComponentContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        unknown = set(BOARDS) - set(ALL_BOARDS)
        missing = set(BOARDS) - set(AVAILABLE_BOARDS)
        if unknown or missing:
            raise AssertionError(
                f"PK_TEST_BOARDS 无效：unknown={sorted(unknown)}, missing={sorted(missing)}"
            )
        cls.schematic = {board: export_schematic(board) for board in BOARDS}

    def test_qpl9547_bias_and_supply_network(self):
        expected = {
            ("U11", "1"): "LNA1_VBIAS",
            ("U11", "6"): "GND",
            ("U11", "7"): "LNA1_OUT",
            ("R11", "1"): "3V3_RF",
            ("R11", "2"): "LNA1_VBIAS",
            ("C21", "1"): "LNA1_VBIAS",
            ("C21", "2"): "GND",
            ("L1", "1"): "3V3_RF",
            ("L1", "2"): "LNA1_OUT",
            ("C36", "1"): "3V3_RF",
            ("C36", "2"): "GND",
            ("C48", "1"): "3V3_RF",
            ("C48", "2"): "GND",
        }
        expected_values = {
            "R11": "3.32k",
            "C21": "100pF",
            "L1": "18nH 0402CS-18NXGRW",
            "C36": "100pF",
            "C48": "1uF",
        }
        for board, (pin_nets, values) in self.schematic.items():
            with self.subTest(board=board):
                self.assertEqual({key: pin_nets.get(key) for key in expected}, expected)
                self.assertEqual({ref: values.get(ref) for ref in expected_values}, expected_values)
                for ref in ("U11", "R11", "C21", "L1", "C36", "C48"):
                    wanted = {
                        pad: net for (component, pad), net in expected.items()
                        if component == ref
                    }
                    actual = pcb_pad_nets(board, ref)
                    self.assertEqual({pad: actual.get(pad) for pad in wanted}, wanted)

    def test_v4_power_corrections(self):
        if "expansion-board-v4" not in BOARDS:
            self.skipTest("当前分支不维护 v4")
        pin_nets, values = self.schematic["expansion-board-v4"]
        self.assertEqual(pin_nets[("U18", "6")], "USB_CC2")
        self.assertEqual(pin_nets[("U18", "7")], "USB_CC1")
        self.assertEqual(values["C72"], "10uF")
        self.assertEqual(pin_nets[("C72", "1")], "CHG_PMID")
        self.assertEqual(pin_nets[("C72", "2")], "GND")
        self.assertEqual(pcb_pad_nets("expansion-board-v4", "U18")["6"], "USB_CC2")
        self.assertEqual(pcb_pad_nets("expansion-board-v4", "U18")["7"], "USB_CC1")

    def test_qmc5883p_and_ta0970a_pin_contracts(self):
        qmc = {
            ("U6", "1"): "I2C_SCL", ("U6", "2"): "3V3_DIG",
            ("U6", "9"): "GND", ("U6", "10"): "QMC_C1",
            ("U6", "11"): "GND", ("U6", "16"): "I2C_SDA",
            ("C15", "1"): "QMC_C1", ("C15", "2"): "GND",
        }
        saw = {
            ("FL1", "A"): "GND", ("FL1", "B"): "SAW1_IN",
            ("FL1", "C"): "GND", ("FL1", "D"): "GND",
            ("FL1", "E"): "SAW1_OUT", ("FL1", "F"): "GND",
            ("FL2", "A"): "GND", ("FL2", "B"): "SAW2_IN",
            ("FL2", "C"): "GND", ("FL2", "D"): "GND",
            ("FL2", "E"): "SAW2_OUT", ("FL2", "F"): "GND",
        }
        for board, (pin_nets, values) in self.schematic.items():
            with self.subTest(board=board):
                expected = qmc | saw
                self.assertEqual({key: pin_nets.get(key) for key in expected}, expected)
                self.assertEqual(values["C15"], "4.7uF")
                self.assertEqual(set(pcb_pad_nets(board, "U6")), {str(i) for i in range(1, 17)})
                self.assertEqual(set(pcb_pad_nets(board, "FL1")), set("ABCDEF"))
                self.assertEqual(set(pcb_pad_nets(board, "FL2")), set("ABCDEF"))

    def test_bno085_orientation_is_explicit_per_board(self):
        # V3 保留已闭环布线的 0°，V4 为 90°；固件必须按板型转换坐标系。
        expected = {"expansion-board-v3": 0.0, "expansion-board-v4": 90.0}
        for board in BOARDS:
            with self.subTest(board=board):
                self.assertEqual(pcb_rotation(board, "U4"), expected[board])

    def test_v4_generated_bom_keeps_variant_rule(self):
        if "expansion-board-v4" not in BOARDS:
            self.skipTest("当前分支不维护 v4")
        bom = ROOT / "hardware" / "expansion-board-v4" / "BOM_PURCHASE-zh_CN.md"
        text = bom.read_text(encoding="utf-8")
        row = next(line for line in text.splitlines() if "R7,R8" in line)
        self.assertIn("带电源版不贴", row)
        self.assertIn("不带电源版必贴", row)
        self.assertNotIn("两颗都要", row)


if __name__ == "__main__":
    unittest.main()
