#!/usr/bin/env python3
"""手工贴片网页（web/assembly/）数据产物的契约。

产物由 `hardware/tools/gen_assembly_web.py` 生成——那个脚本必须用 KiCad 自带的
Python 跑（要 `import pcbnew`），而本仓的硬件测试跑在 `/usr/bin/python3` 上。
所以分工是：**脚本产出 JSON，本测试只校验 JSON**，两边不共享解释器。

重生成：

    hardware/tools/gen_assembly_web.py

## 这份契约要挡住什么

网页是拿着烙铁照着贴的，错一个位号就是一个焊错的件。三类错误必须挡住：

1. **电源变体贴反** —— 带电源版和不带电源版的 `R7`/`R8` 逻辑相反
   （见 `expansion-board-v4/SELECTIVE_PLACEMENT-zh_CN.md` ①）。贴错会烧板。
2. **DNP 件混进待贴列表** —— `R30` 那次就是手抄 BOM 时漏掉，
   没有任何东西报错，照着表贴会让 3V3_RF 与 3V3_DIG 直流相连。
3. **焊盘几何缺失或跑到板外** —— 高亮框指错位置比不高亮更坏。
"""

from __future__ import annotations

import collections
import itertools
import json
import math
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "web" / "assembly" / "data"

# 变体 id → (板子目录名, 期望的位号总数下限)
# 数量写下限而不是精确值：改板加件是常态，加了件不该让这份测试红，
# 但整批消失（解析挂了、过滤写反）一定要红。
VARIANTS = {
    # V4.11 是当前板（新增 SW3 单向软件感知），V4.10 起为存档，
    # 但图还可能被人拿去贴板，一样要过全部契约。
    "v4.11-pwr": "expansion-board-v4",
    "v4.11-nopwr": "expansion-board-v4",
    "v4.10-pwr": "expansion-board-v4",
    "v4.10-nopwr": "expansion-board-v4",
    "v4.9-pwr": "expansion-board-v4",
    "v4.9-nopwr": "expansion-board-v4",
    "v4.8-pwr": "expansion-board-v4",
    "v4.8-nopwr": "expansion-board-v4",
    "v4.7-pwr": "expansion-board-v4",
    "v4.7-nopwr": "expansion-board-v4",
    # V4.6 起同样是存档：V4.7 的 PCB 已覆盖它，但 V4.6 的图还可能被人拿去贴板
    "v4.6-pwr": "expansion-board-v4",
    "v4.6-nopwr": "expansion-board-v4",
    # V4.5 是存档数据（PCB 已被 V4.6 覆盖，重算不出来），但一样要过全部契约——
    # 它还在被用来贴 V4.4/V4.5 的实板，坏了同样会贴错。
    "v4.5-pwr": "expansion-board-v4",
    "v4.5-nopwr": "expansion-board-v4",
    "v3": "expansion-board-v3",
}

# 带电源/不带电源成对出现的版本。加新版本时往这里加一对，
# 别再往下面的用例里塞字面量。
PWR_PAIRS = [("v4.11-pwr", "v4.11-nopwr"), ("v4.10-pwr", "v4.10-nopwr"), ("v4.9-pwr", "v4.9-nopwr"),
             ("v4.8-pwr", "v4.8-nopwr"), ("v4.7-pwr", "v4.7-nopwr"),
             ("v4.6-pwr", "v4.6-nopwr"), ("v4.5-pwr", "v4.5-nopwr")]

