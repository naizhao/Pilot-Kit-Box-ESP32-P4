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
# 用户 2026-09-01 定稿时删掉了 RST / BOOT 两条标签（SW1/SW2 的位号本身已能识别），
# 只保留 TP1-TP7 七条。这是用户对自己排版的决定，合同跟随实际板面。
DEBUG_LABELS = ("TP1", "TP2", "TP3", "TP4", "TP5", "TP6", "TP7")
BOARD_GRAPHICS_COUNT = FINALIZED_GRAPHICS_COUNT + len(DEBUG_LABELS)
# B 面左边缘同一竖列：SW1、SW2 相邻成组排在 TP 之前，SW1 在 SW2 上方。
# 两个 SW 之间用 3.60mm（SolderJumper courtyard 跨 3.39mm，2.50mm 会重叠）。
#
# 2026-09-04 用户手工重排：TP2–TP7 的间距由 2.50mm 收到 **2.033mm**，
# 整列仍在 x=51.85 的 B 面同一竖列，SW1 / SW2 / TP1 三个位置没动。
# 这一列往下腾出的空间给了挪过来的 RT1（54.813, 100.940）。
# 收紧后 `kicad-cli pcb drc` 仍是 0 违例（courtyard 没有重叠），
# 本合同按既定口径**跟随实际板面**，不反过来约束用户的排版决定。
DEBUG_X = 51.85
DEBUG_Y = {
    "SW1": 77.450, "SW2": 81.050, "TP1": 86.050, "TP2": 88.083,
    "TP3": 90.116, "TP4": 92.149, "TP5": 94.181, "TP6": 96.214,
    "TP7": 98.247,
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

    # ── PCB 与原理图的整体一致性审查 ──────────────────────────────────
    #
    # 这一组合同是被同一个盲区连着咬了四次之后补的：
    #   Task 3  U8.40 插入分压后没改网络
    #   Task 5  U15.5 插入串阻后没改网络，R57 被旧铜旁路
    #   9 处    U18.2/3、U4.15/16、U14.1/4、C37/C38、U15.6 网络错挂（用户发现）
    #   19+8 处 value 与 DNP 全部停留在 V4.2 母版的旧值（用户要求深度自查时发现）
    #
    # 共同根因有两条：
    #   ① **既有的合同都在检查正式板**（test_component_contract 用 pcb_footprint()
    #      读 hardware/expansion-board-v4/kicad/），而本轮所有改动都在候选板上。
    #      正式板是绿的，候选板烂着，没人知道。
    #   ② **KiCad DRC 不看这些**：pad 未赋网络时挂 `unconnected-(U18-CFG2-Pad2)`，
    #      那是合法的单节点网络；value 和 DNP 更是完全在 DRC 视野之外。
    #
    # 所以这里按「维度」而不是「个案」来查：网络、value、DNP、封装，全板逐项比对。
    # 再出现同类问题必须扩充这一组，而不是在别处打补丁。

    def _schematic_index(self):
        netlist = ROOT / "hardware" / "expansion-board-v4" / "build" / "netlist.xml"
        if not netlist.is_file():
            self.skipTest("缺少 build/netlist.xml，先跑一次 gen_bom.py 刷新")
        import xml.etree.ElementTree as ET
        root = ET.parse(netlist).getroot()
        nets, values, packages = {}, {}, {}
        for net in root.findall("./nets/net"):
            for node in net.findall("node"):
                nets[(node.attrib["ref"], node.attrib["pin"])] = net.attrib["name"]
        for comp in root.findall("./components/comp"):
            ref = comp.attrib["ref"]
            values[ref] = (comp.findtext("value") or "").strip()
            packages[ref] = (comp.findtext("footprint") or "").strip().split(":")[-1]
        return nets, values, packages

    def test_every_component_value_matches_the_schematic(self):
        _, values, _ = self._schematic_index()
        bad = []
        for reference, footprint in self.footprints.items():
            want = values.get(reference)
            if want is None:
                continue
            actual = re.search(r'\(property "Value" "([^"]*)"', footprint)
            actual = actual.group(1) if actual else None
            if actual != want:
                bad.append(f"{reference}: PCB={actual} 原理图={want}")
        self.assertEqual(sorted(bad), [], "PCB 元件 value 与原理图不一致")

    def test_do_not_populate_flags_match_the_schematic(self):
        """DNP 错了会让贴片厂把不该贴的贴上——R7/R8 与 CH224K 并存会烧板。"""
        _, values, _ = self._schematic_index()
        bad = []
        for reference, footprint in self.footprints.items():
            want = values.get(reference)
            if want is None:
                continue
            should = "DNP" in want
            # DNP 编码在 `(attr smd dnp)` 那一行里，不是独立的 `(dnp yes)`。
            attr = re.search(r'\n\t\t\(attr ([^\n]*)\)', footprint)
            marked = bool(attr) and "dnp" in attr.group(1).split()
            if should != marked:
                bad.append(f"{reference}: PCB dnp={marked} 原理图要求={should}")
        self.assertEqual(sorted(bad), [], "DNP 标记与原理图不一致")

    def test_every_footprint_matches_the_schematic(self):
        """封装不一致会直接导致贴不上（L17 曾是 Bourns 焊盘配 Coilcraft 料）。"""
        _, _, packages = self._schematic_index()
        bad = []
        for reference, footprint in self.footprints.items():
            want = packages.get(reference)
            if not want:
                continue
            actual = re.match(r'\(footprint "([^"]+)"', footprint)
            actual = actual.group(1).split(":")[-1] if actual else None
            if actual != want:
                bad.append(f"{reference}: PCB={actual} 原理图={want}")
        self.assertEqual(sorted(bad), [], "PCB 封装与原理图不一致")

    def test_every_pad_net_matches_the_schematic(self):
        """PCB 上每个焊盘的网络都必须与原理图网表一致。

        这条合同是补上来的，因为前面连着栽了三次：Task 3 的 U8.40、Task 5 的
        U15.5，都是「插入新器件后，既有引脚该改到新网络却没改」；第三次（U18.2/3、
        U4.15/16、U14.1/4、C37/C38、U15.6 共 9 处）是用户在 KiCad 里自己发现的。

        为什么 DRC 拦不住：pad 没被赋网络时 KiCad 给它挂 `unconnected-(U18-CFG2-Pad2)`
        这种**合法的单节点网络**，连通性检查认为它自成一体、毫无问题，一声不吭。
        而挂错到别的网络（U15.6 挂在 3V3_RF 而不是 GND）时，只要那个网络本身连通，
        DRC 同样不报——直到有人真的去读网表比对，或者板子做出来发现功能是死的。

        为什么原有的 test_component_contract 拦不住：它比对的是原理图网表和**正式板**，
        而本轮所有改动都在候选板上；它虽然有 pcb_pad_nets()，却只用来查 Q4/Q5 两处。
        """
        netlist = ROOT / "hardware" / "expansion-board-v4" / "build" / "netlist.xml"
        if not netlist.is_file():
            self.skipTest("缺少 build/netlist.xml，先跑一次 gen_bom.py 刷新")
        import xml.etree.ElementTree as ET
        expected = {}
        for net in ET.parse(netlist).getroot().findall("./nets/net"):
            for node in net.findall("node"):
                expected[(node.attrib["ref"], node.attrib["pin"])] = net.attrib["name"]

        mismatched = []
        for reference, footprint in self.footprints.items():
            for match in re.finditer(r'\(pad "([^"]+)" ', footprint):
                pad = extract_sexpr(footprint, match.start())
                net = re.search(r'\(net "([^"]+)"\)', pad)
                actual = net.group(1) if net else None
                want = expected.get((reference, match.group(1)))
                if want is None or actual == want:
                    continue
                mismatched.append(f"{reference}.{match.group(1)}: PCB={actual} 原理图={want}")
        self.assertEqual(mismatched, [], "PCB 焊盘网络与原理图不一致")

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
