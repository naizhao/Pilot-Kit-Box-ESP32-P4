#!/usr/bin/env python3
"""贴片助手「采购批次」匹配判据的单元测试。

    /usr/bin/python3 -m unittest hardware.test_assembly_batch_match

判据本身在 `hardware/tools/gen_assembly_web.py` 的 `build_index()` / `match_batch()`。
那个脚本整体要 KiCad 自带的 Python（`import pcbnew`），但批次匹配是纯字符串逻辑，
跟 pcbnew 无关——这里把 pcbnew 塞个空壳进 sys.modules，就能在系统 python3 上测。

## 这份测试要挡住什么

匹配不上的组在网页上显示「未在采购表中（多半是库存已有）」。**这句话是个结论，
不是留白**：它告诉人"这盘料不用去翻采购袋"。所以假阴性比没有批次更坏——

2026-09-21 实际发生：`10nF C0G`（C87）明明是「采购清单(补3)」买的，网页上却
写着可能是库存已有。原因是采购表 B 列写 `10nF`、板上组名是 `10nF C0G`，
归一化只去了空格和 ±%，介质后缀没去掉，字符串对不上。同一个原因还埋了
`15pF C0G`、`30k`（采购表写 `30k ±1%`）、`33R`（表里是 `33R ±5%`）。

反过来，**放宽到"忽略介质"是有代价的**：0402 的 100pF C0G 和 100pF X7R 外观
完全一样，指错料袋就是拿错料，而 C34/C35 那两个位置必须 C0G。所以放宽只在
完整规格匹配不上时兜底，且采购表里剥掉后缀后**撞车就必须放弃**——宁可显示
"未匹配"，也不能把人指到错的那一袋。
"""

from __future__ import annotations

import importlib.util
import sys
import types
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

sys.modules.setdefault("pcbnew", types.ModuleType("pcbnew"))
_spec = importlib.util.spec_from_file_location(
    "gen_assembly_web", ROOT / "hardware" / "tools" / "gen_assembly_web.py")
gaw = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gaw)


def row(sheet_n, sheet, spec, fp, lcsc="", refs_text="", material=""):
    """一行采购表。列对应 B=规格 C=封装 D=材质 H=料号 I=规格要求（常写着位号）。"""
    return {"n": sheet_n, "name": sheet, "spec": spec, "fp": fp,
            "qty": "", "mpn": "", "bought": "", "lcsc": lcsc, "refs_text": refs_text,
            "material": material}


def group(spec, size, refs, lcsc=""):
    return {"key": f"{spec}|{size}", "spec": spec, "size": size, "lcsc": lcsc,
            "parts": [{"ref": r} for r in refs]}


