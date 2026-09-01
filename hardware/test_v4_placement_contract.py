#!/usr/bin/env python3
"""v4 PCB 的结构合同：功能丝印、命名 RF zone、板级图形数量和 B 面调试焊盘列。

默认检查 `hardware/expansion-board-v4/kicad/` 下的板。`PK_V4_PCB` 可以指向另一份
PCB（例如发布前的候选板），取绝对路径或相对仓库根的路径。
"""

from __future__ import annotations

from pathlib import Path
import os
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PCB = ROOT / "hardware" / "expansion-board-v4" / "kicad" / "expansion-board-v4.kicad_pcb"
PCB = Path(os.environ.get("PK_V4_PCB") or DEFAULT_PCB)
if not PCB.is_absolute():
    PCB = ROOT / PCB

# V4.2 定稿保留的功能丝印；版权/网址/标题行不在合同内，允许改文案。
REQUIRED_TEXTS = {
    "+", "-", "1090 EXT", "1090 IFA", "1090MHz IFA ANT",
    "978 UAT", "BATT", "GNSS EXT", "GNSS INT", "NO-PWR VER", "USB",
}
REQUIRED_ZONES = {
    "ANT1090_open_end_keepout", "ANT1090_short_end_keepout",
    "ifa_rf50_corridor_ANT1090_IFA", "ifa_rf50_corridor_IFA_MATCH",
}
# V4.2 定稿的板级图形基线；调试列的 9 个 B.SilkS 标签是 V4.3 唯一允许的新增，
# 所以总数写成「基线 + 标签数」，多一个少一个都要失败。
FINALIZED_GRAPHICS_COUNT = 26
DEBUG_LABELS = ("RST", "BOOT", "TP1", "TP2", "TP3", "TP4", "TP5", "TP6", "TP7")
BOARD_GRAPHICS_COUNT = FINALIZED_GRAPHICS_COUNT + len(DEBUG_LABELS)
# B 面左边缘同一竖列：SW1、SW2 相邻成组排在 TP 之前，SW1 在 SW2 上方。
# 两个 SW 之间用 3.60mm（SolderJumper courtyard 跨 3.39mm，2.50mm 会重叠），
# TP 之间仍是 2.50mm。
DEBUG_X = 51.85
DEBUG_Y = {
    "SW1": 77.45, "SW2": 81.05, "TP1": 86.05, "TP2": 88.55,
    "TP3": 91.05, "TP4": 93.55, "TP5": 96.05, "TP6": 98.55,
    "TP7": 101.05,
}
DEBUG_ROTATION = {"SW2": 90.0, "SW1": 90.0}
POSITION_TOLERANCE = 0.05


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
    raise AssertionError(f"未闭合的 s-expression @ {start}")


def pcb_text() -> str:
    return PCB.read_text(encoding="utf-8")


def footprints(text: str) -> dict[str, str]:
    """位号 -> footprint s-expression。"""
    result: dict[str, str] = {}
    cursor = 0
    while True:
        start = text.find("\n\t(footprint ", cursor)
        if start < 0:
            return result
        footprint = extract_sexpr(text, start + 2)
        cursor = start + len(footprint) + 2
        reference = re.search(r'\(property "Reference" "([^"]+)"', footprint)
        if reference:
            result[reference.group(1)] = footprint


def footprint_placement(footprint: str) -> tuple[float, float, float, str]:
    at = re.search(r'\n\t\t\(at ([-0-9.]+) ([-0-9.]+)(?: ([-0-9.]+))?\)', footprint)
    if not at:
        raise AssertionError("footprint 缺少 at")
    layer = re.search(r'\n\t\t\(layer "([FB])\.Cu"\)', footprint)
    if not layer:
        raise AssertionError("footprint 缺少 layer")
    return float(at.group(1)), float(at.group(2)), float(at.group(3) or 0.0), layer.group(1)


class V4PlacementContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not PCB.is_file():
            raise AssertionError(f"找不到被测 PCB：{PCB}")
        cls.text = pcb_text()
        cls.footprints = footprints(cls.text)

    def test_functional_silkscreen_texts_are_preserved(self):
        actual = set(re.findall(r'\(gr_text "([^"]*)"', self.text))
        self.assertLessEqual(REQUIRED_TEXTS, actual, sorted(REQUIRED_TEXTS - actual))

    def test_named_rf_zones_are_preserved(self):
        actual = set(re.findall(r'\(name "([^"]+)"\)', self.text))
        self.assertLessEqual(REQUIRED_ZONES, actual, sorted(REQUIRED_ZONES - actual))

    def test_board_graphics_count_matches_finalized_layout(self):
        self.assertEqual(len(re.findall(r'^\t\(gr_', self.text, re.MULTILINE)), BOARD_GRAPHICS_COUNT)

    def test_debug_column_pads_are_labelled(self):
        actual = set(re.findall(r'\(gr_text "([^"]*)"', self.text))
        self.assertLessEqual(set(DEBUG_LABELS), actual, sorted(set(DEBUG_LABELS) - actual))

    def test_debug_column_is_a_single_back_side_row(self):
        for reference, expected_y in DEBUG_Y.items():
            with self.subTest(reference=reference):
                footprint = self.footprints.get(reference)
                self.assertIsNotNone(footprint, f"{reference} 不在 PCB 上")
                x, y, rotation, layer = footprint_placement(footprint)
                self.assertEqual(layer, "B", f"{reference} 必须在 B 面")
                self.assertAlmostEqual(x, DEBUG_X, delta=POSITION_TOLERANCE)
                self.assertAlmostEqual(y, expected_y, delta=POSITION_TOLERANCE)
                if reference in DEBUG_ROTATION:
                    self.assertAlmostEqual(rotation % 360, DEBUG_ROTATION[reference], delta=0.01)

    def test_back_side_carries_no_regular_components(self):
        """V4 B 面普通装配件只允许电池相关的 J9、RT1；TP/SW 是裸铜调试焊盘。"""
        allowed = {"J9", "RT1"} | set(DEBUG_Y)
        on_back = {
            reference
            for reference, footprint in self.footprints.items()
            if footprint_placement(footprint)[3] == "B"
        }
        self.assertLessEqual(on_back, allowed, sorted(on_back - allowed))


if __name__ == "__main__":
    unittest.main()
