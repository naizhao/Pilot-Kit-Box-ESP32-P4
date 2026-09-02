#!/usr/bin/env python3
"""v3/v4 关键器件引脚、外围网络与最终 PCB 的结构回归测试。"""

from __future__ import annotations

from pathlib import Path
import importlib.util
import csv
import json
import math
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


# RP2040 数据手册给的 ADC 输入阻抗最小值。只按理想分压算会高估读数：
# V4 早先的 300k/100k 理想值 2.5V，计入这个负载后实际只有 1.43V。
ADC_INPUT_RESISTANCE_MIN = 100_000.0
VBUS_SENSE = {
    # 窗口下限保证 5V/9V 可分辨，上限保证不逼近 IOVDD。
    "expansion-board-v4": {
        "r_top": "30k", "r_bottom": "10k", "max_vbus": 10.0, "window": (2.2, 2.5),
    },
    # V3 于 2026-09-02 阶段 C3 回灌为 10k/10k，窗口随之收紧。
    #
    # 换掉 100k/100k 不是为了改分压比（两者都是 2.0），是为了降源阻抗：
    # 100k/100k 的戴维南源阻抗 50k，RP2040 的 ADC 采样电容在采样窗内充不满，
    # 读数偏低而且随采样率变化。10k/10k 降到 5k，同时 ADC 那 100k 输入阻抗的
    # 并联影响也从"吃掉 1/3"变成"只差几个百分点"。
    #
    # V3 只有 USB 一路 5V（没有 CH224K 的 9V 诱骗），所以 max_vbus 取 5.5 而非 10.0，
    # 分压比也就用 2.0 而不是 V4 的 4.0；固件按 board profile 选倍率。
    "expansion-board-v3": {
        "r_top": "10k", "r_bottom": "10k", "max_vbus": 5.5, "window": (2.5, 2.7),
    },
}


def vbus_sense_voltage(max_vbus: float, r_top: str, r_bottom: str) -> float:
    """分压中点在最坏输入下的实际电压，含 ADC 输入阻抗并联。"""
    top = float(r_top.removesuffix("k")) * 1000.0
    bottom = float(r_bottom.removesuffix("k")) * 1000.0
    low = 1 / (1 / bottom + 1 / ADC_INPUT_RESISTANCE_MIN)
    return max_vbus * low / (top + low)


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


def pcb_back_references(board: str) -> set[str]:
    pcb = ROOT / "hardware" / board / "kicad" / f"{board}.kicad_pcb"
    text = pcb.read_text(encoding="utf-8")
    result: set[str] = set()
    cursor = 0
    while True:
        start = text.find("\n\t(footprint ", cursor)
        if start < 0:
            break
        footprint = extract_sexpr(text, start + 2)
        cursor = start + len(footprint) + 2
        reference = re.search(r'\(property "Reference" "([^"]+)"', footprint)
        layer = re.search(r'\n\t\t\(layer "([FB])\.Cu"\)', footprint)
        if reference and layer and layer.group(1) == "B":
            result.add(reference.group(1))
    return result


def pcb_rotation(board: str, reference: str) -> float:
    footprint = pcb_footprint(board, reference)
    match = re.search(r'\n\t\t\(at [-0-9.]+ [-0-9.]+(?: ([-0-9.]+))?\)', footprint)
    if not match:
        raise AssertionError(f"{board} {reference} 缺少 footprint at")
    return float(match.group(1) or 0.0) % 360.0


def pcb_pad_position(board: str, reference: str, pad_number: str) -> tuple[float, float]:
    footprint = pcb_footprint(board, reference)
    footprint_at = re.search(
        r'\n\t\t\(at ([-0-9.]+) ([-0-9.]+)(?: ([-0-9.]+))?\)', footprint
    )
    if not footprint_at:
        raise AssertionError(f"{board} {reference} 缺少 footprint at")
    fx, fy = float(footprint_at.group(1)), float(footprint_at.group(2))
    rotation = math.radians(-float(footprint_at.group(3) or 0.0))
    marker = re.search(rf'\(pad "{re.escape(pad_number)}" ', footprint)
    if not marker:
        raise AssertionError(f"{board} {reference} 缺少 pad {pad_number}")
    pad = extract_sexpr(footprint, marker.start())
    pad_at = re.search(r'\(at ([-0-9.]+) ([-0-9.]+)', pad)
    px, py = (0.0, 0.0) if not pad_at else (
        float(pad_at.group(1)), float(pad_at.group(2))
    )
    return (
        fx + px * math.cos(rotation) - py * math.sin(rotation),
        fy + px * math.sin(rotation) + py * math.cos(rotation),
    )


def pad_distance(board: str, first: tuple[str, str], second: tuple[str, str]) -> float:
    return math.dist(
        pcb_pad_position(board, *first),
        pcb_pad_position(board, *second),
    )


def pcb_vias(board: str) -> list[tuple[float, float, str]]:
    """板上每个过孔的 (x, y, 网络名)。

    KiCad 10 在 via 里直接写网络**名**（`(net "3V3_DIG")`），文件里没有
    `(net N "NAME")` 声明表；更早的版本写的是序号。两种都认，否则解析出来
    每个过孔的网络都是空串，判据会静默退化成「整个网络一个过孔都没有」。
    """
    text = (ROOT / "hardware" / board / "kicad" / f"{board}.kicad_pcb").read_text(
        encoding="utf-8"
    )
    names = {
        int(code): name
        for code, name in re.findall(r'\n\t\(net (\d+) "([^"]*)"\)', text)
    }
    vias = []
    for match in re.finditer(r'\n\t\(via\b', text):
        via = extract_sexpr(text, match.start() + 2)
        at = re.search(r'\(at ([-0-9.]+) ([-0-9.]+)\)', via)
        if not at:
            continue
        named = re.search(r'\(net "([^"]*)"\)', via)
        numbered = re.search(r'\(net (\d+)\)', via)
        if named:
            net = named.group(1)
        elif numbered:
            net = names.get(int(numbered.group(1)), "")
        else:
            continue
        vias.append((float(at.group(1)), float(at.group(2)), net))
    if not vias:
        raise AssertionError(f"{board} 一个过孔都没解析出来，检查 pcb_vias 的正则")
    return vias


def pcb_zone_polygons(board: str) -> dict[tuple[str, str], list[list[tuple[float, float]]]]:
    """(铜层, 网络) -> 该层该网络的全部填充多边形。规则区没有填充，自然不会进来。"""
    text = (ROOT / "hardware" / board / "kicad" / f"{board}.kicad_pcb").read_text(
        encoding="utf-8"
    )
    polygons: dict[tuple[str, str], list[list[tuple[float, float]]]] = {}
    for match in re.finditer(r'\n\t\(zone\b', text):
        zone = extract_sexpr(text, match.start() + 2)
        # KiCad 10 写 `(net "GND")`；旧版是 `(net_name "GND")`。规则区两者都没有，
        # 正好被跳过。写错这个正则的后果是解析结果全空，而判据会静默退化成
        # 「哪个焊盘都不在铜面上」——看起来像板子有问题，其实是解析器瞎了。
        net = re.search(r'\(net(?:_name)? "([^"]*)"\)', zone)
        if not net:
            continue
        for filled in re.finditer(r'\(filled_polygon\b', zone):
            block = extract_sexpr(zone, filled.start())
            layer = re.search(r'\(layer "([^"]+)"\)', block)
            if not layer:
                continue
            points = [
                (float(x), float(y))
                for x, y in re.findall(r'\(xy ([-0-9.]+) ([-0-9.]+)\)', block)
            ]
            if points:
                polygons.setdefault((layer.group(1), net.group(1)), []).append(points)
    if not polygons:
        raise AssertionError(
            f"{board} 一块覆铜都没解析出来，检查 pcb_zone_polygons 的正则"
        )
    return polygons


