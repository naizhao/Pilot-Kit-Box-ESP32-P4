import unittest

from silk_geometry import first_clear_box


class SilkGeometryTest(unittest.TestCase):
    def test_chooses_first_in_bounds_non_overlapping_candidate(self):
        candidates = [
            ("outside", (-1, 0, 1, 1)),
            ("blocked", (1, 1, 2, 2)),
            ("clear", (3, 3, 4, 4)),
        ]

        self.assertEqual(
            first_clear_box(candidates, [(0.5, 0.5, 2.5, 2.5)], (0, 0, 5, 5), 0.1),
            "clear",
        )


if __name__ == "__main__":
    unittest.main()