# SELECTIVE_PLACEMENT-zh_CN.md ①：电源部分，两个版本逻辑相反。
#
# ⚠️ 电源区的成员**随版本变**，不能只留一个全局集合：`SW3`（SY6970 的 /QON
# 电源键）是 V4.7 才加的，V4.5/V4.6 的板上没有这个位号。2026-09-13 把 SW3
# 加进电源区后，这份测试对 v4.5/v4.6 两套存档数据报「缺电源区位号 SW3」——
# 那不是数据坏了，是判据把新版的成员套到了旧版头上。
# 加新版本时：往 VARIANTS / PWR_PAIRS 各加一对，若引入新的电源区位号，
# 在 POWER_REFS_EXTRA 里按版本前缀登记，**不要**直接改 POWER_REFS_BASE。
POWER_REFS_BASE = (
    {"U18", "U19", "U20", "L16", "L17", "J9", "RT1"}
    | {f"C{n}" for n in range(70, 81)}
    | {f"R{n}" for n in range(37, 47)}
)
# v4.7 加 SW3 电源键；v4.8 起再加 GNSS VBAT 常供电整组（U23/C101/C102/D7/R63/BT1）
# 与充电指示灯（D9/R65）。D8/R64（电源指示灯）**不在**电源区：它接 3V3_DIG，
# 不带电源版照样有电、照样该亮——判据见 gen_assembly_web.py 的 POWER_SECTION。
# 注意 BT1 虽是手焊件，但它是 BOM 上的真器件，两个变体的数据里都有它，
# 「nopwr 一个不贴」对它同样成立。
POWER_REFS_EXTRA = {
    "v4.11": {"SW3", "U23", "C101", "C102", "D7", "R63", "BT1", "D9", "R65", "R67", "D10"},
    "v4.7": {"SW3"},
    "v4.8": {"SW3", "U23", "C101", "C102", "D7", "R63", "BT1", "D9", "R65"},
    "v4.9": {"SW3", "U23", "C101", "C102", "D7", "R63", "BT1", "D9", "R65"},
    "v4.10": {"SW3", "U23", "C101", "C102", "D7", "R63", "BT1", "D9", "R65"},
}

# 电源区里**允许**出现在不带电源版上的位号。
#
# 2026-09-13 一度为 {"SW3"}：当时 `v4.7-nopwr.json` 的说明写着"贴上无害，
# 装壳时按了没反应属正常"，而 `gen_jlc_smt.py` / `SELECTIVE_PLACEMENT.md`
# 把它算电源件 —— 两个事实源打架。重出装配站点数据后（生成器用的就是含 SW3
# 的 VARIANT_PWR）nopwr 已不含 SW3，分歧消解，**豁免随即撤掉，断言恢复严格**。
# 留这个空集合是给将来用的：只有"贴反会烧板"级别的位号才该走硬断言，
# 纯粹"按了没反应"的件若要放行，往这里加，别去放宽上面的集合。
POWER_REFS_OPTIONAL_ON_NOPWR: set[str] = set()


def power_refs(variant_id):
    rev = variant_id.rsplit("-", 1)[0]
    return POWER_REFS_BASE | POWER_REFS_EXTRA.get(rev, set())
# 5.1k CC 下拉，跟电源区反向：带电源版绝不能贴，不带电源版必须贴。
CC_PULLDOWN_REFS = {"R7", "R8"}
# 两个版本都不贴：天线通路旁路 + IFA 并联匹配 + AD8313 输出上拉
# + R66（26MHz 探测点隔离，贴上给 TCXO 加约 17pF，超出手册负载预算）。
ALWAYS_DNP_REFS = {"R24", "R25", "R30", "R36", "ZP1", "ZP2", "R66"}


def load(variant: str) -> dict:
    path = DATA / f"{variant}.json"
    if not path.exists():
        raise unittest.SkipTest(f"{path} 不存在，先跑 hardware/tools/gen_assembly_web.py")
    return json.loads(path.read_text(encoding="utf-8"))


def refs_of(doc: dict) -> set[str]:
    return {p["ref"] for g in doc["groups"] for p in g["parts"]}


def pad_span(part: dict) -> float:
    """焊盘包络的对角线长度——判断两个件是不是同一个物理尺寸。"""
    pts = [q for pad in part["pads"] for q in pad["poly"]]
    if not pts:
        return 0.0
    xs = [q[0] for q in pts]
    ys = [q[1] for q in pts]
    return math.hypot(max(xs) - min(xs), max(ys) - min(ys))


