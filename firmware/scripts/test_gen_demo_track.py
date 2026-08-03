#!/usr/bin/env python3
"""gen_demo_track.py 的解析 / 派生 / 抽稀单测。

    python3 -m unittest discover -s firmware/scripts -p 'test_*.py'

盯的是四类曾经会静默出错的地方：
  1. 命名空间与时间格式（写死 GPX 1.1 或写死 `Z` 结尾都会返回 0 个点，
     而 0 个点在下游只表现为"演示模式不动"，很难追）；
  2. 角度回绕（359°→1° 必须走 2°，走 358° 会把直线判成急转）；
  3. 抽稀的**误差上限**——这是本脚本唯一的正确性承诺；
  4. 生成的 C 文本能被后续编译，字段顺序与 demo_track.h 的结构体一致。
"""

from __future__ import annotations

import math
import unittest

import gen_demo_track as G


GPX_11 = """<?xml version="1.0"?>
<gpx xmlns="http://www.topografix.com/GPX/1/1" version="1.1">
 <trk><trkseg>
  <trkpt lat="23.0" lon="113.0"><ele>100.0</ele><time>2020-12-16T05:24:34+0000</time></trkpt>
  <trkpt lat="23.01" lon="113.0"><ele>200.0</ele><time>2020-12-16T05:24:44+0000</time></trkpt>
  <trkpt lat="23.02" lon="113.0"><ele>300.0</ele><time>2020-12-16T05:24:54Z</time></trkpt>
 </trkseg></trk></gpx>"""

GPX_10 = """<?xml version="1.0"?>
<gpx xmlns="http://www.topografix.com/GPX/1/0" version="1.0">
 <trk><trkseg>
  <trkpt lat="40.0" lon="116.0"><ele>10</ele><time>2021-01-01T00:00:00Z</time></trkpt>
  <trkpt lat="40.001" lon="116.0"><ele>10</ele><time>2021-01-01T00:00:01Z</time></trkpt>
 </trkseg></trk></gpx>"""


class ParseTest(unittest.TestCase):
    def test_gpx11_offset_and_z_timezones(self) -> None:
        pts = G.parse_gpx(GPX_11, is_text=True)
        self.assertEqual(len(pts), 3)
        self.assertEqual([p.t_s for p in pts], [0.0, 10.0, 20.0])
        self.assertAlmostEqual(pts[0].lat, 23.0)
        self.assertAlmostEqual(pts[2].alt_m, 300.0)

    def test_gpx10_namespace_is_not_hardcoded(self) -> None:
        """GPX 1.0 的 xmlns 与 1.1 不同；写死 1.1 会静默返回 0 个点。"""
        self.assertEqual(len(G.parse_gpx(GPX_10, is_text=True)), 2)

    def test_non_monotonic_time_is_dropped(self) -> None:
        text = GPX_11.replace(
            '<trkpt lat="23.02" lon="113.0"><ele>300.0</ele><time>2020-12-16T05:24:54Z</time>',
            '<trkpt lat="23.02" lon="113.0"><ele>300.0</ele><time>2020-12-16T05:24:40Z</time>')
        pts = G.parse_gpx(text, is_text=True)
        self.assertEqual([p.t_s for p in pts], [0.0, 10.0])

    def test_point_without_time_is_skipped(self) -> None:
        text = GPX_11.replace(
            "<time>2020-12-16T05:24:44+0000</time>", "")
        self.assertEqual(len(G.parse_gpx(text, is_text=True)), 2)


class GeometryTest(unittest.TestCase):
    def test_bearing_cardinals(self) -> None:
        self.assertAlmostEqual(G.bearing_deg(0, 0, 1, 0), 0.0, places=3)
        self.assertAlmostEqual(G.bearing_deg(0, 0, 0, 1), 90.0, places=3)
        self.assertAlmostEqual(G.bearing_deg(0, 1, 0, 0), 270.0, places=3)

    def test_dist_one_degree_lat_is_60nm(self) -> None:
        nm = G.dist_m(30.0, 120.0, 31.0, 120.0) / 1852.0
        self.assertAlmostEqual(nm, 60.0, delta=0.3)

    def test_lerp_angle_takes_short_arc(self) -> None:
        self.assertAlmostEqual(G.lerp_angle(359.0, 1.0, 0.5), 0.0, places=6)
        self.assertAlmostEqual(G.lerp_angle(10.0, 350.0, 0.5), 0.0, places=6)
        self.assertAlmostEqual(G.lerp_angle(350.0, 10.0, 0.25), 355.0, places=6)