def point_in_polygon(point: tuple[float, float], polygon: list[tuple[float, float]]) -> bool:
    """标准 PNPOLY 射线法。

    ⚠️ 边界是**半开**的：左边/下边判 True，右边/上边判 False（实测单位正方形
    (10,5) 和 (5,10) 都返回 False）。这里原先的注释写着「边界算在内」，是假的，
    2026-09-01 独立复核时抓出来的。实际风险低——填充坐标 6 位小数，焊盘中心与
    铜边精确重合的概率极小——但别拿这句话当保证。
    """
    x, y = point
    inside = False
    for i in range(len(polygon)):
        x1, y1 = polygon[i]
        x2, y2 = polygon[i - 1]
        if (y1 > y) != (y2 > y):
            crossing = (x2 - x1) * (y - y1) / (y2 - y1) + x1
            if x < crossing:
                inside = not inside
    return inside


def pad_copper_layer(board: str, reference: str, pad_number: str) -> str | None:
    """SMD 焊盘所在的铜层。通孔焊盘横跨 F/B，这里只关心表贴。"""
    footprint = pcb_footprint(board, reference)
    marker = re.search(rf'\(pad "{re.escape(pad_number)}" ', footprint)
    if not marker:
        return None
    pad = extract_sexpr(footprint, marker.start())
    for layer in ("F.Cu", "B.Cu"):
        if re.search(rf'\(layers[^)]*"{re.escape(layer)}"', pad):
            return layer
    return None


def pad_reaches_plane(board: str, reference: str, pad_number: str,
                      via_limit: float = 1.5) -> tuple[bool, str]:
    """焊盘有没有**低电感地**接入同网平面，返回 (结论, 说明)。

    两条路都算数，只要走通一条：
      ① 焊盘所在层就有同网覆铜且焊盘落在里面 —— 直接融进铜面，没有额外电感
      ② 旁边 `via_limit` 内有同网过孔 —— 穿层下到别层的铜面

    只认过孔会误判：C84/C85/C86 的地脚坐在 F.Cu 的 GND 大铜面上，最近的
    GND 过孔在 3mm 外，但它们本来就在平面里，那个距离没有意义。
    """
    net = pcb_pad_nets(board, reference).get(pad_number)
    if not net:
        raise AssertionError(f"{board} {reference}.{pad_number} 没有网络")
    position = pcb_pad_position(board, reference, pad_number)
    layer = pad_copper_layer(board, reference, pad_number)
    if layer:
        for polygon in pcb_zone_polygons(board).get((layer, net), []):
            if point_in_polygon(position, polygon):
                return True, f"落在 {layer} 的 {net} 铜面内"
    distance = pad_to_plane_via(board, reference, pad_number)
    if distance < via_limit:
        return True, f"最近 {net} 过孔 {distance:.2f}mm"
    return False, f"既不在同层 {net} 铜面内，最近 {net} 过孔也有 {distance:.2f}mm"


def point_touches_polygon(point: tuple[float, float],
                          polygon: list[tuple[float, float]],
                          tolerance: float = 0.6) -> bool:
    """点在多边形内，**或**贴着它的边界。

    过孔不能用「中心是否在多边形内」来判归属：KiCad 填充覆铜时会绕开过孔生成
    边界，属于该网络的过孔其中心往往落在填充多边形**之外**，靠自身铜环相连。
    只判包含会把接得好好的过孔判成「没接到平面」（U8.23 实测就是这样）。
    """
    if point_in_polygon(point, polygon):
        return True
    x, y = point
    for i in range(len(polygon)):
        x1, y1 = polygon[i]
        x2, y2 = polygon[i - 1]
        dx, dy = x2 - x1, y2 - y1
        span = dx * dx + dy * dy
        t = 0.0 if span == 0 else max(0.0, min(1.0, ((x - x1) * dx + (y - y1) * dy) / span))
        if math.dist((x, y), (x1 + t * dx, y1 + t * dy)) <= tolerance:
            return True
    return False


def plane_island(board: str, reference: str, pad_number: str) -> tuple[str, int] | None:
    """焊盘接入的是**哪一块**平面铜，返回 (层, 块序号)；接不上返回 None。

    去耦的物理论证（扩散电感随距离只是对数增长）有一个隐含前提：电容和芯片
    引脚必须在**同一块连续铜面**上。如果平面碎了、两者落在不同岛上，电流就得
    绕到别处去汇合，扩散公式立刻不适用，而「距离」和「有没有过孔」这两个判据
    都看不见这件事。

    本项目栽过：In3 铺 3V3_DIG 又走 181 段信号，碎成 14 块。那种板上每颗电容
    旁边都有过孔，旧判据一样全绿。
    """
    net = pcb_pad_nets(board, reference).get(pad_number)
    if not net:
        raise AssertionError(f"{board} {reference}.{pad_number} 没有网络")
    position = pcb_pad_position(board, reference, pad_number)
    polygons = pcb_zone_polygons(board)
    # ① 焊盘所在层直接压在同网铜面上
    layer = pad_copper_layer(board, reference, pad_number)
    if layer:
        for index, polygon in enumerate(polygons.get((layer, net), [])):
            if point_in_polygon(position, polygon):
                return (layer, index)
    # ② 否则看它的接入过孔落在哪一块——过孔是穿层的，用它的坐标去找
    vias = [(x, y) for x, y, n in pcb_vias(board) if n == net]
    if not vias:
        return None
    entry = min(vias, key=lambda p: math.dist(position, p))
    if math.dist(position, entry) >= 1.5:
        return None
    for (plane_layer, plane_net), shapes in sorted(polygons.items()):
        if plane_net != net or plane_layer == layer:
            continue
        for index, polygon in enumerate(shapes):
            if point_touches_polygon(entry, polygon):
                return (plane_layer, index)
    return None


def pad_to_plane_via(board: str, reference: str, pad_number: str) -> float:
    """焊盘到最近的**同网**过孔的距离——衡量它接入内层/背面平面有多快。

    去耦电容要起作用，两个焊盘都得低电感地落到平面上。落法只有两种：脚下
    直接是同网覆铜，或者旁边有同网过孔穿层下去。RP2040 这几颗本地去耦服务
    的平面都不在 F.Cu 上（`RP_1V1` 在 B.Cu、`3V3_DIG` 在 In3），电容本身在
    F.Cu，所以**只能走过孔**，这个距离就是完整判据。
    """
    px, py = pcb_pad_position(board, reference, pad_number)
    net = pcb_pad_nets(board, reference).get(pad_number)
    if not net:
        raise AssertionError(f"{board} {reference}.{pad_number} 没有网络")
    same_net = [(x, y) for x, y, n in pcb_vias(board) if n == net]
    if not same_net:
        raise AssertionError(f"{board} {net} 整个网络一个过孔都没有")
    return min(math.dist((px, py), point) for point in same_net)


def pcb_placements(board: str) -> dict[str, tuple[float, float, bool]]:
    """全板每个位号的 (中心x, 中心y, 是否DNP)。给 CPL 校验用。

    走整份文件遍历，不复用 pcb_footprint()——那个按位号定位，一次只取一个，
    207 个封装要重读 207 次文件。
    """
    pcb = ROOT / "hardware" / board / "kicad" / f"{board}.kicad_pcb"
    text = pcb.read_text(encoding="utf-8")
    result: dict[str, tuple[float, float, bool]] = {}
    for match in re.finditer(r"\n\t\(footprint ", text):
        block = extract_sexpr(text, match.start() + 2)
        reference = re.search(r'\(property "Reference" "([^"]*)"', block)
        at = re.search(r"\n\t\t\(at ([-0-9.]+) ([-0-9.]+)", block)
        if not reference or not at:
            continue
        result[reference.group(1)] = (
            float(at.group(1)), float(at.group(2)), "(dnp yes)" in block
        )
    if not result:
        raise AssertionError(f"{board} 一个封装都没解析出来，正则该更新了")
    return result


