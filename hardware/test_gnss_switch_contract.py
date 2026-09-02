#!/usr/bin/env python3
"""v3/v4 GNSS RF 选择与有源天线 bias 的结构回归测试。

依据 AS179-92LF 与实际装配 XA17-G4K 的真值表：

* V1=Low、V2=High：J1-J2 导通，因此外接 J2 的低有效 PMOS Q4 gate 必须接 V1。
* V1=High、V2=Low：J1-J3 导通，因此内置 J3 的低有效 PMOS Q5 gate 必须接 V2。

两颗器件的真值表逐行一致，这一点直接从手册 PDF 正文表格解析验证，不靠转述：
新版 AS179 手册（200176J, 2022）删掉了真值表，只看方框图或搜索摘要会得出相反结论。

测试通过 KiCad CLI 从最终层级原理图临时导出 XML 网表，不读取生成脚本字符串，
避免出现“脚本改了但忘记重新生成原理图”的假通过。

运行：python3 hardware/test_gnss_switch_contract.py
"""

from pathlib import Path
import os
import re
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

DATASHEETS = ROOT / "hardware" / "datasheets"
XA17_DATASHEET = DATASHEETS / "XA17-G4K_lcsc.pdf"
AS179_DATASHEET = DATASHEETS / "AS179-92LF_rev2004_truthtable.pdf"
SWITCH_REFERENCES = ("U16", "U17")
SWITCH_VALUE = "XA17-G4K"
# U16/U17 只认 XA17-G4K 单一料号：真值表相同，但供应链/BOM 决定不列双源。
# V3 在 Task 13（V3-BACKPORT）回灌后加入此元组。
XA17_ONLY_BOARDS = ("expansion-board-v4",)
# 生产/装配文本禁止继续出现的直接替代口径。
DUAL_SOURCE_TEXTS = ("XA17-G4K(或AS179-92LF)", "XA17-G4K(or AS179-92LF)")


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


def export_netlist(board: str) -> tuple[dict[tuple[str, str], str], dict[str, str]]:
    """返回 (pin -> net, 位号 -> value)。"""
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

    pin_nets: dict[tuple[str, str], str] = {}
    for net in root.findall("./nets/net"):
        name = net.attrib["name"]
        for node in net.findall("node"):
            pin_nets[(node.attrib["ref"], node.attrib["pin"])] = name
    values = {
        comp.attrib["ref"]: comp.findtext("value", "")
        for comp in root.findall("./components/comp")
    }
    return pin_nets, values


def pdf_text(path: Path) -> str:
    tool = shutil.which("pdftotext")
    if not tool:
        raise unittest.SkipTest("需要 pdftotext 才能核对手册真值表")
    return subprocess.run(
        [tool, "-layout", str(path), "-"],
        check=True, capture_output=True, text=True,
    ).stdout


def parse_truth_table(text: str) -> dict[tuple[bool, bool], tuple[str, str]]:
    """把手册真值表正文解析成 {(V1高, V2高): (J1-J2 状态, J1-J3 状态)}。

    两份手册用词不同（Low/High vs 0/VHIGH、Insertion Loss vs Insertion loss），
    统一归一化为 through/isolated 后才能逐行比较。AS179 2004 版把 Pin Out 图排在
    真值表右侧，同一文本行尾部还跟着引脚编号，所以第四列之后不能锚定行尾。
    """
    levels = {"low": False, "0": False, "high": True, "vhigh": True}
    states = {"insertion loss": "through", "isolation": "isolated"}
    pattern = re.compile(
        r"^\s*(Low|High|VHIGH|0)\s{2,}(Low|High|VHIGH|0)\s{2,}"
        r"(Insertion Loss|Insertion loss|Isolation)\s{2,}"
        r"(Insertion Loss|Insertion loss|Isolation)(?=\s|$)",
        re.MULTILINE,
    )
    table = {
        (levels[m.group(1).lower()], levels[m.group(2).lower()]):
            (states[m.group(3).lower()], states[m.group(4).lower()])
        for m in pattern.finditer(text)
    }
    if len(table) != 2:
        raise AssertionError(f"手册真值表解析失败，得到 {table}")
    return table


def generation_sources() -> dict[Path, str]:
    """参与生产/装配文本生成的脚本；只覆盖 v4，V3 在 Task 13 回灌。"""
    tools = ROOT / "hardware" / "expansion-board-v4" / "internal" / "tools"
    return {
        path: path.read_text(encoding="utf-8")
        for path in sorted(tools.glob("*.py"))
    }


def extract_sexpr(text: str, start: int) -> str:
    """提取从 ``start`` 左括号开始的一个完整 KiCad S-expression。"""
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
                return text[start : index + 1]
    raise ValueError(f"未闭合的 S-expression，起点 {start}")


def load_pcb_pad_nets(board: str) -> dict[tuple[str, str], str]:
    pcb = ROOT / "hardware" / board / "kicad" / f"{board}.kicad_pcb"
    text = pcb.read_text(encoding="utf-8")
    result: dict[tuple[str, str], str] = {}
    for reference in ("Q4", "Q5"):
        marker = f'(property "Reference" "{reference}"'
        marker_at = text.index(marker)
        footprint_at = text.rfind("\n\t(footprint ", 0, marker_at) + 2
        footprint = extract_sexpr(text, footprint_at)
        for match in re.finditer(r'\(pad "([^"]+)" ', footprint):
            pad = extract_sexpr(footprint, match.start())
            net_match = re.search(r'\(net "([^"]+)"\)', pad)
            if net_match:
                result[(reference, match.group(1))] = net_match.group(1)
    return result


class GnssSwitchContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        netlists = {board: export_netlist(board) for board in BOARDS}
        cls.pin_nets = {board: nets for board, (nets, _) in netlists.items()}
        cls.values = {board: values for board, (_, values) in netlists.items()}
        cls.pcb_pad_nets = {board: load_pcb_pad_nets(board) for board in BOARDS}

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

    def test_xa17_and_as179_truth_tables_are_identical(self):
        """禁止再用“真值表相反”当作禁替理由：直接对照两份手册的表格原文。"""
        xa17 = parse_truth_table(pdf_text(XA17_DATASHEET))
        as179 = parse_truth_table(pdf_text(AS179_DATASHEET))
        self.assertEqual(xa17, as179)
        # 控制脚互补，且 V1 高时导通的是 J1-J3——Q4/Q5 的交叉栅极接法依赖这一行。
        self.assertEqual(xa17[(True, False)], ("isolated", "through"))
        self.assertEqual(xa17[(False, True)], ("through", "isolated"))

    def test_rf_switches_are_xa17_only(self):
        for board in XA17_ONLY_BOARDS:
            if board not in BOARDS:
                continue
            for reference in SWITCH_REFERENCES:
                with self.subTest(board=board, reference=reference):
                    self.assertEqual(self.values[board][reference], SWITCH_VALUE)

    def test_generation_sources_do_not_offer_as179_as_drop_in(self):
        for path, text in generation_sources().items():
            for dual in DUAL_SOURCE_TEXTS:
                with self.subTest(path=path.name, text=dual):
                    self.assertNotIn(dual, text)

    def test_final_pcb_gate_pad_nets_match_schematic(self):
        expected = {
            ("Q4", "1"): "ANT_SEL_GNSS_A",
            ("Q5", "1"): "ANT_SEL_GNSS_B",
        }
        for board, pad_nets in self.pcb_pad_nets.items():
            with self.subTest(board=board):
                self.assertEqual(
                    {key: pad_nets[key] for key in expected},
                    expected,
                )


if __name__ == "__main__":
    unittest.main()