def ref_key(ref: str):
    """位号的自然顺序键。独立实现，不 import 生产代码。"""
    m = re.match(r"^([A-Za-z]+)(\d+)$", ref)
    return (m.group(1), int(m.group(2))) if m else (ref, 0)


def _board_rev(variant_id: str):
    """变体 id → 板本号元组：'v4.10-pwr' → (4, 10)。按数字比，别按字符串比
    （'v4.10' < 'v4.8' 在字典序里是反的）。"""
    m = re.match(r"^v(\d+)\.(\d+)", variant_id)
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


def body_area(part: dict) -> float:
    """元件本体的占地面积，优先用 courtyard。

    跟 `pad_span` 是两个判据，别混用：
    - 「同一料号尺寸是否一致」看**焊盘**——焊盘决定这个件能不能贴上去
    - 「先大后小」看**本体**——热容和对位难度由元件外框决定

    SOT-363 这类封装外框小但焊盘分散，两个判据会给出相反的顺序。
    """
    poly = part["courtyard"] or [q for pad in part["pads"] for q in pad["poly"]]
    if not poly:
        return 0.0
    xs = [q[0] for q in poly]
    ys = [q[1] for q in poly]
    return (max(xs) - min(xs)) * (max(ys) - min(ys))


class AssemblyWebDataContract(unittest.TestCase):
    def test_current_board_is_first_in_version_selector(self):
        index = json.loads((DATA / "index.json").read_text(encoding="utf-8"))
        self.assertEqual([item["id"] for item in index[:2]],
                         ["v4.11-pwr", "v4.11-nopwr"])
        for item in index[:2]:
            self.assertEqual(item["total"], load(item["id"])["total"])

    def test_v411_sw3_sense_parts_follow_power_variant(self):
        for variant in ("v4.11-pwr", "v4.11-nopwr"):
            path = DATA / f"{variant}.json"
            self.assertTrue(path.exists(), f"缺当前板数据：{path}")
            parts = {p["ref"]: (p, g) for g in load(variant)["groups"] for p in g["parts"]}
            for ref, value, lcsc in (("R67", "1k", "C21190"),
                                     ("D10", "1N4148WS", "C2128")):
                with self.subTest(variant=variant, ref=ref):
                    if variant.endswith("-nopwr"):
                        self.assertNotIn(ref, parts)
                    else:
                        self.assertIn(ref, parts)
                        part, group = parts[ref]
                        self.assertEqual(part["val"], value)
                        self.assertEqual(group["lcsc"], lcsc)


    def test_all_variants_present(self):
        missing = [v for v in VARIANTS if not (DATA / f"{v}.json").exists()]
        self.assertEqual(missing, [], f"缺变体数据：{missing}")

    def test_power_variant_logic_is_inverted(self):
        """带电源版贴电源区不贴 R7/R8；不带电源版正好相反。贴反会烧板。"""
        for pwr_id, nopwr_id in PWR_PAIRS:
            with self.subTest(pair=pwr_id):
                pwr = refs_of(load(pwr_id))
                nopwr = refs_of(load(nopwr_id))
                expect = power_refs(pwr_id)

                self.assertTrue(
                    expect <= pwr,
                    f"{pwr_id} 缺电源区位号：{sorted(expect - pwr)}",
                )
                forbidden = expect - POWER_REFS_OPTIONAL_ON_NOPWR
                self.assertEqual(
                    forbidden & nopwr, set(),
                    f"{nopwr_id} 混进了电源区位号：{sorted(forbidden & nopwr)}",
                )
                self.assertEqual(
                    CC_PULLDOWN_REFS & pwr, set(),
                    f"{pwr_id} 混进了 CC 下拉 {sorted(CC_PULLDOWN_REFS & pwr)}"
                    f"——与 CH224K 并存会烧板",
                )
                self.assertTrue(
                    CC_PULLDOWN_REFS <= nopwr,
                    f"{nopwr_id} 缺 CC 下拉：{sorted(CC_PULLDOWN_REFS - nopwr)}"
                    f"——插电脑不认，刷不进固件",
                )

    def test_dnp_never_listed(self):
        for variant in VARIANTS:
            with self.subTest(variant=variant):
                got = refs_of(load(variant)) & ALWAYS_DNP_REFS
                self.assertEqual(got, set(), f"{variant} 列出了设计上不贴的件：{sorted(got)}")

    def test_no_placeholder_refs(self):
        """ANT1 是 PCB 铜箔、SW2 是已删的焊盘跳线、H*/FID*/TP* 没有器件，
        不该出现在贴片流程里；J11 自 V4.10 起是裸焊盘阵列，同样不该出现。

        ⚠️ J11 禁令**只对 V4.10+ 生效**：V4.8/V4.9 板上 J11 是 2x8 贴片排针，
        是要贴的真器件（v4.9 的数据里它照贴）。一刀切会让存档数据假红——
        V4.10 改板踩过：生成文档一侧把裸焊盘 J11 当成要贴的件（总数 219 vs
        web 的 218），这条守卫就是那时加的。
        """
        for variant in VARIANTS:
            with self.subTest(variant=variant):
                refs = refs_of(load(variant))
                banned = {"ANT1", "SW2"}
                if _board_rev(variant) >= (4, 10):
                    banned.add("J11")
                bad = {r for r in refs
                       if r in banned or r.startswith(("H", "FID", "TP"))}
                self.assertEqual(bad, set(), f"{variant} 列出了无器件位号：{sorted(bad)}")

    def test_every_part_has_pads_inside_the_board(self):
        """焊盘几何缺失或跑到板外，高亮就会指错地方——比不高亮更坏。"""
        for variant in VARIANTS:
            doc = load(variant)
            w, h = doc["board"]["w"], doc["board"]["h"]
            for group in doc["groups"]:
                for part in group["parts"]:
                    with self.subTest(variant=variant, ref=part["ref"]):
                        self.assertTrue(part["pads"], f"{part['ref']} 没有焊盘几何")
                        for pad in part["pads"]:
                            self.assertGreaterEqual(len(pad["poly"]), 3,
                                                    f"{part['ref']} 的焊盘不是多边形")
                            for x, y in pad["poly"]:
                                self.assertTrue(
                                    -1 <= x <= w + 1 and -1 <= y <= h + 1,
                                    f"{part['ref']} 焊盘 ({x}, {y}) 落在板外 {w}x{h}",
                                )

    def test_groups_are_one_physical_reel(self):
        """同料聚合的前提：一组 = 一盘料，否则「准备 23 个 100nF」就是错的。

        判据是**焊盘尺寸**，不是封装库名。名字不可靠：`ZS1` 和 `R21` 都是
        0603 的 0R、同一个料号 C21189，但 ZS1 在 PCB 上挂的是
        `L_0603_1608Metric`（π 网络那个位置设计上可以贴 0R 或电感）。
        按名字判会把同一盘料拆成两组；按尺寸判才对得上桌上的实物。

        反过来，0402 和 0603 合成一组是真错误——人会照着拿错，
        而且贴上去之前看不出来。1.5 倍留给同尺寸封装的焊盘画法差异。
        """
        for variant in VARIANTS:
            doc = load(variant)
            for group in doc["groups"]:
                with self.subTest(variant=variant, group=group["key"]):
                    self.assertTrue(group["parts"], f"空组 {group['key']}")
                    self.assertEqual(group["qty"], len(group["parts"]),
                                     f"{group['key']} 的数量与位号数对不上")
                    spans = [pad_span(p) for p in group["parts"]]
                    if min(spans) > 0:
                        self.assertLessEqual(
                            max(spans) / min(spans), 1.5,
                            f"{group['key']} 混了不同尺寸的件："
                            f"{max(spans):.2f}mm vs {min(spans):.2f}mm "
                            f"（{sorted({p['fp'] for p in group['parts']})}）",
                        )

    def test_dielectric_is_single_valued_per_group(self):
        """一组只能有一种介质。

        0402 的 100pF C0G 和 100pF X7R 外观完全一样，混在桌上分不回来，
        而 `C34`/`C35` 决定 1090MHz 检波输入高通、必须锁 C0G/NP0。
        一个料号解析出两种介质就是数据出问题了，照着贴会拿错料。
        """
        known = {"C0G", "X7R", "X5R", "X6S", "X7S", "Y5V", "Z5U", ""}
        for variant in VARIANTS:
            doc = load(variant)
            for group in doc["groups"]:
                with self.subTest(variant=variant, group=group["key"]):
                    self.assertIn(group.get("dielectric", ""), known,
                                  f"{group['key']} 的介质代号不认识")

    def test_merged_groups_keep_per_position_use(self):
        """合并同料号的多行后，每个位号自己的用途不能丢。

        `C41432122` 那款 U.FL 座在板上有 5 个，分别是 GNSS 外接 / 978 /
        1090 外接 / IFA 调试口 / 内置 patch——**接错口就是接错天线**。
        合并后组标题只剩公共部分「U.FL」，用途必须逐个留在位号上。
        """
        doc = load("v4.10-pwr")
        merged = [g for g in doc["groups"] if g.get("spec_variants")]
        self.assertTrue(merged, "没有任何合并组，用例失去意义（分组逻辑是不是退回按规格分了？）")
        for group in merged:
            with self.subTest(group=group["key"]):
                # 跟组标题一样的用途是冗余的，故意不标（省得每行重复一遍 "100pF"）。
                # 要挡住的是**有差异的用途被合并吞掉**。
                distinct = {v for v in group["spec_variants"] if v != group["spec"]}
                shown = {p["use"] for p in group["parts"] if p.get("use")}
                self.assertEqual(
                    distinct - shown, set(),
                    f"{group['key']} 合并后丢了这些用途：{sorted(distinct - shown)}",
                )

        # U.FL 是最要命的一组：座子全是不同的射频口，接错就是接错天线。
        # V4.6 起加 airband 的 J10 后是 6 个（此前 5 个）。单独钉死，
        # 防止将来有人"优化"掉逐位号的用途标注。
        ufl = next((g for g in doc["groups"] if g["lcsc"] == "C41432122"), None)
        self.assertIsNotNone(ufl, "板上找不到 U.FL 座那一组")
        self.assertEqual(len(ufl["parts"]), 6,
                         "U.FL 座应该是 6 个（含 V4.6 airband 的 J10）")
        self.assertEqual(
            len({p["use"] for p in ufl["parts"]}), 6,
            "6 个 U.FL 座必须各自标出用途"
            "（GNSS外接/978/1090外接/IFA调试/内置patch/VHF）",
        )

    def test_refs_unique_across_groups(self):
        """一个位号只能属于一组，否则总进度会把它数两次。"""
        for variant in VARIANTS:
            doc = load(variant)
            seen: dict[str, str] = {}
            for group in doc["groups"]:
                for part in group["parts"]:
                    with self.subTest(variant=variant, ref=part["ref"]):
                        self.assertNotIn(
                            part["ref"], seen,
                            f"{part['ref']} 同时在 {seen.get(part['ref'])} 和 {group['key']}",
                        )
                        seen[part["ref"]] = group["key"]
            self.assertEqual(doc["total"], len(seen), "total 与实际位号数对不上")

    def test_kinds_are_contiguous(self):
        """同一元件大类的组必须连成一段，不能交错。

        这是 2026-09-09 用户实际贴片时提的：原来只按封装面积降序排，
        结果是 0805电容 → 0603电阻 → 0603电容 → 0402电阻。工艺上说得通，
        但**手边一盒电容，贴两个就要去翻电阻盒**，找料成本压倒一切。

        对**位号**序列判，不是对组判。这一点是浏览器实测抓出来的：本条一开始
        只查 `group["kind"]`（组内取多数），而 `C21189` 那盘 0R 里 ZS1 归
        「匹配网络」、其余归「电阻」——组排在电阻段，组内那一个件却属于另一类，
        把电阻段劈成两截。组级检查完全看不见，只有展平成实际推进序列才暴露。
        """
        for variant in VARIANTS:
            doc = load(variant)
            # 展平成用户实际按空格走过的顺序
            kinds = [p["kind"] for g in doc["groups"] for p in g["parts"]]
            runs = [k for k, _ in itertools.groupby(kinds)]
            dupes = [k for k, n in collections.Counter(runs).items() if n > 1]
            with self.subTest(variant=variant):
                self.assertEqual(
                    dupes, [],
                    f"{variant} 这些大类在推进序列里被切成多段：{dupes}\n顺序：{runs}",
                )

    def test_part_kind_matches_its_group(self):
        """位号的大类必须跟它所在的组一致。

        分类的粒度跟「一盘料」对齐——分类导航是用来找料的，同一盘料的件
        不该散在两个分类里。这条防止有人改回「每个位号各自按前缀归类」。
        """
        for variant in VARIANTS:
            doc = load(variant)
            for group in doc["groups"]:
                bad = [p["ref"] for p in group["parts"] if p["kind"] != group["kind"]]
                with self.subTest(variant=variant, group=group["key"]):
                    self.assertEqual(
                        bad, [],
                        f"{group['key']}（{group['kind']}）里这些位号的大类不一致：{bad}",
                    )

    def test_sizes_descend_within_each_kind(self):
        """段内保持「先大后小」。

        大类分段是一级，工艺原则退到二级但仍要生效：同类里大件先贴，
        热风吹外围才不会掀翻已贴的小件。
        """
        for variant in VARIANTS:
            doc = load(variant)
            for kind, grp in itertools.groupby(doc["groups"], key=lambda g: g["kind"]):
                areas = [max(body_area(p) for p in g["parts"]) for g in grp]
                with self.subTest(variant=variant, kind=kind):
                    # 允许 1% 的浮点毛刺，但不允许小件排在大件前面
                    bad = [(a, b) for a, b in zip(areas, areas[1:]) if b > a * 1.01]
                    self.assertEqual(
                        bad, [],
                        f"{variant}／{kind} 段内不是先大后小："
                        f"{[f'{s:.2f}' for s in areas]}",
                    )

    def test_every_part_has_a_kind(self):
        """每个位号都要能归类——漏一类就是漏贴一批件，而按分类导航的人不会发现。"""
        for variant in VARIANTS:
            doc = load(variant)
            missing = [p["ref"] for g in doc["groups"] for p in g["parts"] if not p.get("kind")]
            with self.subTest(variant=variant):
                self.assertEqual(missing, [], f"{variant} 这些位号没有大类：{missing}")

    def test_kind_navigation_covers_everything(self):
        """按大类导航时，各类件数之和必须等于总数，不重不漏。"""
        for variant in VARIANTS:
            doc = load(variant)
            per_kind = collections.Counter(
                p["kind"] for g in doc["groups"] for p in g["parts"])
            with self.subTest(variant=variant):
                self.assertEqual(
                    sum(per_kind.values()), doc["total"],
                    f"{variant} 分类件数 {sum(per_kind.values())} ≠ 总数 {doc['total']}",
                )

    def test_refs_ascend_within_each_group(self):
        """组内位号必须升序：D2 → D3 → D4 → D5。

        用户 2026-09-09 提的：原来组内按板上位置排（想减少找焊盘的眼睛跳跃），
        贴出来是 D2 → D4 → D5 → D3，对着位号清单核对时完全对不上，
        也说不清自己贴到哪了。

        而且必须**按数字比**：字符串序会得到 R1 R10 R11 R2，看着像漏了一堆。
        """
        for variant in VARIANTS:
            doc = load(variant)
            for group in doc["groups"]:
                refs = [p["ref"] for p in group["parts"]]
                keys = [ref_key(r) for r in refs]
                with self.subTest(variant=variant, group=group["key"]):
                    self.assertEqual(
                        keys, sorted(keys),
                        f"{group['key']} 组内位号不是升序：{','.join(refs)}",
                    )

    def test_numeric_not_lexicographic(self):
        """至少有一组能证明用的是数字序而不是字符串序。

        没有这条，上一条在「恰好没有 R9/R10 这种组合」的数据上会假绿。
        """
        doc = load("v4.10-pwr")
        proof = []
        for group in doc["groups"]:
            refs = [p["ref"] for p in group["parts"]]
            # 字符串序和数字序会给出不同结果的组，才有证明力
            if sorted(refs) != [r for _, r in sorted((ref_key(r), r) for r in refs)]:
                proof.append((group["key"], refs))
        self.assertTrue(
            proof,
            "没有任何一组能区分数字序和字符串序，这条用例证明不了任何事",
        )
        for key, refs in proof:
            with self.subTest(group=key):
                keys = [ref_key(r) for r in refs]
                self.assertEqual(keys, sorted(keys), f"{key}: {','.join(refs)}")

    def test_board_outline_present(self):
        for variant in VARIANTS:
            with self.subTest(variant=variant):
                board = load(variant)["board"]
                self.assertGreater(board["w"], 10)
                self.assertGreater(board["h"], 10)
                self.assertGreaterEqual(len(board["edges"]), 3, "板框段数不足，画不出轮廓")


