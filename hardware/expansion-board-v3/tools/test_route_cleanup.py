#!/usr/bin/env python3
"""布线几何规范化的行为测试。"""

import unittest

from route_cleanup import cleanup_records
from route_quality import analyze_records, normalize_record


def seg(a, b, net="N", layer="F.Cu", width=0.15):
    return normalize_record(net, layer, a, b, width)


class RouteCleanupTest(unittest.TestCase):
    def assert_clean(self, records):
        report = analyze_records(records)
        self.assertEqual(report.problem_count, 0, report)

    def test_removes_duplicates_and_merges_collinear_fragments(self):
        cleaned = cleanup_records([
            seg((0, 0), (1, 0)),
            seg((1, 0), (0, 0)),
            seg((1, 0), (2, 0)),
        ])

        self.assertEqual(cleaned, [seg((0, 0), (2, 0))])
        self.assert_clean(cleaned)

    def test_splits_trunk_at_branch_vertex(self):
        cleaned = cleanup_records([
            seg((0, 0), (2, 0)),
            seg((1, 0), (1, 1)),
        ])

        self.assertEqual(len(cleaned), 3)
        self.assert_clean(cleaned)

    def test_converts_arbitrary_angle_without_moving_endpoints(self):
        cleaned = cleanup_records([seg((0, 0), (2, 1))])

        vertices = {point for item in cleaned for point in (item.start, item.end)}
        self.assertIn((0.0, 0.0), vertices)
        self.assertIn((2.0, 1.0), vertices)
        self.assert_clean(cleaned)

    def test_does_not_merge_short_45_degree_elbow_as_collinear(self):
        cleaned = cleanup_records([
            seg((0, 0), (0.01, 0)),
            seg((0.01, 0), (0.02, 0.01)),
        ])

        self.assertEqual(len(cleaned), 2)
        self.assert_clean(cleaned)

    def test_chamfers_direct_right_angle(self):
        cleaned = cleanup_records([
            seg((0, 0), (1, 0)),
            seg((1, 0), (1, 1)),
        ])

        self.assertEqual(len(cleaned), 3)
        self.assert_clean(cleaned)

    def test_collapses_sub_micron_corner_fragment_before_chamfering(self):
        cleaned = cleanup_records([
            seg((0, 0), (1, 0)),
            seg((1, 0), (1, 0.0001)),
            seg((1, 0.0001), (2, 0.0001)),
        ])

        self.assertEqual(cleaned, [seg((0, 0), (2, 0))])
        self.assert_clean(cleaned)

    def test_snaps_copper_width_touch_to_exact_vertex(self):
        cleaned = cleanup_records([
            seg((0, 0), (1, 0), width=0.2),
            seg((1.05, 0), (2, 0), width=0.2),
        ])

        self.assertEqual(cleaned, [seg((0, 0), (2, 0), width=0.2)])
        self.assert_clean(cleaned)

    def test_snaps_branch_to_trunk_and_splits_exactly(self):
        cleaned = cleanup_records([
            seg((0, 0), (2, 0), width=0.2),
            seg((1, 0.05), (1, 1), width=0.2),
        ])

        self.assertEqual(len(cleaned), 3)
        self.assert_clean(cleaned)


if __name__ == "__main__":
    unittest.main()
