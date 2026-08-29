#!/usr/bin/env python3
"""从 KiCad DRC 精确移除悬空铜对象。"""

import unittest

from route_drc_fixes import (
    ensure_u16_gnd_tie,
    ensure_frozen_ifa_feed,
    prune_dangling,
    prune_free_gnd_vias_in_power_islands,
)


class PruneDanglingTest(unittest.TestCase):
    def test_removes_only_the_reported_via(self):
        data = {
            "tracks": [],
            "vias": [
                ["PWR", 10.0, 20.0, 0.45, 0.3],
                ["PWR", 11.0, 20.0, 0.45, 0.3],
            ],
        }
        report = {"violations": [{
            "type": "via_dangling",
            "items": [{
                # KiCad 覆铜回填可能把悬空 free via 临时归到所在 zone 的网络；
                # 精确目标必须按唯一坐标匹配，不能依赖报告里的网络名。
                "description": "过孔 [ADOPTED_ZONE_NET] (F.Cu - B.Cu)",
                "pos": {"x": 10.0, "y": 20.0},
            }],
        }]}

        cleaned, counts = prune_dangling(data, report)

        self.assertEqual(cleaned["vias"], [["PWR", 11.0, 20.0, 0.45, 0.3]])
        self.assertEqual(counts, {"vias": 1, "tracks": 0})

    def test_removes_only_track_matching_net_layer_position_and_length(self):
        data = {
            "tracks": [
                ["N", "In2.Cu", 1.0, 2.0, 1.05, 2.05, 0.25],
                ["N", "In2.Cu", 1.0, 2.0, 3.0, 2.0, 0.25],
            ],
            "vias": [],
        }
        report = {"violations": [{
            "type": "track_dangling",
            "items": [{
                "description": "走线 [N] (In2.Cu), 长度: 0.0707 mm",
                "pos": {"x": 1.0, "y": 2.0},
            }],
        }]}

        cleaned, counts = prune_dangling(data, report)

        self.assertEqual(cleaned["tracks"], [["N", "In2.Cu", 1.0, 2.0, 3.0, 2.0, 0.25]])
        self.assertEqual(counts, {"vias": 0, "tracks": 1})

    def test_u16_gnd_tie_is_idempotent(self):
        data = {"tracks": [], "vias": []}

        first = ensure_u16_gnd_tie(data)
        second = ensure_u16_gnd_tie(first)

        self.assertEqual(first, second)
        self.assertEqual(len(first["tracks"]), 1)
        self.assertEqual(len(first["vias"]), 1)

    def test_prunes_only_unanchored_gnd_via_inside_power_island(self):
        data = {
            "tracks": [["GND", "F.Cu", 70.0, 70.0, 71.0, 70.0, 0.2]],
            "vias": [
                ["GND", 70.0, 70.0, 0.45, 0.3],
                ["GND", 80.0, 70.0, 0.45, 0.3],
                ["GND", 50.0, 50.0, 0.45, 0.3],
                ["3V3_RF", 80.0, 70.0, 0.45, 0.3],
            ],
        }

        cleaned, removed = prune_free_gnd_vias_in_power_islands(data)

        self.assertEqual(removed, 1)
        self.assertNotIn(["GND", 80.0, 70.0, 0.45, 0.3], cleaned["vias"])
        self.assertIn(["GND", 70.0, 70.0, 0.45, 0.3], cleaned["vias"])

    def test_frozen_ifa_feed_replaces_chamfered_variant_idempotently(self):
        data = {
            "tracks": [["ANT1090_IFA", "F.Cu", 85.472, 60.012,
                        85.672, 62.865, 0.15]],
            "vias": [],
        }

        first = ensure_frozen_ifa_feed(data)
        second = ensure_frozen_ifa_feed(first)

        self.assertEqual(first, second)
        self.assertEqual(len(first["tracks"]), 2)


if __name__ == "__main__":
    unittest.main()
