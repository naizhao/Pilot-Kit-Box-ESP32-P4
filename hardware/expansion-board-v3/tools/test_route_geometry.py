#!/usr/bin/env python3
"""路由栅格端点吸附到真实铜几何的回归测试。"""

import unittest

from route_geometry import closest_point_on_block


class ClosestPointOnBlockTest(unittest.TestCase):
    def test_track_anchor_uses_projected_point_not_grid_cell_center(self):
        block = [("trk", (60.0, 70.0, 66.0, 70.0), [1])]

        point = closest_point_on_block(block, (63.08, 70.06), layer=1)

        self.assertEqual(point, (63.08, 70.0))

    def test_pad_anchor_is_clamped_inside_pad_rectangle(self):
        block = [("pad", (10.0, 20.0, 11.0, 21.0), [0])]

        point = closest_point_on_block(block, (11.12, 20.4), layer=0)

        self.assertEqual(point, (11.0, 20.4))

    def test_via_anchor_is_available_on_every_listed_layer(self):
        block = [("via", (5.0, 6.0), [0, 1, 2, 3])]

        point = closest_point_on_block(block, (5.1, 6.1), layer=2)

        self.assertEqual(point, (5.0, 6.0))

    def test_elements_on_other_layers_are_ignored(self):
        block = [
            ("trk", (1.0, 1.0, 2.0, 1.0), [0]),
            ("trk", (8.0, 8.0, 10.0, 8.0), [1]),
        ]

        point = closest_point_on_block(block, (1.5, 1.1), layer=1)

        self.assertEqual(point, (8.0, 8.0))


if __name__ == "__main__":
    unittest.main()