class PolarityContract(unittest.TestCase):
    """极性标注（pol/pin1）：语义正确 + 与 PCB 原文逐个对得上。

    背景：⚠ 有方向曾是前端前缀启发式——双向 ESD（D2~D6）被误标、
    真极性件却只说「有方向」不说往哪边。现在生成器按封装/型号输出
    语义（cath 阴极 / pin1 / none 双向），这里两道护栏：
    ① 语义自洽（谁该有、谁该没有、pin1 是不是 1 脚焊盘的形心）；
    ② 用**第二条独立路径**直接解析 .kicad_pcb 原文（不经 pcbnew、
    不经生成器）全量比对 pin1 坐标——生成器错一个旋转/移位都当场红。
    """

    def test_polarity_semantics(self):
        for variant in VARIANTS:
            for p in (part for g in load(variant)["groups"] for part in g["parts"]):
                with self.subTest(variant=variant, ref=p["ref"]):
                    if p["ref"].startswith("D"):
                        if "TPESD8L3.3" in p["val"]:
                            self.assertEqual(p["pol"], "none", p["ref"])
                        else:
                            self.assertEqual(p["pol"], "cath", p["ref"])
                    elif p["ref"].startswith(("U", "Q", "Y")):
                        self.assertEqual(p["pol"], "pin1", p["ref"])
                    elif p["ref"].startswith(("J", "BT")):
                        # J4（USB-C）没有名为 "1" 的焊盘且可正反插 → 允许不标
                        self.assertIn(p.get("pol"), ("pin1", None), p["ref"])
                    else:
                        self.assertNotIn("pol", p, p["ref"])
                    if p.get("pol") in ("cath", "pin1"):
                        pad1 = next(q for q in p["pads"] if q["n"] == "1")
                        xs = [q[0] for q in pad1["poly"]]
                        ys = [q[1] for q in pad1["poly"]]
                        self.assertAlmostEqual(p["pin1"]["x"], sum(xs) / len(xs), delta=0.02, msg=p["ref"])
                        self.assertAlmostEqual(p["pin1"]["y"], sum(ys) / len(ys), delta=0.02, msg=p["ref"])

    def test_pin1_matches_pcb_text(self):
        pcb = ROOT / "hardware" / "expansion-board-v4" / "kicad" / "expansion-board-v4.kicad_pcb"
        if not pcb.exists():
            self.skipTest("当前板的 kicad_pcb 不在仓库（存档版本无法对比 PCB）")
        toks = re.findall(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+', pcb.read_text(encoding="utf-8"))
        pos = 0

        def parse():
            nonlocal pos
            assert toks[pos] == "("
            pos += 1
            node = []
            while toks[pos] != ")":
                if toks[pos] == "(":
                    node.append(parse())
                else:
                    t = toks[pos]
                    pos += 1
                    node.append(t[1:-1] if t.startswith('"') else t)
            pos += 1
            return node

        root = parse()
        fps = {}

        def walk(n):
            if not (isinstance(n, list) and n):
                return
            if n[0] == "footprint":
                ref = fx = fy = ang = None
                pads = []
                for c in n[1:]:
                    if isinstance(c, list) and c:
                        if c[0] == "property" and len(c) > 2 and c[1] == "Reference":
                            ref = c[2]
                        elif c[0] == "at" and len(c) >= 3 and fx is None:
                            fx, fy = float(c[1]), float(c[2])
                            ang = float(c[3]) if len(c) > 3 else 0.0
                        elif c[0] == "pad":
                            pat = next((cc for cc in c[2:]
                                        if isinstance(cc, list) and cc and cc[0] == "at"), None)
                            if pat:
                                pads.append((c[1], float(pat[1]), float(pat[2])))
                if ref:
                    fps[ref] = ((fx, fy, ang), pads)
            for c in n[1:]:
                walk(c)

        walk(root)
        checked = 0
        # 当前板中不能存在 MARK；仅从待贴列表过滤会掩盖实际 PCB 的回归。
        self.assertEqual({ref for ref in fps if ref.startswith(("FID", "MARK"))}, set())
        # 原点独立现读板框线及笔宽，不沿用旧版本 JSON 的坐标。
        edges = [n for n in root if isinstance(n, list) and n and n[0] == "gr_line"
                 and ["layer", "Edge.Cuts"] in n]
        bounds = []
        for edge in edges:
            stroke = next(c for c in edge if isinstance(c, list) and c[0] == "stroke")
            width = float(next(c[1] for c in stroke if isinstance(c, list) and c[0] == "width"))
            for c in edge:
                if isinstance(c, list) and c[0] in ("start", "end"):
                    bounds.append((float(c[1]) - width / 2, float(c[2]) - width / 2))
        self.assertTrue(bounds, "当前板框没有 Edge.Cuts 直线")
        ox, oy = min(x for x, _ in bounds), min(y for _, y in bounds)
        self.assertTrue((DATA / "v4.11-pwr.json").exists(), "缺当前 V4.11 贴片数据")
        for g in load("v4.11-pwr")["groups"]:
            for p in g["parts"]:
                with self.subTest(ref=p["ref"]):
                    self.assertIn(p["ref"], fps)
                    (fx, fy, ang), pads = fps[p["ref"]]
                    self.assertAlmostEqual(p["x"], fx - ox, delta=0.002, msg=p["ref"])
                    self.assertAlmostEqual(p["y"], fy - oy, delta=0.002, msg=p["ref"])
                    self.assertAlmostEqual(p["rot"] % 360, ang % 360, delta=0.01, msg=p["ref"])
                if not p.get("pin1"):
                    continue
                with self.subTest(ref=p["ref"]):
                    self.assertIn(p["ref"], fps, f"PCB 原文里找不到 {p['ref']}")
                    (fx, fy, ang), pads = fps[p["ref"]]
                    pad1 = next(q for q in pads if q[0] == "1")
                    t = math.radians(ang)
                    # KiCad 文件坐标 y 向下、角度顺时针：[cos, sin; -sin, cos]
                    dx = pad1[1] * math.cos(t) + pad1[2] * math.sin(t)
                    dy = -pad1[1] * math.sin(t) + pad1[2] * math.cos(t)
                    self.assertAlmostEqual(p["pin1"]["x"], p["x"] + dx, delta=0.05, msg=p["ref"])
                    self.assertAlmostEqual(p["pin1"]["y"], p["y"] + dy, delta=0.05, msg=p["ref"])
                    checked += 1
        self.assertGreaterEqual(checked, 40, f"极性件只核到 {checked} 个，判据面缩水了")


if __name__ == "__main__":
    unittest.main(verbosity=2)
