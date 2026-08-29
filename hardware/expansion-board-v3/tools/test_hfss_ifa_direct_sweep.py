#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""hfss_ifa_direct_sweep 的纯参数与几何回归测试。"""

import importlib.util
import pathlib
import unittest


SCRIPT_PATH = pathlib.Path(__file__).with_name("hfss_ifa_direct_sweep.py")
SPEC = importlib.util.spec_from_file_location("hfss_ifa_direct_sweep", SCRIPT_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class DirectSweepParameterTest(unittest.TestCase):
    def test_existing_three_argument_call_keeps_new_defaults(self):
        params = MODULE.parse_script_argument("50.0,1.012,full6")

        self.assertEqual(params, (50.0, 1.012, "full6", 5.0, 6.0))

    def test_five_argument_call_parses_d_and_h(self):
        params = MODULE.parse_script_argument("50.5,1.634,top2,4.5,7.0")

        self.assertEqual(params, (50.5, 1.634, "top2", 4.5, 7.0))

    def test_case_token_contains_d_and_h(self):
        token = MODULE.build_case_token(50.0, 1.012, "full6", 4.5, 7.0)

        self.assertIn("_d4p500_", token)
        self.assertIn("_h7p000_", token)

    def test_internal_design_name_is_short_for_windows_result_paths(self):
        self.assertLessEqual(len(MODULE.DESIGN_NAME), 16)


class DirectSweepGeometryTest(unittest.TestCase):
    def test_d5_h6_exactly_matches_rev2_geometry(self):
        geometry = MODULE.calculate_geometry(50.0, 1.634, 5.0, 6.0)

        expected = {
            "arm_y": 59.616,
            "arm_open_x": 20.0,
            "feed_x": 65.0,
            "short_x": 70.0,
            "leg_center_end_y": 53.616,
            "leg_copper_end_y": 52.866,
            "feed_end_y": 48.5128,
            "taper_start_y": 52.866,
            "taper_end_y": 51.366,
            "microstrip_length": 2.8532,
            "slot_bottom": 48.2128,
        }
        for name, value in expected.items():
            self.assertAlmostEqual(geometry[name], value, places=9, msg=name)

    def test_d_moves_only_feed_path_x(self):
        baseline = MODULE.calculate_geometry(50.0, 1.634, 5.0, 6.0)
        changed = MODULE.calculate_geometry(50.0, 1.634, 4.25, 6.0)

        self.assertAlmostEqual(changed["feed_x"], 65.75, places=9)
        self.assertEqual(changed["short_x"], baseline["short_x"])
        self.assertEqual(changed["arm_open_x"], baseline["arm_open_x"])
        self.assertEqual(changed["arm_y"], baseline["arm_y"])

    def test_h_moves_leg_ground_taper_feed_and_port_y_together(self):
        baseline = MODULE.calculate_geometry(50.0, 1.634, 5.0, 6.0)
        changed = MODULE.calculate_geometry(50.0, 1.634, 5.0, 7.25)

        for name in (
            "leg_center_end_y",
            "leg_copper_end_y",
            "feed_end_y",
            "taper_start_y",
            "taper_end_y",
            "slot_bottom",
        ):
            self.assertAlmostEqual(changed[name], baseline[name] - 1.25, places=9)
        self.assertEqual(changed["arm_y"], baseline["arm_y"])
        self.assertEqual(changed["microstrip_length"], baseline["microstrip_length"])


if __name__ == "__main__":
    unittest.main()