def schematic_symbol(board: str, sheet: str, reference: str) -> str:
    path = ROOT / "hardware" / board / "kicad" / f"{sheet}.kicad_sch"
    text = path.read_text(encoding="utf-8")
    marker_at = text.index(f'(property "Reference" "{reference}"')
    symbol_at = text.rfind("\n\t(symbol", 0, marker_at) + 2
    return extract_sexpr(text, symbol_at)


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
                root = ROOT / "hardware" / board
                sources = (root / "tools" / "sheet_rf1090.py", root / "internal" / "tools" / "sheet_rf1090.py")
                source = next(path for path in sources if path.is_file()).read_text(encoding="utf-8")
                self.assertIn('"#FLG04", "power", "PWR_FLAG"', source)
                self.assertIn('{"1": "LNA1_VBIAS"}', source)

    def test_component_side_assembly_contract(self):
        if "expansion-board-v3" in BOARDS:
            self.assertEqual(
                pcb_back_references("expansion-board-v3"),
                set(),
                "V3 是单面装配板，禁止任何 B 面元件",
            )
        if "expansion-board-v4" in BOARDS:
            self.assertEqual(
                pcb_back_references("expansion-board-v4"),
                # V4.3 把调试列整理成 B 面左边缘一竖列：SW1/SW2 成组在前，
                # 其后 TP1-TP7。之前这里只写了 SW2/TP1/TP2/TP6/TP7，是 V4.2
                # 时代那几个散落焊盘的残留，会把 V4.3 的完整调试列判成违规。
                {"J9", "RT1", "SW1", "SW2",
                 "TP1", "TP2", "TP3", "TP4", "TP5", "TP6", "TP7"},
                "V4 B 面只允许电池件，以及不进 BOM 的测试/短接焊盘",
            )

    def test_secondary_audit_rf_and_reset_corrections(self):
        expected_nets = {
            ("U15", "6"): "GND",
            ("C34", "1"): "SAW2_OUT",
            ("C34", "2"): "DET_IN",
            ("C35", "1"): "DET_INLO",
            ("C35", "2"): "GND",
            ("C81", "1"): "3V3_RF",
            ("C81", "2"): "GND",
            ("R47", "1"): "3V3_DIG",
            ("R47", "2"): "SUBG_RESET",
        }
        expected_values = {
            "C34": "100pF C0G",
            "C35": "100pF C0G",
            "C81": "470pF C0G",
            "R47": "10k",
        }
        for board, (pin_nets, values) in self.schematic.items():
            with self.subTest(board=board):
                self.assertEqual(
                    {key: pin_nets.get(key) for key in expected_nets}, expected_nets
                )
                self.assertEqual(
                    {ref: values.get(ref) for ref in expected_values}, expected_values
                )
                self.assertLess(pad_distance(board, ("U12", "1"), ("C81", "1")), 2.5)
                # R47 是静态复位上拉，不是高速旁路。V3 在 4.32 mm 处满足电气需求；
                # 继续向 U10 靠近会与 QFN courtyard 重叠，因此按 4.5 mm 约束。
                self.assertLess(pad_distance(board, ("U10", "35"), ("R47", "2")), 4.5)
                self.assertIn("(dnp yes)", schematic_symbol(board, "rf1090", "R36"))
                self.assertTrue("dnp" in pcb_footprint(board, "R36").split("(attr", 1)[1].split(")", 1)[0])

                # AD8313 VPOS 由 10R 隔离后在本地去耦；检波输出不再跨电源域上拉。
                ad8313_supply = {
                    ("U14", "1"): "AD8313_VPOS", ("U14", "4"): "AD8313_VPOS",
                    ("R54", "1"): "3V3_RF", ("R54", "2"): "AD8313_VPOS",
                    ("C37", "1"): "AD8313_VPOS", ("C38", "1"): "AD8313_VPOS",
                }
                self.assertEqual(
                    {key: pin_nets.get(key) for key in ad8313_supply}, ad8313_supply
                )
                self.assertEqual(values.get("R54"), "10R")
                for ref in ("R30",):
                    self.assertIn("(dnp yes)", schematic_symbol(board, "rf1090", ref))
                    attr = pcb_footprint(board, ref).split("(attr", 1)[1].split(")", 1)[0]
                    self.assertIn("dnp", attr)

        if "expansion-board-v3" in BOARDS:
            _, values = self.schematic["expansion-board-v3"]
            self.assertEqual(values.get("U14"), "AD8313ARMZ")
            self.assertEqual(values.get("R21"), "0R")
            # 这里原本还断言「U13 必须带 DNP」——那是双检波位时代的判据：
            # AD8319(U13) 与 AD8313(U14) 并存，靠 DNP 表达"默认不贴哪个"。
            # 2026-09-02 整条 AD8319 支路已从设计中删除，U13 不存在了，
            # 再去取它的符号只会抛 ValueError。
            # 「删干净了没有」由 test_v3_ad8319_experimental_branch_is_gone 负责。

    def test_rp2040_core_decoupling_crystal_and_usb_protection(self):
        expected_nets = {
            ("C82", "1"): "RP_1V1", ("C82", "2"): "GND",
            ("C83", "1"): "RP_1V1", ("C83", "2"): "GND",
            ("C84", "1"): "3V3_DIG", ("C84", "2"): "GND",
            ("C85", "1"): "3V3_DIG", ("C85", "2"): "GND",
            ("C86", "1"): "3V3_DIG", ("C86", "2"): "GND",
            ("D4", "1"): "USB_DP", ("D4", "2"): "GND",
            ("D5", "1"): "USB_DM", ("D5", "2"): "GND",
            ("R50", "1"): "USB_VBUS", ("R50", "2"): "USB_VBUS_SENSE",
            ("R51", "1"): "USB_VBUS_SENSE", ("R51", "2"): "GND",
            ("U8", "40"): "USB_VBUS_SENSE",
        }
        expected_values = {
            "C19": "15pF C0G", "C20": "15pF C0G",
            "C82": "100nF", "C83": "100nF",
            "C84": "1uF", "C85": "100nF", "C86": "100nF",
            "D4": "TPESD8L3.3 0.3pFtyp 0.5pFmax", "D5": "TPESD8L3.3 0.3pFtyp 0.5pFmax",
            "Y1": "12MHz CL=10pF ABM8-272-T3",
        }
        for board, (pin_nets, values) in self.schematic.items():
            with self.subTest(board=board):
                self.assertEqual(
                    {key: pin_nets.get(key) for key in expected_nets}, expected_nets
                )
                self.assertEqual(
                    {ref: values.get(ref) for ref in expected_values}, expected_values
                )
                spec = VBUS_SENSE[board]
                self.assertEqual(values.get("R50"), spec["r_top"])
                self.assertEqual(values.get("R51"), spec["r_bottom"])
                v_adc = vbus_sense_voltage(
                    spec["max_vbus"], spec["r_top"], spec["r_bottom"]
                )
                self.assertLess(
                    v_adc, 3.3,
                    f"{board} USB_VBUS_SENSE 最坏输入超过 RP2040 IOVDD",
                )
                low, high = spec["window"]
                self.assertTrue(
                    low < v_adc < high,
                    f"{board} 计入 ADC 输入阻抗后为 {v_adc:.3f}V，不在 {low}-{high}V 窗口内",
                )
                if board == "expansion-board-v4":
                    # 分压中点的抗干扰电容；V3 在 Task 13 回灌。
                    self.assertEqual(values.get("C87"), "10nF C0G")
                    self.assertEqual(pin_nets.get(("C87", "1")), "USB_VBUS_SENSE")
                    self.assertEqual(pin_nets.get(("C87", "2")), "GND")
                # RP2040 本地去耦的判据：2026-09-01 由「离引脚多远」换成
                # 「接入平面有多快」。
                #
                # 换的理由是物理，不是为了让板子通过：
                #   · 这几颗服务的平面都不在 F.Cu（RP_1V1 在 B.Cu、3V3_DIG 在
                #     In3），电容在 F.Cu，必须靠过孔穿层才能接入。
                #   · 接入之后，平面对的扩散电感随距离是**对数**增长——
                #     L ≈ (µ0·h/2π)·ln(r2/r1)，0.2mm 层间距下 30mm 也只有
                #     约 0.16nH，距离项基本消失。
                #   · 而焊盘到过孔那一小段是**走线**，电感随长度线性增长
                #     （约 0.35nH/mm），加上过孔自身约 1nH，这才是主导项。
                #
                # 旧的纯距离阈值恰恰管不到主导项：把电容贴在引脚旁却一个孔都
                # 不打，照样是废的，而旧合同会放行。所以这是**收紧**不是放宽。
                #
                # 实测（2026-09-01 定稿板）：13 颗的电源脚过孔 0.48–0.86mm，
                # C82/C83 的地脚 0.59mm。1.5mm 阈值留了约一倍余量。
                #
                # ⚠️ 已知局限，别把它当成比实际更强的防护：**真正起作用的是电源脚
                # 那一半**。反例实测——把 C82 假想挪到 (140,105)，pad1 正确报
                # 未接入（59.24mm），但 pad2 仍判通过，因为 F.Cu 全板铺 GND，
                # 地脚落在哪都在铜面里。这是物理事实不是 bug（GND 平面本就覆盖
                # 全板），但意味着地脚这一半几乎恒真。若将来 F.Cu 不再整面铺地，
                # 这条才会重新有鉴别力。
                # ⚠️ 2026-09-02 扩容：此前这个循环**只有 C82-C86**，
                # C22-C29 从来没被任何合同检查过——外部复核指出这个盲区。
                # C22-C27 是 RP2040 六个 IOVDD 引脚的专属 100nF，C28/C29 是
                # VREG_VOUT/DVDD 的 1uF，漏检它们意味着「RP2040 去耦已审查」
                # 这句话本身是不成立的。现在 13 颗全覆盖。
                RP2040_DECOUPLING = ("C22", "C23", "C24", "C25", "C26", "C27",
                                     "C28", "C29", "C82", "C83", "C84", "C85", "C86")
                for cap in RP2040_DECOUPLING:
                    for pad in ("1", "2"):
                        with self.subTest(cap=cap, pad=pad, check="接入平面"):
                            reached, why = pad_reaches_plane(board, cap, pad)
                            self.assertTrue(
                                reached,
                                f"{cap}.{pad} 没有低电感地接入平面：{why}",
                            )
                # 这一条替换了 2026-09-01 最早写的「距离 < 45mm」。那个上限是从
                # 实测最大值 39.72mm 反推的，蒙特卡洛实测它覆盖全板 76.6% 面积
                # （旧的五个距离阈值只覆盖 0.78%~3.17%），鉴别力弱了约 40 倍——
                # 它保留了「防止有人挪得更远」这句注释，没保留这句话说的防护。
                #
                # 换成真正的物理前提：电容与它服务的引脚必须落在**同一块连续铜面**
                # 上。扩散电感的对数公式只在同一块平面内成立；平面一碎，电流要绕到
                # 别处汇合，距离和过孔这两个判据都看不见这件事。
                for pin, cap in (
                    ("23", "C82"), ("50", "C83"), ("44", "C84"),
                    ("48", "C85"), ("43", "C86"),
                ):
                    with self.subTest(cap=cap, check="与引脚同一块平面"):
                        cap_island = plane_island(board, cap, "1")
                        pin_island = plane_island(board, "U8", pin)
                        self.assertIsNotNone(cap_island, f"{cap} 没接到任何平面")
                        self.assertIsNotNone(pin_island, f"U8.{pin} 没接到任何平面")
                        self.assertEqual(
                            cap_island, pin_island,
                            f"{cap} 在 {cap_island}、U8.{pin} 在 {pin_island}，"
                            f"不是同一块铜面——去耦回路要绕路，扩散电感的推导不成立",
                        )
                # ── 本地距离：锁住 2026-09-02 那次搬迁 ──────────────────────
                # 上面三条（接入平面 / 同块平面 / 平面连续）验的都是**接入质量**，
                # 一条都不管电容摆在哪。外部复核点破了这个洞：C22-C27 在 30.6-34.0mm
                # 的那版旧布局，上面三条全过。判据必须能把那版判死，否则「本地去耦
                # 已合规」这句话是空的。
                #
                # 判据做成**双向**，因为单向哪一边都能被绕过：
                #   · 只查「每个脚有近电容」→ 在 U8 正中放一颗就能覆盖全部 9 个脚
                #     （QFN-56 是 7×7mm，中心到最远脚约 5mm），其余五颗扔到板边也过。
                #   · 只查「每颗电容离脚近」→ 六颗全挤在同一侧也过，另一侧的脚
                #     一颗都没有。
                #
                # 但**别把它当成分布性判据**：反例实测，六颗全部叠放到 U8 几何中心
                # 这一点上，双向判据照样全过（每颗都离脚近、每个脚也都离容近）。
                # 挡住这种情况的是 DRC 的 courtyards_overlap（同一反例报 21 条），
                # 不是这里。这是分工，不是漏洞——但如果哪天有人放宽了 courtyard
                # 规则，这条判据不会替它补位。
                #
                # 反例标定（沙箱里改副本跑的，真板未动）：
                #   C27 沿背离 U8 方向外推 1.0mm → 仍过（在设计余量内）
                #                        1.5mm → 报警 6.75≥6.5 ✓
                #   C22-C27 整体还原到 30mm 那版旧布局 → 报警 15 条 ✓
                #
                # 候选池按**网络**分，不是按外部复核给的那份清单。复核把 C82-C86
                # 一并称作「豁免五颗」，但实测 C82/C83 挂在 RP_1V1 上、离 U8 只有
                # 6.10/7.65mm —— U8.23(DVDD) 的最近电容就是 C83(6.10mm)，而不是
                # C28/C29(9.45mm)。C82/C83 是本地去耦的一部分，不是豁免。
                # 真正远置的只有 3V3_DIG 上的 C84/C85/C86（20.09/23.11/34.11mm），
                # 它们不进候选池，也就不会替任何脚背书。
                #
                # 阈值＝实测值 + 约 1.2-1.4mm（一颗 0603 的长度）。这是**回归阈值**
                # 不是 datasheet 数字：RP2040 手册 §2.9 只说 "as close as possible"
                # 没给毫米数。留这点余量是允许布线微调，不允许搬家。
                if board == "expansion-board-v4":
                    u8_nets = pcb_pad_nets(board, "U8")
                    for net, caps, pin_limit, cap_limit, measured in (
                        # 网络      候选电容                          脚→容  容→脚  (实测覆盖, 实测锚定)
                        ("3V3_DIG", ("C22", "C23", "C24", "C25", "C26", "C27"),
                         6.0, 6.5, (4.97, 5.25)),
                        ("RP_1V1", ("C28", "C29", "C82", "C83"),
                         7.5, 9.0, (6.10, 7.65)),
                    ):
                        pins = [p for p, n in u8_nets.items() if n == net]
                        self.assertTrue(pins, f"U8 上找不到 {net} 引脚")
                        # 覆盖侧：每个电源脚都得有一颗近电容
                        for pin in sorted(pins, key=int):
                            with self.subTest(pin=f"U8.{pin}", net=net, check="脚有本地电容"):
                                near, cap = min(
                                    (pad_distance(board, ("U8", pin), (c, "1")), c)
                                    for c in caps
                                )
                                self.assertLess(
                                    near, pin_limit,
                                    f"U8.{pin}({net}) 最近的去耦是 {cap} {near:.2f}mm，"
                                    f"超过 {pin_limit}mm；实测基线 {measured[0]}mm",
                                )
                        # 锚定侧：每颗电容都得贴着某个电源脚
                        for cap in caps:
                            with self.subTest(cap=cap, net=net, check="电容在本地"):
                                near, pin = min(
                                    (pad_distance(board, (cap, "1"), ("U8", p)), p)
                                    for p in pins
                                )
                                self.assertLess(
                                    near, cap_limit,
                                    f"{cap}({net}) 离最近的 U8.{pin} 有 {near:.2f}mm，"
                                    f"超过 {cap_limit}mm；实测基线 {measured[1]}mm",
                                )
                # V3 不套这条，理由是**候选池对不上**，不是 V3 布局差。
                #
                # 更正一处误测：此处原写「V3 同口径实测锚定 22.80mm / 覆盖 19.68mm，就是
                # V4 搬迁前那种布局」——错的。那是把 V4 的候选池 C22-C27 直接套到 V3 上量的，
                # 而 V3 的 C22-C27 根本不是 RP2040 去耦，量的是一组不相干的电容。
                # 用 V3 真正的去耦件重测：C84→U8.44 1.46mm、C85→U8.49 3.87mm、
                # C86→U8.44 3.75mm、C82→U8.23 3.52mm、C83→U8.50 1.51mm，**本来就是好的**。
                #
                # 两版去耦结构不同：V4 是 6 颗 IOVDD 专属 + 2 颗 VREG + 5 颗共 13 颗，
                # V3 只有 C82-C86 五颗。要给 V3 加同类判据得按它自己的结构另立候选池，
                # 照抄这段会再犯一次同样的错。已列入 V3.9 回灌清单。
                # 平面本身必须连续。碎成多块时上面那条同块断言仍可能通过（两者
                # 恰好在同一小块里），但整个平面的阻抗已经不是当初论证的那个了。
                for plane_net, plane_layer in (("3V3_DIG", "In3.Cu"), ("RP_1V1", "B.Cu")):
                    with self.subTest(plane=plane_net, check="平面连续"):
                        islands = pcb_zone_polygons(board).get((plane_layer, plane_net), [])
                        self.assertEqual(
                            len(islands), 1,
                            f"{plane_net}@{plane_layer} 碎成 {len(islands)} 块。"
                            f"RP2040 本地去耦的整套论证以它是单块为前提",
                        )
                # V3 是 0402；V4 曾经写死 0201，那是错的——用户的 BOM 里从来没有
                # 0201，凭空多出 3 种 SKU 且手工无法返修。2026-09-01 统一到 0603。
                local_fp = (
                    "C_0402_1005Metric"
                    if board == "expansion-board-v3"
                    else "C_0603_1608Metric"
                )
                for ref in ("C82", "C83", "C84", "C85", "C86"):
                    self.assertIn(local_fp, pcb_footprint(board, ref))
                # 只查 fpid，不要在整段 footprint 文本里搜 "0201"。
                # 2026-09-03 实测这个粗判据会误报：C82 的一个 UUID 恰好是
                # `5d03dc64-3800-43de-89e4-80201ddbb589`，中间含 "80201d"，
                # 于是明明是 C_0402_1005Metric 也被判成引入了 0201。
                # 假阳性比漏报更坏——它会淹没同一个测试里真正的失败项。
                for ref in ("C82", "C83", "C84", "C85", "C86"):
                    fpid = re.search(r'\(footprint "([^"]+)"',
                                     pcb_footprint(board, ref)).group(1)
                    self.assertNotIn(
                        "0201", fpid,
                        f"{ref} 用了 {fpid}；BOM 里没有 0201，不得引入这个封装",
                    )
                self.assertLess(pad_distance(board, ("J4", "A6"), ("D4", "1")), 6.0)
                self.assertLess(pad_distance(board, ("J4", "A7"), ("D5", "1")), 6.0)
                project = json.loads(
                    (ROOT / "hardware" / board / "kicad" / f"{board}.kicad_pro")
                    .read_text(encoding="utf-8")
                )
                assignments = project["net_settings"].get("netclass_patterns", [])
                self.assertNotIn(
                    {"netclass": "POWER", "pattern": "USB_VBUS_SENSE"},
                    assignments,
                    "VBUS 分压后的 ADC sense 线不是大电流电源线",
                )
                classes = {item["name"]: item for item in project["net_settings"]["classes"]}
                self.assertEqual(classes["FINE"]["track_width"], 0.13)
                self.assertEqual(classes["FINE"]["clearance"], 0.13)
                for net in ("SWCLK", "SWDIO", "DEMOD0", "DEMOD1", "DEMOD2", "DEMOD3",
                            "RECOVERED_CLK"):
                    self.assertIn({"netclass": "FINE", "pattern": net}, assignments)

    def test_smt_release_cpl_matches_current_board(self):
        """release/smt/ 里的三套 CPL 必须是**当前板**导出的。

        2026-09-02 抓到的真实事故：`gen_jlc_smt.py` 有 minimal/passives/full
        三个方案，命令行不带参数只会重出 passives。搬完 C22-C27 之后我只跑了
        默认那次，`CPL-minimal.csv` 就留在了搬迁前——里面 C22 还写着 X=121.25，
        而板上已经是 93.218。外部复核当时查了 CPL-full 是对的，就没往下查，
        三套里坏掉的那套一路活到了发布目录。

        位号数和 DNP 泄漏都查不出这个：旧文件的位号集合、行数、DNP 全是对的，
        **只有坐标是旧的**。所以这里比的是坐标本身。

        嘉立创 CPL 的口径：Mid X = 封装中心 x，Mid Y = **负的**中心 y。
        """
        for board in BOARDS:
            smt = ROOT / "hardware" / board / "release" / "smt"
            if not smt.is_dir():
                continue
            placements = pcb_placements(board)
            dnp = {ref for ref, (_x, _y, is_dnp) in placements.items() if is_dnp}
            files = sorted(smt.glob("CPL-*.csv"))
            with self.subTest(board=board, check="CPL 文件齐全"):
                self.assertEqual(
                    [path.name for path in files],
                    ["CPL-full.csv", "CPL-minimal.csv", "CPL-passives.csv"],
                    "三个方案的 CPL 要么都在，要么就是有人漏跑了 gen_jlc_smt.py",
                )
            for path in files:
                rows = list(csv.DictReader(path.open(encoding="utf-8-sig")))
                with self.subTest(cpl=path.name, check="非空"):
                    self.assertTrue(rows, f"{path.name} 是空的")
                for row in rows:
                    ref = row["Designator"].strip()
                    with self.subTest(cpl=path.name, ref=ref):
                        self.assertIn(ref, placements, f"{ref} 不在板上")
                        self.assertNotIn(
                            ref, dnp, f"{path.name} 把 DNP 件 {ref} 发给贴片了",
                        )
                        x, y, _ = placements[ref]
                        self.assertAlmostEqual(
                            float(row["Mid X"]), x, delta=0.01,
                            msg=f"{path.name} 的 {ref} X={row['Mid X']}，"
                                f"板上是 {x:.3f}——这套 CPL 不是当前板导出的",
                        )
                        self.assertAlmostEqual(
                            float(row["Mid Y"]), -y, delta=0.01,
                            msg=f"{path.name} 的 {ref} Y={row['Mid Y']}，"
                                f"板上是 {-y:.3f}——这套 CPL 不是当前板导出的",
                        )

    def test_v3_ad8319_experimental_branch_is_gone(self):
        """阶段 B：AD8319 实验支路必须从设计里物理消失。

        V3 曾经把 AD8319(U13) 和 AD8313(U14) 并排放成"二选一"的双检波实验位，默认贴
        AD8319。产品决策后来统一到 AD8313（两版同料，固件不必按检波器型号翻 RSSI 斜率），
        但 U13/R20/DET_TADJ 一直留在生成源里，制造输出还在指示"贴 AD8319、不贴 AD8313"
        ——**方向正好相反**，照着贴就是错的。

        这里同时锁住"删干净"和"没删过头"：R19/C34/C35/R54/C37/C38 属于保留的
        AD8313 通路，不能跟着一起被删。
        """
        if "expansion-board-v3" not in BOARDS:
            self.skipTest("当前分支不维护 v3")
        pin_nets, values = self.schematic["expansion-board-v3"]
        for gone in ("U13", "R20"):
            with self.subTest(reference=gone, check="已从网表消失"):
                self.assertNotIn(gone, values, f"{gone} 仍在网表里")
        with self.subTest(check="DET_TADJ 网络已消失"):
            self.assertNotIn("DET_TADJ", set(pin_nets.values()))
        with self.subTest(check="AD8313 是唯一通路"):
            self.assertEqual(values.get("U14"), "AD8313ARMZ")
            self.assertEqual(values.get("R21"), "0R")
            self.assertNotIn("DNP", values.get("R21", ""))
        for kept in ("R19", "C34", "C35", "R54", "C37", "C38"):
            with self.subTest(reference=kept, check="保留件没被误删"):
                self.assertIn(kept, values, f"{kept} 属于 AD8313 通路，不该删")
        with self.subTest(check="RF_DET_OUT 仍连通下游"):
            for ref, pad in (("R21", "2"), ("R30", "1"), ("R31", "1"),
                             ("R32", "1"), ("R35", "1")):
                self.assertEqual(
                    pin_nets.get((ref, pad)), "RF_DET_OUT",
                    f"{ref}.{pad} 掉出了 RF_DET_OUT",
                )
        # 生成源也要干净——网表干净但脚本里还留着，下次重新生成就会长回来
        tools = ROOT / "hardware" / "expansion-board-v3" / "tools"
        for script in ("sheet_rf1090.py", "route_rf.py", "PLACEMENT.py"):
            body = (tools / script).read_text(encoding="utf-8")
            with self.subTest(script=script, check="生成源不再产出 U13/R20"):
                self.assertNotRegex(
                    body, r'["\']U13["\']',
                    f"{script} 里还在引用 U13，重新生成会把它带回来",
                )
        # 制造输出的方向性错误：不能再出现"默认贴 U13 / AD8313 不贴"。
        #
        # 查的是 `internal/` 下的**生成产物**，不是仓库根目录那几份。V3 已归档，
        # 根目录的 CHECKLIST.md / ASSEMBLY.md 是冻结的公开快照（gen_checklist.py
        # 的注释写明了这件事），拿它们当判据只会一直红。
        #
        # "跳号"那一行要放过：删掉 U13/R20 之后位号确实缺号，产物如实记录
        # `U 缺 U13`，那是对的，不是残留。
        internal = ROOT / "hardware" / "expansion-board-v3" / "internal"
        for doc in ("CHECKLIST.md", "ASSEMBLY.md", "BOM_PURCHASE.md"):
            path = internal / doc
            if not path.is_file():
                continue
            offenders = [
                line for line in path.read_text(encoding="utf-8").splitlines()
                if ("U13" in line or "AD8319" in line) and "跳号" not in line
            ]
            with self.subTest(doc=doc, check="不再指示贴 AD8319"):
                self.assertFalse(
                    offenders,
                    f"internal/{doc} 仍在提 AD8319/U13：{offenders[:2]}",
                )
        # U14 必须落在"要贴"那一侧。它曾被 gen_bom_smt.py 的 SKIP_REF 整个跳过，
        # 板子回来会没有检波器——这是本阶段最实际的一个后果。
        checklist = internal / "CHECKLIST.md"
        if checklist.is_file():
            u14 = [line for line in checklist.read_text(encoding="utf-8").splitlines()
                   if "**U14**" in line]
            with self.subTest(check="U14 是必贴项"):
                self.assertTrue(u14, "CHECKLIST 里找不到 U14")
                self.assertNotIn("不贴", u14[0], f"U14 仍被标成不贴：{u14[0]}")

    def test_v4_pd_configuration_power_stage_and_debug_access(self):
        if "expansion-board-v4" not in BOARDS:
            self.skipTest("当前分支不维护 v4")
        pin_nets, values = self.schematic["expansion-board-v4"]
        expected = {
            ("U18", "9"): "PD_CFG1", ("U18", "2"): "PD_CFG2",
            ("U18", "3"): "PD_CFG3",
            ("R37", "1"): "PD_CFG1", ("R37", "2"): "GND",
            ("R48", "1"): "PD_CFG2", ("R48", "2"): "GND",
            ("R49", "1"): "PD_CFG3", ("R49", "2"): "GND",
            ("SW1", "1"): "RP_RUN", ("SW1", "2"): "GND",
        }
        debug_nets = {
            "TP1": "SWCLK", "TP2": "SWDIO", "TP3": "DEMOD0",
            "TP4": "DEMOD1", "TP5": "DEMOD2", "TP6": "DEMOD3",
            "TP7": "RECOVERED_CLK",
        }
        expected |= {(ref, "1"): net for ref, net in debug_nets.items()}
        self.assertEqual({key: pin_nets.get(key) for key in expected}, expected)
        self.assertEqual(values["R37"], "0R")
        self.assertEqual(values["R48"], "0R")
        self.assertEqual(values["R49"], "0R")
        self.assertEqual(values["L17"], "4.7uH XEL4030-472MEC")
        self.assertEqual(values["C77"], "22uF 16V X5R")
        for ref in ("R7", "R8"):
            self.assertIn("(dnp yes)", schematic_symbol("expansion-board-v4", "mcu", ref))
            self.assertTrue("dnp" in pcb_footprint("expansion-board-v4", ref).split("(attr", 1)[1].split(")", 1)[0])

    def test_v4_power_corrections(self):
        if "expansion-board-v4" not in BOARDS:
            self.skipTest("当前分支不维护 v4")
        pin_nets, values = self.schematic["expansion-board-v4"]
        self.assertEqual(pin_nets[("U18", "6")], "USB_CC2")
        self.assertEqual(pin_nets[("U18", "7")], "USB_CC1")
        self.assertEqual(values["C72"], "10uF")
        self.assertEqual(values["C80"], "1uF 25V")
        self.assertEqual(pin_nets[("C72", "1")], "CHG_PMID")
        self.assertEqual(pin_nets[("C72", "2")], "GND")
        self.assertEqual(pcb_pad_nets("expansion-board-v4", "U18")["6"], "USB_CC2")
        self.assertEqual(pcb_pad_nets("expansion-board-v4", "U18")["7"], "USB_CC1")

    def test_bno085_environment_bus_has_required_pullups(self):
        expected = {
            ("U4", "15"): "BNO_ENV_SCL", ("U4", "16"): "BNO_ENV_SDA",
            ("R52", "1"): "3V3_DIG", ("R52", "2"): "BNO_ENV_SCL",
            ("R53", "1"): "3V3_DIG", ("R53", "2"): "BNO_ENV_SDA",
        }
        for board, (pin_nets, values) in self.schematic.items():
            with self.subTest(board=board):
                self.assertEqual({key: pin_nets.get(key) for key in expected}, expected)
                self.assertEqual(values.get("R52"), "10k")
                self.assertEqual(values.get("R53"), "10k")
                self.assertLess(pad_distance(board, ("U4", "15"), ("R52", "2")), 4.0)
                self.assertLess(pad_distance(board, ("U4", "16"), ("R53", "2")), 4.0)
                if board == "expansion-board-v4":
                    for ref in ("R52", "R53"):
                        self.assertIn(
                            "R_0402_1005Metric",
                            pcb_footprint(board, ref),
                        )

    def test_bias_and_antenna_select_pullups_track_driver_rail(self):
        """四颗栅极上拉必须与驱动源同轨（3V3_DIG），不能挂在 3V3_GNSS 上。

        Q2/Q3/Q4/Q5 的源极都是 3V3_GNSS，把栅极上拉也接到 3V3_GNSS 看似能保证
        Vgs=0 关断，但驱动端是 RP2040 的 GPIO（3V3_DIG 域）：数字域掉电时
        3V3_GNSS 会经 10k 倒灌进未上电的 IO。改到 3V3_DIG 后这条路径消失，
        而上电时序仍安全——3V3_DIG 由 ME6211（无软启动）供给，3V3_GNSS 由
        TPS7A20（软启动 750-1150µs）供给，数字轨先建立，全程 Vgs >= 0。
        """
        expected = {
            ("R17", "1"): "3V3_DIG", ("R17", "2"): "BIAS_EN_978",
            ("R18", "1"): "3V3_DIG", ("R18", "2"): "BIAS_EN_1090",
            ("R26", "1"): "3V3_DIG", ("R26", "2"): "ANT_SEL_GNSS_A",
            ("R27", "1"): "3V3_DIG", ("R27", "2"): "ANT_SEL_GNSS_B",
        }
        sources = {("Q2", "2"), ("Q3", "2"), ("Q4", "2"), ("Q5", "2")}
        for board, (pin_nets, values) in self.schematic.items():
            if board != "expansion-board-v4":
                continue  # V3 在 Task 13（V3-BACKPORT）回灌
            with self.subTest(board=board):
                self.assertEqual({key: pin_nets.get(key) for key in expected}, expected)
                for ref in ("R17", "R18", "R26", "R27"):
                    self.assertEqual(values.get(ref), "10k")
                # 源极仍应全部在 3V3_GNSS：本修复只改电源端，不动功率路径。
                for key in sources:
                    self.assertEqual(pin_nets.get(key), "3V3_GNSS", key)

    def test_reset_chipselect_pullups_and_pulses_series_damping(self):
        """P1-14：IMU_RST/SUBG_CSN 在 MCU 复位期不能悬空；P2-10：PULSES 需源端阻尼。

        IMU_RST 经 J1.16 直连 ESP32-P4 的 GPIO，SUBG_CSN 接 RP2040 GPIO13，两者在
        主控复位期间都是高阻，BNO085 会被随机复位、CC1312R 会被误选中。各加 10k
        上拉到驱动侧的 3V3_DIG。

        PULSES 是 TLV3501 的输出（传播延迟 4.5ns、边沿极陡）横跨全板接到 RP2040，
        中间没有任何阻尼。串阻必须放在**源端**（紧贴 U15.5），放到 MCU 端起不到
        抑制反射的作用。插入后源端网络改名 PULSES_RAW，负载侧仍叫 PULSES。
        """
        expected = {
            ("R55", "1"): "3V3_DIG", ("R55", "2"): "IMU_RST",
            ("R56", "1"): "3V3_DIG", ("R56", "2"): "SUBG_CSN",
            ("U15", "5"): "PULSES_RAW",
            ("R57", "1"): "PULSES_RAW", ("R57", "2"): "PULSES",
            ("U8", "30"): "PULSES",
        }
        for board, (pin_nets, values) in self.schematic.items():
            if board != "expansion-board-v4":
                continue  # V3 在 Task 13（V3-BACKPORT）回灌
            with self.subTest(board=board):
                self.assertEqual({key: pin_nets.get(key) for key in expected}, expected)
                self.assertEqual(values.get("R55"), "10k")
                self.assertEqual(values.get("R56"), "10k")
                self.assertEqual(values.get("R57"), "33R")
                # R36 是 PULSES 的 1k 上拉，留在负载侧且保持 DNP：串阻与删上拉是两件事。
                self.assertEqual(pin_nets.get(("R36", "1")), "PULSES")
                self.assertIn("(dnp yes)", schematic_symbol(board, "rf1090", "R36"))

    def test_generators_carry_no_superseded_values(self):
        """P1-9：生成器里不得残留已被推翻的旧口径。

        这些值在网表里已经不存在（Y1 改成 ABM8-272-T3 CL=10pF、C19/C20 改成
        15pF C0G、CH224K 的 CFG1/2/3 改成三个 0R 拉低选 9V），所以旧条目属于
        死代码——但它们是**可执行**的死代码：PARTS/SPEC 表按 (值,封装) 匹配，
        一旦有人把晶振换回 C9002，33pF 会被静默配上去，没有任何提示。

        只扫 active generator。历史审计文档保留旧值作为反例是可以的。
        """
        generators = (
            "gen_bom.py", "gen_bom_smt.py", "gen_assembly.py",
            "gen_checklist.py", "gen_jlc_smt.py",
        )
        forbidden = (
            r"CL\s*=\s*20pF",
            r"C19/C20[^\n]*33pF",
            r"Y1[^\n]*33pF|33pF[^\n]*Y1",
            r"33pF[^\n]*晶振|晶振[^\n]*33pF",
            r"CH224K[^\n]*6\.8k[^\n]*9V|6\.8k[^\n]*CH224K[^\n]*9V",
        )
        tools = ROOT / "hardware" / "expansion-board-v4" / "internal" / "tools"
        for name in generators:
            path = tools / name
            self.assertTrue(path.is_file(), path)
            text = path.read_text(encoding="utf-8")
            for pattern in forbidden:
                with self.subTest(generator=name, pattern=pattern):
                    hit = re.search(pattern, text)
                    self.assertIsNone(
                        hit, f"{name} 残留旧口径：{hit.group(0) if hit else ''}")

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

    def test_v40_rework_guide_does_not_invert_correct_gnss_controls(self):
        guide = ROOT / "hardware" / "expansion-board-v4" / "internal" / "BOARD_TEST.md"
        text = guide.read_text(encoding="utf-8")
        self.assertIn("V4.0 实物返修与 V4.3 验收流程", text)
        # 认意图不认字面：文档 2026-08-31 二次核实后把措辞改成了语义更强的
        # 「禁止交换」，绑死「不交换」这四个字会把正确的文档判成违规。
        self.assertRegex(text, r"(不得|不要|禁止|不)交换 `U17 pin4/pin6`")
        self.assertNotRegex(text, r"(必须|需要|应)交换 `U17 pin4/pin6`")
        self.assertNotIn("必须把 U17 的 V1/V2 控制交叉", text)

        for board in BOARDS:
            readme = (ROOT / "hardware" / board / "README.md").read_text(encoding="utf-8")
            self.assertNotIn("GNSS antenna-bias ECO", readme, board)
            self.assertNotIn("requires the documented Gate rework", readme, board)

        v4_root = ROOT / "hardware" / "expansion-board-v4"
        if v4_root.is_dir():
            rebuild = (v4_root / "internal" / "tools" / "rebuild.sh").read_text(encoding="utf-8")
            # 判「有没有被调用」，不判「字符串出没出现」：rebuild.sh 的注释里
            # 会引用这个脚本名说明退役理由，裸 assertNotIn 会误杀那段注释。
            self.assertNotIn('$KP "$TOOLS/route_eco_v42.py"', rebuild)
            retired = (v4_root / "internal" / "tools" / "route_eco_v42.py").read_text(encoding="utf-8")
            self.assertIn("PK_ALLOW_RETIRED_V42_ECO", retired)

    def test_generated_process_docs_respect_dnp_and_bidirectional_usb_esd(self):
        for board in BOARDS:
            root = ROOT / "hardware" / board
            internal = root / "internal"
            checklist_path = internal / "CHECKLIST.md"
            assembly_path = internal / "ASSEMBLY.md"
            checklist = (checklist_path if checklist_path.is_file() else root / "CHECKLIST-zh_CN.md").read_text(encoding="utf-8")
            assembly = (assembly_path if assembly_path.is_file() else root / "ASSEMBLY-zh_CN.md").read_text(encoding="utf-8")
            r36_check = next(line for line in checklist.splitlines() if "**R36**" in line)
            self.assertIn("不贴", r36_check, board)
            self.assertIn("DNP", r36_check, board)
            self.assertIn("R36", next(line for line in assembly.splitlines() if "DNP" in line and "R36" in line), board)
            for ref in ("D4", "D5"):
                row = next(line for line in checklist.splitlines() if f"**{ref}**" in line)
                self.assertNotIn("有极性", row, f"{board}:{ref}")

    def test_public_english_process_docs_cover_current_board(self):
        for board in BOARDS:
            root = ROOT / "hardware" / board
            for stem in ("CHECKLIST", "ASSEMBLY"):
                chinese = (root / f"{stem}-zh_CN.md").read_text(encoding="utf-8")
                english = (root / f"{stem}.md").read_text(encoding="utf-8")
                refs = lambda text: set(re.findall(r"\*\*([A-Z]+\d+)\*\*", text))
                self.assertEqual(refs(english), refs(chinese), f"{board}/{stem}")
            checklist = (root / "CHECKLIST.md").read_text(encoding="utf-8")
            r36 = next(line for line in checklist.splitlines() if "**R36**" in line)
            self.assertIn("DNP", r36, board)
            self.assertIn("Not placed", r36, board)
            for ref in ("D4", "D5"):
                row = next(line for line in checklist.splitlines() if f"**{ref}**" in line)
                self.assertNotIn("Polarized", row, f"{board}:{ref}")

    def test_v4_generated_bom_keeps_variant_rule(self):
        if "expansion-board-v4" not in BOARDS:
            self.skipTest("当前分支不维护 v4")
        bom = ROOT / "hardware" / "expansion-board-v4" / "BOM_PURCHASE-zh_CN.md"
        text = bom.read_text(encoding="utf-8")
        row = next(line for line in text.splitlines() if "R7,R8" in line)
        self.assertIn("带电源版不贴", row)
        self.assertRegex(row, r"不带电源版必(?:须)?贴")
        self.assertNotIn("两颗都要", row)
        c71 = next(line for line in text.splitlines() if "C71" in line)
        self.assertNotIn("C15,C71", c71)
        self.assertIn("CHG_REGN", c71)
        self.assertIn("带电源版", c71)

        master = ROOT / "hardware" / "expansion-board-v4" / "internal" / "BOM_MASTER.md"
        master_text = master.read_text(encoding="utf-8")
        c46 = next(line for line in master_text.splitlines() if line.startswith("| C46 |"))
        self.assertIn("`C46219`", c46)
        self.assertNotIn("正确应为 `C696883`", c46)

    def test_route_cleanup_removes_dangling_tracks_and_vias(self):
        for board in BOARDS:
            root = ROOT / "hardware" / board
            candidates = (root / "tools" / "route_fix.py", root / "internal" / "tools" / "route_fix.py")
            route_fix = next(path for path in candidates if path.is_file())
            text = route_fix.read_text(encoding="utf-8")
            self.assertRegex(
                text,
                r'v\["type"\]\s+in\s+\("track_dangling",\s*"via_dangling"\)',
                route_fix,
            )
            self.assertIn("GetConnectedPads", text, route_fix)

    def test_freerouting_defaults_to_single_thread_for_deterministic_passes(self):
        for board in BOARDS:
            root = ROOT / "hardware" / board
            candidates = (root / "tools" / "run_route.sh", root / "internal" / "tools" / "run_route.sh")
            run_route = next(path for path in candidates if path.is_file())
            text = run_route.read_text(encoding="utf-8")
            self.assertIn('FR_THREADS="${PK_FR_THREADS:-1}"', text, run_route)
            self.assertIn('-mt "$FR_THREADS"', text, run_route)
            self.assertIn("--gui.enabled=false", text, run_route)
            self.assertIn("freerouting-2.3.0.jar", text, run_route)
            self.assertIn("3cf18d608437740bc497db6b8ef5888e2e60a08de0def20691d1bad0c0e0ee24", text, run_route)

            import_ses = next(path for path in (
                root / "tools" / "import_ses.py", root / "internal" / "tools" / "import_ses.py"
            ) if path.is_file()).read_text(encoding="utf-8")
            self.assertIn("m_TrackMinWidth", import_ses, board)
            self.assertIn("SetWidth(min_track_width)", import_ses, board)

    def test_schematic_erc_has_no_errors(self):
        for board in BOARDS:
            schematic = ROOT / "hardware" / board / "kicad" / f"{board}.kicad_sch"
            with tempfile.TemporaryDirectory(prefix=f"{board}-erc-") as tmp:
                report = Path(tmp) / "erc.json"
                subprocess.run(
                    [find_kicad_cli(), "sch", "erc", "--format", "json",
                     "-o", str(report), str(schematic)],
                    check=True,
                    capture_output=True,
                    text=True,
                )
                data = json.loads(report.read_text(encoding="utf-8"))
            errors = [
                violation
                for sheet in data.get("sheets", [])
                for violation in sheet.get("violations", [])
                if violation.get("severity") == "error"
            ]
            self.assertEqual(errors, [], board)

    def test_drc_classification_is_locale_independent(self):
        samples = {
            "need": {
                "items": [
                    {"description": "F.Cu 上 U8 的焊盘 1 [USB_DP]"},
                    {"description": "走线 [USB_DP] (F.Cu), 长度: 1.0 mm"},
                ]
            },
            "plane": {
                "items": [
                    {"description": "填充区[GND]在 F.Cu 上, 优先级 0"},
                    {"description": "F.Cu 上 U8 的焊盘 57 [GND]"},
                ]
            },
            "orphan": {
                "items": [
                    {"description": "过孔 [3V3_RF] (F.Cu - B.Cu)"},
                    {"description": "走线 [3V3_RF] (F.Cu), 长度: 1.0 mm"},
                ]
            },
        }
        for board in BOARDS:
            root = ROOT / "hardware" / board
            candidates = (root / "tools" / "drc_classify.py", root / "internal" / "tools" / "drc_classify.py")
            helper = next(path for path in candidates if path.is_file())
            spec = importlib.util.spec_from_file_location(f"drc_classify_{board}", helper)
            module = importlib.util.module_from_spec(spec)
            assert spec.loader is not None
            spec.loader.exec_module(module)
            for expected, item in samples.items():
                self.assertEqual(module.classify_unconnected_item(item), expected, board)


if __name__ == "__main__":
    unittest.main()
