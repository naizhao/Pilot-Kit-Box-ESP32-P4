#!/usr/bin/env python3
"""v4 PCB 的结构合同：功能丝印、命名 RF zone、板级图形数量和 B 面调试焊盘列。

默认检查 `hardware/expansion-board-v4/kicad/` 下的板。`PK_V4_PCB` 可以指向另一份
PCB（例如发布前的候选板），取绝对路径或相对仓库根的路径。
"""

from __future__ import annotations

from pathlib import Path
import json
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
    # V4.7：SW3 主电源键。这颗键是整机唯一的机械总电门（SY6970 /QON），
    # 盒子装好后按哪里全靠这条丝印指路，属功能性标注、必须在。
    "PWR BTN",
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
# V4.7 新增：用户 2026-09-12 摆 SW3 时加的 F.SilkS 标注。
# 跟 DEBUG_LABELS 一样单列，而不是把 FINALIZED_GRAPHICS_COUNT 改大——
# 基线数字一旦被"顺手 +1"，这条合同就退化成事后追认，再也拦不住误增误删。
V47_LABELS = ("PWR BTN",)
BOARD_GRAPHICS_COUNT = (FINALIZED_GRAPHICS_COUNT + len(DEBUG_LABELS)
                        + len(V47_LABELS))
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

    def test_zone_fill_params_are_frozen(self):
        """ZONES.json 必须给每个非规则区覆铜存齐填充参数。

        拦的是一类 **DRC 查不出来** 的回归：板上 clearance 是 0.15，而 gen_pcb.py
        若不从 ZONES.json 读回就吃 KiCad 默认 0.5，覆铜多退 0.35mm，靠覆铜连通的
        焊盘/过孔当场断开——2026-09-12 首次完整重放同口径比对实测到 缺线 2 /
        孤立断头 4 / 焊盘未接覆铜 4 / via_dangling 1，而 kicad-cli drc 全程 0 违例
        （默认值更宽松，不违反任何规则）。

        这条只校验**固化数据本身**。"板上实际值 == 固化值" 由 gen_pcb.py 里的
        断言把关（它用 pcbnew API，重建时必然执行，比在这里解析 PCB 文本可靠——
        KiCad 10 的 zone 块里既无 net_name、顶部也无 (net N "name") 声明，
        按文本解析口径极易写成恒真的空断言）。
        """
        zjson = ROOT / "hardware" / "expansion-board-v4" / "internal" / "tools" / "ZONES.json"
        self.assertTrue(zjson.is_file(), "缺 ZONES.json")
        data = json.loads(zjson.read_text())
        need = ("clearance", "min_thickness", "pad_connection",
                "thermal_gap", "thermal_spoke", "island_removal")
        n = 0
        for z in data["zones"]:
            if z["rule_area"]:
                self.assertNotIn("fill", z, f"规则区 {z['name']} 不该有 fill")
                continue
            who = f"{z['net']}@{z['layer']}"
            self.assertIn("fill", z, f"{who} 缺 fill 参数，跑 export_zones.py 重新固化")
            for k in need:
                self.assertIn(k, z["fill"], f"{who} 的 fill 缺字段 {k}")
            # 0.5 是 KiCad 的默认 clearance。固化值等于默认值时，"漏读固化数据"
            # 这个 bug 就不再有可观测后果，这条门禁也就失去意义——真要改成 0.5，
            # 请连这条断言一起改，并说明为什么不再需要它。
            self.assertNotAlmostEqual(
                z["fill"]["clearance"], 0.5, places=3,
                msg=f"{who} 的固化 clearance 等于 KiCad 默认 0.5，"
                    f"这条门禁将无法再发现'漏读固化参数'的退化")
            n += 1
        self.assertGreaterEqual(n, 10, f"只校验到 {n} 个覆铜，ZONES.json 可能不完整")

    def test_board_rev_silk_matches_single_source(self):
        """板面版本丝印必须等于 board_meta.BOARD_REV。

        board_meta.py 的文件头写着"改版本号只改这一个文件"，并列了历史上三处
        写死副本的教训。但品牌丝印是个例外：它的位置被手工调过、已固化进
        SILK.json，而 gen_brand_silk.py 是**按 y 坐标**清旧的——那条被挪到
        y=102.46 的版本文字不在它的槽位里，重跑只会叠加出第二条
        （2026-09-12 实测：板上同时出现 'V4.6 (2026-09)' 和 'V4.7   2026-09'）。

        所以版本丝印的实际内容存在 SILK.json 里，改版本号要改两处。这条门禁
        就是补上那个"记得同步"——让漏改当场变红，而不是流到 Gerber 里。
        """
        import importlib.util
        bm_path = (ROOT / "hardware" / "expansion-board-v4" / "internal"
                   / "tools" / "board_meta.py")
        spec = importlib.util.spec_from_file_location("board_meta", bm_path)
        bm = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(bm)

        # ⚠️ 不校验 SILK.json 里的版本文字：silk_texts.py:142 在 import 时会用
        # REVISION 正则把它替换成 board_meta.BOARD_REV，所以固化数据里存的是
        # 哪一版都无所谓、也不该手动维护。真源始终只有 board_meta.py 一处。
        # （我一度以为要手改 SILK.json 并为此加了断言，那条约束是错的。）
        # 这里只查板子：版本必须一致，且必须恰好一条。
        on_board = re.findall(r'\(gr_text "(V\d+\.\d+[^"]*)"', self.text)
        self.assertEqual(len(on_board), 1,
                         f"PCB 上版本丝印应恰好 1 条，实际 {on_board}")
        self.assertTrue(on_board[0].startswith(bm.BOARD_REV),
                        f"PCB 上是 {on_board[0]!r}，BOARD_REV 是 {bm.BOARD_REV!r}")

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
        # 只判存在性不判时间戳，等于拿旧数据去证明新板子是对的。
        # 原理图比网表新时，下面几条「与原理图一致」的契约都会基于过期数据判断。
        # 陈旧就直接失败，不 skip —— skip 会被当成绿灯一路放行。
        sch_dir = ROOT / "hardware" / "expansion-board-v4" / "kicad"
        newest = max((p.stat().st_mtime for p in sch_dir.glob("*.kicad_sch")),
                     default=0.0)
        if newest > netlist.stat().st_mtime:
            self.fail("build/netlist.xml 比原理图旧，先重新导出网表再跑本测试")
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

    def test_schematic_has_no_component_missing_from_the_pcb(self):
        """原理图里的元件必须都落到 PCB 上。

        这是反向检查。本类其余几条「与原理图一致」的契约都是「遍历 PCB 上的
        封装，再拿原理图的值来比对」，而 `values.get(reference)` 取不到就
        `continue` —— 于是「原理图有、PCB 没有」这一整类问题从不被触发。

        若缺少这条检查，原理图新增元件可能不触发现有正向契约，直到重新布板时
        才悄悄进入 PCB。
        """
        _, values, _ = self._schematic_index()
        # 只看真实位号：#PWR/#FLG 这类电源与旗标符号本就不上板，
        # 而 H1–H4 是 PCB 独有的安装孔（反方向，不归本条管）。
        want = {r for r in values if re.fullmatch(r"[A-Z]+\d+", r)}
        missing = sorted(want - set(self.footprints))
        self.assertEqual(missing, [], "原理图有这些元件，PCB 上却没有")

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
