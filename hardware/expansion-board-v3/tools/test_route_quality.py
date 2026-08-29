import unittest

from route_quality import analyze_records, normalize_record


def seg(a, b, net="N", layer="F.Cu", width=0.15):
    return normalize_record(net, layer, a, b, width)


class RouteQualityTest(unittest.TestCase):
    def test_clean_45_degree_elbow_passes(self):
        report = analyze_records([
            seg((0.0, 0.0), (1.0, 1.0)),
            seg((1.0, 1.0), (2.0, 1.0)),
        ])
        self.assertEqual(report.problem_count, 0)

    def test_arbitrary_angle_is_rejected(self):
        report = analyze_records([seg((0.0, 0.0), (1.0, 0.5))])
        self.assertEqual(len(report.non45), 1)

    def test_duplicate_is_direction_independent(self):
        report = analyze_records([
            seg((0.0, 0.0), (1.0, 0.0)),
            seg((1.0, 0.0), (0.0, 0.0)),
        ])
        self.assertEqual(len(report.duplicates), 1)

    def test_endpoint_on_segment_interior_is_rejected(self):
        report = analyze_records([
            seg((0.0, 0.0), (2.0, 0.0)),
            seg((1.0, 0.0), (1.0, 1.0)),
        ])
        self.assertEqual(len(report.interior_joins), 1)

    def test_copper_touch_without_exact_vertex_is_rejected(self):
        report = analyze_records([
            seg((0.0, 0.0), (1.0, 0.0)),
            seg((1.05, 0.0), (2.0, 0.0)),
        ])
        self.assertEqual(len(report.copper_touches), 1)

    def test_nearby_legs_of_one_exactly_connected_chamfer_are_not_fake_connections(self):
        report = analyze_records([
            seg((0.0, 0.0), (0.9, 0.0), width=0.2),
            seg((0.9, 0.0), (1.0, 0.1), width=0.2),
            seg((1.0, 0.1), (1.0, 1.0), width=0.2),
        ])
        self.assertEqual(len(report.copper_touches), 0)

    def test_direct_right_angle_corner_is_rejected(self):
        report = analyze_records([
            seg((0.0, 0.0), (1.0, 0.0)),
            seg((1.0, 0.0), (1.0, 1.0)),
        ])
        self.assertEqual(len(report.right_angle_corners), 1)

    def test_collinear_splice_is_rejected(self):
        report = analyze_records([
            seg((0.0, 0.0), (1.0, 0.0)),
            seg((1.0, 0.0), (2.0, 0.0)),
        ])
        self.assertEqual(len(report.collinear_splices), 1)

    def test_collinear_width_transition_is_not_a_redundant_splice(self):
        report = analyze_records([
            seg((0.0, 0.0), (1.0, 0.0), width=0.15),
            seg((1.0, 0.0), (2.0, 0.0), width=0.25),
        ])
        self.assertEqual(len(report.collinear_splices), 0)

    def test_corner_at_via_anchor_can_be_exempted(self):
        report = analyze_records([
            seg((0.0, 0.0), (1.0, 0.0)),
            seg((1.0, 0.0), (1.0, 1.0)),
        ], corner_exemptions={('N', 'F.Cu', (1.0, 0.0))})
        self.assertEqual(len(report.right_angle_corners), 0)


if __name__ == "__main__":
    unittest.main()
