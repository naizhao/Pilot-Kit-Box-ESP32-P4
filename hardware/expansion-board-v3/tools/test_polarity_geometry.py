import unittest

from polarity_geometry import has_clear_cathode_bar, is_diode_target, marker_segment


class PolarityGeometryTest(unittest.TestCase):
    def test_all_two_pad_diode_references_are_targets(self):
        self.assertTrue(is_diode_target("D1", {"1", "2"}))
        self.assertTrue(is_diode_target("D27", {"1", "2"}))
        self.assertFalse(is_diode_target("R1", {"1", "2"}))
        self.assertFalse(is_diode_target("D4", {"1"}))

    def test_horizontal_marker_is_outside_cathode_pad(self):
        start, end = marker_segment((10.0, 20.0), (9.5, 20.0), (10.5, 20.0), 1.09, 0.7)
        self.assertEqual({start, end}, {(8.91, 19.65), (8.91, 20.35)})

    def test_vertical_marker_is_outside_cathode_pad(self):
        start, end = marker_segment((10.0, 20.0), (10.0, 19.5), (10.0, 20.5), 1.09, 0.7)
        self.assertEqual({start, end}, {(10.35, 18.91), (9.65, 18.91)})

    def test_existing_perpendicular_bar_beyond_cathode_is_accepted(self):
        segments = [((7.65, 19.0), (7.65, 21.0), 0.12)]
        self.assertTrue(has_clear_cathode_bar(
            (10.0, 20.0), (8.35, 20.0), (11.65, 20.0), segments
        ))

    def test_line_on_anode_side_is_not_a_cathode_bar(self):
        segments = [((12.35, 19.0), (12.35, 21.0), 0.12)]
        self.assertFalse(has_clear_cathode_bar(
            (10.0, 20.0), (8.35, 20.0), (11.65, 20.0), segments
        ))


if __name__ == "__main__":
    unittest.main()