class BatchMatchContract(unittest.TestCase):
    def test_dielectric_suffix_on_the_board_side(self):
        """板上 `10nF C0G`、采购表 `10nF` —— 真实漏判，C87 那一颗。"""
        idx = gaw.build_index([row(4, "采购清单(补3)", "10nF", "0603")])
        got = gaw.match_batch(group("10nF C0G", "0603", ["C87"]), idx)
        self.assertIsNotNone(got, "10nF C0G 应该匹配到采购表的 10nF 行")
        self.assertEqual(got["name"], "采购清单(补3)")

    def test_tolerance_suffix_on_the_purchase_side(self):
        """反方向：板上 `30k`、采购表 `30k ±1%`。归一化只删 ± 和 %，
        留下的 `30k1` 跟 `30k` 一样对不上。"""
        idx = gaw.build_index([row(4, "采购清单(补3)", "30k ±1%", "0603")])
        got = gaw.match_batch(group("30k", "0603", ["R50"]), idx)
        self.assertIsNotNone(got, "30k 应该匹配到采购表的 30k ±1% 行")
        self.assertEqual(got["name"], "采购清单(补3)")

    def test_exact_dielectric_wins_over_the_relaxed_one(self):
        """采购表分了介质时，必须各回各家——放宽只是兜底，不能抢答。"""
        idx = gaw.build_index([
            row(1, "采购清单", "100pF C0G", "0402"),
            row(2, "采购清单(补)", "100pF X7R", "0402"),
        ])
        self.assertEqual(gaw.match_batch(group("100pF C0G", "0402", ["C34"]), idx)["name"],
                         "采购清单")
        self.assertEqual(gaw.match_batch(group("100pF X7R", "0402", ["C40"]), idx)["name"],
                         "采购清单(补)")

    def test_ambiguous_base_spec_is_refused(self):
        """采购表里 C0G 和 X7R 分两批买，板上组名却没写介质——
        剥掉后缀会同时撞上两行。这时候**不能猜**：指错料袋就是拿错料。"""
        idx = gaw.build_index([
            row(1, "采购清单", "100pF C0G", "0402"),
            row(2, "采购清单(补)", "100pF X7R", "0402"),
        ])
        warn = []
        self.assertIsNone(gaw.match_batch(group("100pF", "0402", ["C40"]), idx, warn))
        self.assertTrue(warn, "放弃匹配时要留一句警告，否则没人知道这里被跳过了")

    def test_size_still_separates(self):
        """放宽的是介质，**不是尺寸**。0402 和 0603 的 10nF 外观一样，
        混成一盘料是真错误。"""
        idx = gaw.build_index([row(4, "采购清单(补3)", "10nF", "0603")])
        self.assertIsNone(gaw.match_batch(group("10nF C0G", "0402", ["C87"]), idx))

    def test_stripping_never_empties_the_spec(self):
        """剥后缀不能把规格剥没了：`C0G` 单独成组时剥完是空串，
        空串会跟所有空规格互相匹配上。"""
        self.assertEqual(gaw._spec_base("C0G"), "C0G")
        self.assertEqual(gaw._spec_base("±1%"), "±1%")

    def test_plain_values_are_left_alone(self):
        """别把有效信息当后缀剥掉：100pF 的 pF、4.7uF 的 uF 都得留着。"""
        for s in ("100pF", "4.7uF", "10nF", "30k", "33R", "0R", "1N4148WS"):
            self.assertEqual(gaw._spec_base(s), s)

    def test_ref_and_lcsc_still_win(self):
        """放宽的是第三级判据，前两级的优先级不变。"""
        idx = gaw.build_index([
            row(1, "采购清单", "10nF", "0603"),
            row(3, "采购清单(补2)", "随便什么", "0603", refs_text="C87 射频去耦"),
        ])
        got = gaw.match_batch(group("10nF C0G", "0603", ["C87"]), idx)
        self.assertEqual(got["by"], "位号")
        self.assertEqual(got["name"], "采购清单(补2)")

    def test_fp_typo_still_lands_in_the_right_base(self):
        """采购表把 0402 写成 "402"（补2 实表如此），归一化要兜住——
        否则那行落错键，介质候选集里根本没有它。"""
        self.assertEqual(gaw._norm_fp("402"), "0402")
        self.assertEqual(gaw._norm_fp("603"), "0603")
        self.assertEqual(gaw._norm_fp("33R"), "33R")   # 真 3 位值不许误归一

    def test_100pf_two_batches_resolved_by_group_dielectric(self):
        """2026-09-22 实际数据：100pF 0402 两袋——批1 X7R（隔直位）、
        补2 C0G/NP0（射频位 C31~C35，封装还写成 "402"、没写位号）。
        板上拆成两组后，组名带介质的必须各回各家。"""
        idx = gaw.build_index([
            row(1, "采购清单", "100pF", "0402", material="X7R"),
            row(3, "采购清单(补2)", "100pF", "402", material="C0G / NP0"),
        ])
        got = gaw.match_batch(group("100pF C0G", "0402", ["C31", "C34", "C35"]), idx)
        self.assertIsNotNone(got, "C0G 组要能挑到补2 那袋")
        self.assertEqual(got["n"], 3, "C0G 组应命中补2（第3批），不是批1 的 X7R 袋")
        # 组名没点介质（普通隔直位）：严格 规格+封装 命中最早的批1 X7R 袋——
        # 与实际用料一致；若哪天普通位改用 C0G 袋，得在采购表里写明介质
        got2 = gaw.match_batch(group("100pF", "0402", ["C21", "C30"]), idx)
        self.assertEqual(got2["n"], 1, "无介质后缀的组按严格规格命中最早一批")


if __name__ == "__main__":
    unittest.main()