def _synth_turn(n: int = 240, gs_kt: float = 300.0, rate_dps: float = 3.0):
    """合成一段标准率转弯：地速固定、航迹每秒转 rate_dps。"""
    v = gs_kt / G.KT_PER_MS
    lat, lon, trk = 30.0, 120.0, 0.0
    out = []
    for i in range(n):
        out.append(G.Sample(t_s=float(i), lat=lat, lon=lon, alt_m=3000.0))
        r = math.radians(trk)
        lat += (v * math.cos(r)) / G.EARTH_R_M * 180.0 / math.pi
        lon += (v * math.sin(r)) / (G.EARTH_R_M * math.cos(math.radians(lat))) \
            * 180.0 / math.pi
        trk = (trk + rate_dps) % 360.0
    return out


class DeriveTest(unittest.TestCase):
    def test_gs_and_track_recovered_from_positions(self) -> None:
        s = G.derive(_synth_turn(rate_dps=0.0))
        mid = s[len(s) // 2]
        self.assertAlmostEqual(mid.gs_kt, 300.0, delta=2.0)
        self.assertAlmostEqual(mid.trk_deg, 0.0, delta=0.5)

    def test_standard_rate_turn_gives_textbook_bank(self) -> None:
        """3°/s @ 300 kt 的协调转弯坡度 = atan(Vω/g) ≈ 38°，钳到 30°。
        钳位本身也是被测行为：民航自驾常用上限 25–30°，超出的一律是噪声。"""
        s = G.derive(_synth_turn(rate_dps=3.0))
        mid = s[len(s) // 2]
        self.assertAlmostEqual(mid.roll_deg, 30.0, delta=0.5)

    def test_gentle_turn_bank_matches_formula(self) -> None:
        s = G.derive(_synth_turn(gs_kt=200.0, rate_dps=1.0))
        v = 200.0 / G.KT_PER_MS
        want = math.degrees(math.atan2(v * math.radians(1.0), G.G_MS2))
        self.assertAlmostEqual(s[len(s) // 2].roll_deg, want, delta=1.0)

    def test_turn_direction_signs_roll(self) -> None:
        right = G.derive(_synth_turn(rate_dps=1.5))
        left = G.derive(_synth_turn(rate_dps=-1.5))
        self.assertGreater(right[len(right) // 2].roll_deg, 5.0)
        self.assertLess(left[len(left) // 2].roll_deg, -5.0)


class DespikeTest(unittest.TestCase):
    """真实 GPX 里高度有两类脏数据，处方不同，都得测。"""

    def test_isolated_spike_is_removed_by_median(self) -> None:
        s = [G.Sample(t_s=float(i), lat=30.0, lon=120.0, alt_m=3000.0)
             for i in range(20)]
        s[10].alt_m = 3400.0                      # 单点毛刺
        G.despike_alt(s)
        self.assertAlmostEqual(s[10].alt_m, 3000.0, delta=1.0)

    def test_step_is_spread_at_physical_rate(self) -> None:
        """台阶（跳完不回来）中值滤波管不了，靠限速跟随摊开。"""
        s = [G.Sample(t_s=float(i), lat=30.0, lon=120.0,
                      alt_m=1000.0 + (400.0 if i >= 10 else 0.0))
             for i in range(60)]
        G.despike_alt(s)
        rates = [abs(s[i].alt_m - s[i - 1].alt_m) / (s[i].t_s - s[i - 1].t_s)
                 for i in range(1, len(s))]
        self.assertLessEqual(max(rates), G.ALT_MAX_RATE_MPS + 1e-6)
        # 摊开之后必须**追平**原始基准，而不是永久留一个偏置——
        # 减常数那种做法会让落地高度整段错掉。
        self.assertAlmostEqual(s[-1].alt_m, 1400.0, delta=1.0)

    def test_normal_climb_is_untouched(self) -> None:
        s = [G.Sample(t_s=float(i), lat=30.0, lon=120.0, alt_m=1000.0 + 10.0 * i)
             for i in range(40)]
        G.despike_alt(s)
        self.assertAlmostEqual(s[-1].alt_m, 1000.0 + 10.0 * 39, delta=0.01)


class QuantizeTimeTest(unittest.TestCase):
    def test_subsecond_samples_never_collide(self) -> None:
        """1.8 s 这种非整秒采样四舍五入会撞成同一个整数，
        撞了之后回放的二分查找会取到一个分母为 0 的段。"""
        s = [G.Sample(t_s=t, lat=0, lon=0, alt_m=0)
             for t in (0.0, 0.2, 0.4, 1.8, 2.0, 2.1, 9.9)]
        t_col = G.quantize_time(s)
        self.assertEqual(len(t_col), len(s))
        self.assertEqual(t_col, sorted(t_col))
        self.assertEqual(len(set(t_col)), len(t_col))
        self.assertEqual(t_col[0], 0)

    def test_well_spaced_times_are_unchanged(self) -> None:
        s = [G.Sample(t_s=float(t), lat=0, lon=0, alt_m=0)
             for t in (0, 7, 31, 900)]
        self.assertEqual(G.quantize_time(s), [0, 7, 31, 900])


class SimplifyTest(unittest.TestCase):
    def test_straight_line_collapses_but_respects_max_seg(self) -> None:
        s = G.derive(_synth_turn(n=300, rate_dps=0.0))
        kept = G.simplify(s)
        # 全直线：只受 MAX_SEG_S 约束，300 s / MAX_SEG_S 段（+首尾）。
        expect = 300.0 / G.MAX_SEG_S
        self.assertLessEqual(len(kept), expect + 3)
        self.assertGreaterEqual(len(kept), expect - 1)

    def test_error_stays_within_tolerance(self) -> None:
        """抽稀的唯一承诺：任何原始点在抽稀后的折线上插值，误差不超容差。"""
        s = G.derive(_synth_turn(n=600, rate_dps=1.2))
        kept = G.simplify(s)
        self.assertLess(len(kept), len(s))
        worst_pos = worst_trk = 0.0
        j = 0
        for p in s:
            while j + 1 < len(kept) - 1 and kept[j + 1].t_s <= p.t_s:
                j += 1
            a, b = kept[j], kept[j + 1]
            span = b.t_s - a.t_s
            u = 0.0 if span <= 0 else (p.t_s - a.t_s) / span
            u = max(0.0, min(1.0, u))
            lat = a.lat + (b.lat - a.lat) * u
            lon = a.lon + (b.lon - a.lon) * u
            worst_pos = max(worst_pos, G.dist_m(lat, lon, p.lat, p.lon))
            worst_trk = max(worst_trk, abs(G.wrap180(
                G.lerp_angle(a.trk_deg, b.trk_deg, u) - p.trk_deg)))
        self.assertLessEqual(worst_pos, G.POS_TOL_M + 1.0)
        self.assertLessEqual(worst_trk, G.TRK_TOL_DEG + 0.2)

    def test_endpoints_are_always_kept(self) -> None:
        s = G.derive(_synth_turn(n=200, rate_dps=0.5))
        kept = G.simplify(s)
        self.assertEqual(kept[0].t_s, s[0].t_s)
        self.assertEqual(kept[-1].t_s, s[-1].t_s)

    def test_short_input_passthrough(self) -> None:
        s = [G.Sample(0, 1, 2, 3), G.Sample(1, 1, 2, 3)]
        self.assertEqual(len(G.simplify(s)), 2)


class EmitTest(unittest.TestCase):
    def test_record_field_order_and_ranges(self) -> None:
        s = G.derive(_synth_turn(n=60, rate_dps=2.0))
        text = G.emit_c(G.simplify(s), "unit.gpx", len(s))
        self.assertIn('#include "demo_track.h"', text)
        self.assertIn("const pk_demo_track_pt_t pk_demo_track[]", text)
        self.assertIn("pk_demo_track_dur_s", text)
        rows = [l.strip().rstrip(",") for l in text.splitlines()
                if l.strip().startswith("{")]
        self.assertGreaterEqual(len(rows), 2)
        for r in rows:
            f = [int(x) for x in r.strip("{}").split(",")]
            self.assertEqual(len(f), 7)
            self.assertTrue(-900000000 <= f[0] <= 900000000)     # lat_e7
            self.assertTrue(-1800000000 <= f[1] <= 1800000000)   # lon_e7
            self.assertGreaterEqual(f[2], 0)                     # t_s
            self.assertTrue(-32768 <= f[3] <= 32767)             # alt_m
            self.assertTrue(-300 <= f[4] <= 300)                 # roll 0.1°
            self.assertTrue(0 <= f[5] <= 3599)                   # trk 0.1°
            self.assertGreaterEqual(f[6], 0)                     # gs_kt

    def test_time_column_is_monotonic(self) -> None:
        """回放靠二分查找，t_s 非单调会查出乱段。"""
        s = G.derive(_synth_turn(n=400, rate_dps=1.0))
        text = G.emit_c(G.simplify(s), "unit.gpx", len(s))
        ts = [int(l.strip().rstrip(",").strip("{}").split(",")[2])
              for l in text.splitlines() if l.strip().startswith("{")]
        self.assertEqual(ts, sorted(ts))
        self.assertEqual(len(ts), len(set(ts)))


if __name__ == "__main__":
    unittest.main()
