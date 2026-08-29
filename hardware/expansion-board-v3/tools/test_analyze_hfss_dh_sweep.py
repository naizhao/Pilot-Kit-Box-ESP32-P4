#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""D/H HFSS 扫参结果分析器的回归测试。"""

import csv
import importlib.util
import io
import pathlib
import tempfile
import unittest


SCRIPT_PATH = pathlib.Path(__file__).with_name("analyze_hfss_dh_sweep.py")
SPEC = importlib.util.spec_from_file_location("analyze_hfss_dh_sweep", SCRIPT_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def gamma_from_z(resistance, reactance):
    """把测试用阻抗换算成 50Ω 参考面的复数 S11。"""
    impedance = complex(resistance, reactance)
    return (impedance - 50.0) / (impedance + 50.0)


def fixture_csv():
    """同时包含 0.91GHz 高阻交叉和 1.05GHz 目标低阻交叉。"""
    rows = (
        (0.900, 500.0, -10.0),
        (0.920, 500.0, +10.0),
        (1.000, 30.0, -20.0),
        (1.100, 20.0, +20.0),
        (1.200, 40.0, +50.0),
    )
    stream = io.StringIO()
    writer = csv.writer(stream, lineterminator="\n")
    writer.writerow(
        (
            "Freq [GHz]",
            "dB(S(Port1,Port1)) []",
            "re(S(Port1,Port1)) []",
            "im(S(Port1,Port1)) []",
        )
    )
    for frequency_ghz, resistance, reactance in rows:
        gamma = gamma_from_z(resistance, reactance)
        writer.writerow((frequency_ghz, "", gamma.real, gamma.imag))
    stream.seek(0)
    return stream


class FilenameParameterTest(unittest.TestCase):
    def test_parse_l_d_h_from_hfss_case_filename(self):
        params = MODULE.parse_case_parameters(
            "ifa_direct_50p20_feed5p10_edge1p012_"
            "d4p988_h6p000_full6_copperedge_rev2_s11.csv"
        )

        self.assertEqual(params, {"length_mm": 50.2, "d_mm": 4.988, "h_mm": 6.0})

    def test_missing_d_and_h_are_reported_as_none_for_legacy_csv(self):
        params = MODULE.parse_case_parameters(
            "ifa_direct_50p00_feed5p10_edge1p012_full6_copperedge_rev2_s11.csv"
        )

        self.assertEqual(params, {"length_mm": 50.0, "d_mm": None, "h_mm": None})


class SParameterMathTest(unittest.TestCase):
    def test_s11_to_impedance_and_swr(self):
        gamma = gamma_from_z(25.0, 10.0)

        impedance = MODULE.s11_to_impedance(gamma)

        self.assertAlmostEqual(impedance.real, 25.0, places=12)
        self.assertAlmostEqual(impedance.imag, 10.0, places=12)
        self.assertAlmostEqual(MODULE.s11_to_swr(0j), 1.0, places=12)

    def test_analysis_interpolates_1090_and_rejects_high_resistance_crossing(self):
        samples = MODULE.read_s11_csv(fixture_csv())

        result = MODULE.analyze_samples(samples, target_frequency_mhz=1090.0)

        expected_gamma = samples[2].s11 + 0.9 * (samples[3].s11 - samples[2].s11)
        expected_z = MODULE.s11_to_impedance(expected_gamma)
        self.assertAlmostEqual(result["at_target"]["frequency_mhz"], 1090.0, places=12)
        self.assertAlmostEqual(result["at_target"]["z_real_ohm"], expected_z.real, places=12)
        self.assertAlmostEqual(result["at_target"]["z_imag_ohm"], expected_z.imag, places=12)
        self.assertAlmostEqual(result["at_target"]["s11_real"], expected_gamma.real, places=12)
        self.assertAlmostEqual(result["at_target"]["s11_imag"], expected_gamma.imag, places=12)
        self.assertAlmostEqual(
            result["at_target"]["swr"], MODULE.s11_to_swr(expected_gamma), places=12
        )
        self.assertAlmostEqual(result["series_resonance"]["frequency_mhz"], 1050.0)
        self.assertAlmostEqual(result["series_resonance"]["resistance_ohm"], 25.0)
        self.assertLess(result["series_resonance"]["resistance_ohm"], 100.0)
        self.assertGreater(result["s11_minimum"]["frequency_mhz"], 980.0)


class OutputAndRegressionTest(unittest.TestCase):
    def test_analyze_file_includes_parameters_and_can_write_both_formats(self):
        filename = (
            "ifa_direct_50p20_feed5p10_edge1p012_"
            "d4p500_h7p000_full6_copperedge_rev2_s11.csv"
        )
        with tempfile.TemporaryDirectory() as directory:
            input_path = pathlib.Path(directory, filename)
            input_path.write_text(fixture_csv().read(), encoding="utf-8")
            result = MODULE.analyze_file(input_path)
            json_path = pathlib.Path(directory, "summary.json")
            csv_path = pathlib.Path(directory, "summary.csv")

            MODULE.write_json_summary([result], json_path)
            MODULE.write_csv_summary([result], csv_path)

            self.assertEqual(result["parameters"]["d_mm"], 4.5)
            self.assertIn('"series_resonance"', json_path.read_text(encoding="utf-8"))
            self.assertIn("series_resonance_frequency_mhz", csv_path.read_text(encoding="utf-8"))

    def test_existing_rev2_center_csv_matches_recorded_main_mode(self):
        csv_path = SCRIPT_PATH.parent.parent / "build" / "hfss" / (
            "ifa_direct_50p00_feed5p10_edge1p012_"
            "full6_copperedge_rev2_s11.csv"
        )

        result = MODULE.analyze_file(csv_path)

        self.assertAlmostEqual(
            result["series_resonance"]["frequency_mhz"], 1059.052, places=3
        )
        self.assertAlmostEqual(
            result["series_resonance"]["resistance_ohm"], 20.951, places=3
        )

    def test_nine_point_summary_matches_all_raw_hfss_csvs(self):
        hfss_dir = SCRIPT_PATH.parent.parent / "build" / "hfss"
        expected_grid = {
            (distance, height)
            for distance in (4.0, 4.988, 7.5)
            for height in (5.0, 6.0, 7.0)
        }
        computed = {}
        for path in hfss_dir.glob(
                "ifa_direct_50p00_feed5p10_edge1p012_d*_h*_full6_copperedge_rev2_s11.csv"):
            parameters = MODULE.parse_case_parameters(path.name)
            key = (parameters["d_mm"], parameters["h_mm"])
            if key not in expected_grid:
                continue
            result = MODULE.analyze_file(path)
            computed[key] = (
                result["at_target"]["z_real_ohm"],
                result["at_target"]["z_imag_ohm"],
                result["series_resonance"]["frequency_mhz"],
                result["series_resonance"]["resistance_ohm"],
            )

        with (hfss_dir / "ifa_dh_sweep_summary.csv").open(
                encoding="utf-8", newline="") as stream:
            reported = {
                (float(row["d_mm"]), float(row["h_mm"])): (
                    float(row["target_z_real_ohm"]),
                    float(row["target_z_imag_ohm"]),
                    float(row["series_resonance_frequency_mhz"]),
                    float(row["series_resonance_resistance_ohm"]),
                )
                for row in csv.DictReader(stream)
            }

        self.assertEqual(computed, reported)


if __name__ == "__main__":
    unittest.main()
