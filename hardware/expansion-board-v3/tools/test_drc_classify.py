#!/usr/bin/env python3
"""KiCad DRC 项目分类的中英文回归测试。"""

import unittest

from drc_classify import (
    classify_unconnected_item,
    description_kind,
    drc_failure_summary,
    extract_layer_name,
    extract_net_name,
)


class DescriptionKindTest(unittest.TestCase):
    def test_english_kinds(self):
        self.assertEqual(description_kind("Pad 1 [NET] of U1 on F.Cu"), "pad")
        self.assertEqual(description_kind("Track [NET] on F.Cu"), "track")
        self.assertEqual(description_kind("Via [NET] on F.Cu - B.Cu"), "via")
        self.assertEqual(description_kind("Zone [GND] on F.Cu"), "zone")

    def test_simplified_chinese_kinds(self):
        self.assertEqual(description_kind("F.Cu 上 U3 的焊盘 5 [3V3_GNSS]"), "pad")
        self.assertEqual(description_kind("走线 [3V3_GNSS] (F.Cu), 长度: 3.8 mm"), "track")
        self.assertEqual(description_kind("过孔 [3V3_GNSS] (F.Cu - B.Cu)"), "via")
        self.assertEqual(description_kind("填充区[GND]在 F.Cu 上"), "zone")

    def test_layer_name_is_extracted_from_english_and_chinese_descriptions(self):
        self.assertEqual(extract_layer_name("Pad 1 [NET] of U1 on F.Cu"), "F.Cu")
        self.assertEqual(extract_layer_name("F.Cu 上 U3 的焊盘 5 [3V3_GNSS]"), "F.Cu")
        self.assertEqual(extract_layer_name("走线 [NET] (In2.Cu), 长度: 1 mm"), "In2.Cu")
        self.assertEqual(extract_layer_name("过孔 [NET] (F.Cu - B.Cu)"), "F.Cu")


class UnconnectedClassificationTest(unittest.TestCase):
    def test_pad_to_track_is_a_real_missing_connection_in_chinese_report(self):
        item = {
            "items": [
                {"description": "F.Cu 上 U3 的焊盘 5 [3V3_GNSS]"},
                {"description": "走线 [3V3_GNSS] (F.Cu), 长度: 3.8 mm"},
            ]
        }

        self.assertEqual(classify_unconnected_item(item), "need")
        self.assertEqual(extract_net_name(item), "3V3_GNSS")

    def test_track_to_track_is_kept_as_orphan_for_connectivity_analysis(self):
        item = {
            "items": [
                {"description": "走线 [SW1_J2] (In2.Cu), 长度: 0.65 mm"},
                {"description": "走线 [SW1_J2] (F.Cu), 长度: 2.75 mm"},
            ]
        }

        self.assertEqual(classify_unconnected_item(item), "orphan")

    def test_zone_connection_is_classified_separately(self):
        item = {
            "items": [
                {"description": "F.Cu 上 SW2 的焊盘 2 [GND]"},
                {"description": "填充区[GND]在 F.Cu 上"},
            ]
        }

        self.assertEqual(classify_unconnected_item(item), "plane")


class DrcFailureGateTest(unittest.TestCase):
    def test_clean_report_passes(self):
        self.assertEqual(
            drc_failure_summary({"violations": [], "unconnected_items": []}),
            "",
        )

    def test_any_violation_fails_even_when_it_is_silkscreen(self):
        report = {
            "violations": [{"type": "silk_overlap"}],
            "unconnected_items": [],
        }

        self.assertEqual(drc_failure_summary(report), "DRC 违例 1 项")

    def test_any_unconnected_item_fails(self):
        report = {
            "violations": [],
            "unconnected_items": [{"type": "unconnected_items"}],
        }

        self.assertEqual(drc_failure_summary(report), "未连通 1 项")

    def test_violation_and_unconnected_counts_are_both_reported(self):
        report = {
            "violations": [{"type": "clearance"}, {"type": "silk_overlap"}],
            "unconnected_items": [{"type": "unconnected_items"}],
        }

        self.assertEqual(
            drc_failure_summary(report),
            "DRC 违例 2 项；未连通 1 项",
        )


if __name__ == "__main__":
    unittest.main()
