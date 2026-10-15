"""用实际 PCB 坐标、OpenSCAD 求值和导出网格验证外壳。"""

import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest

from export_a1mini import validate_mesh


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
SCAD = HERE / "pilot-kit-box-43-base.scad"
PCB = ROOT / "hardware/expansion-board-v4/kicad/expansion-board-v4.kicad_pcb"
OPENSCAD = os.environ.get("OPENSCAD", "/Applications/OpenSCAD.app/Contents/MacOS/OpenSCAD")


def pcb_footprints():
    # KiCad 顶层 footprint 块；嵌套字段保持原样，只读取块自身的 at。
    result = {}
    for block in re.split(r"\n\t\(footprint ", PCB.read_text())[1:]:
        ref = re.search(r'\(property "Reference" "([^"]+)"', block)
        at = re.search(r"\n\t\t\(at ([^\)]+)\)", block)
        if ref and at:
            result[ref[1]] = [float(v) for v in at[1].split()[:2]]
    return result


def scad_run(source, output, defines=()):
    command = [OPENSCAD, "--backend=Manifold", "-o", str(output)]
    for value in defines:
        command.extend(["-D", value])
    result = subprocess.run(command + [str(source)], text=True, capture_output=True, timeout=180)
    if result.returncode or re.search(r"^(ERROR|WARNING):", result.stderr, re.M):
        raise AssertionError(result.stdout + result.stderr)
    return result


def evaluate(expression, defines=()):
    with tempfile.TemporaryDirectory(prefix="pilot-case-test-") as directory:
        source = Path(directory) / "evaluate.scad"
        source.write_text(f'include <{SCAD}>\necho("__TEST__", {expression});\n')
        result = scad_run(source, Path(directory) / "evaluate.csg", ['part=""', *defines])
        match = re.search(r'^ECHO: "__TEST__", (.+)$', result.stderr, re.M)
        if not match:
            raise AssertionError(result.stderr)
        return json.loads(match[1].replace("undef", "null"))


def triangles(path):
    data = path.read_bytes()
    if len(data) >= 84 and 84 + struct.unpack_from("<I", data, 80)[0] * 50 == len(data):
        return [tuple(tuple(v[j:j + 3]) for j in (3, 6, 9))
                for v in struct.iter_unpack("<12fH", data[84:])]
    vertices = [tuple(map(float, row)) for row in
                re.findall(rb"vertex\s+([-+\deE.]+)\s+([-+\deE.]+)\s+([-+\deE.]+)", data)]
    if not vertices:
        raise AssertionError("STL 没有三角形")
    return [tuple(vertices[i:i + 3]) for i in range(0, len(vertices), 3)]


def contains(mesh, point):
    """朝 +X 射线与三角面求交；轻微偏移避免采样恰好落在网格边上。"""
    x, y, z = point
    y += 0.000031
    z += 0.000073
    hits = []
    for a, b, c in mesh:
        by, bz, cy, cz = b[1] - a[1], b[2] - a[2], c[1] - a[1], c[2] - a[2]
        determinant = by * cz - bz * cy
        if abs(determinant) < 1e-12:
            continue
        u = ((y - a[1]) * cz - (z - a[2]) * cy) / determinant
        v = (by * (z - a[2]) - bz * (y - a[1])) / determinant
        if u >= 0 and v >= 0 and u + v <= 1:
            distance = a[0] + u * (b[0] - a[0]) + v * (c[0] - a[0]) - x
            if distance > 1e-7:
                hits.append(round(distance, 6))
    return len(set(hits)) % 2 == 1


class BoardAlignmentTests(unittest.TestCase):
    def test_four_mounting_holes_match_real_v411_pcb(self):
        footprints = pcb_footprints()
        actual = sorted([[x - 48.75, 111 - y] for ref, (x, y) in footprints.items()
                         if ref in ("H1", "H2", "H3", "H4")])
        local = [[x - 50, y - 50] for ref, (x, y) in footprints.items()
                 if ref in ("H1", "H2", "H3", "H4")]
        calculated = evaluate(f"[for (p = {json.dumps(local)}) [exp_x(p[0]), exp_y(p[1])]]")
        self.assertEqual(len(actual), 4)
        self.assertEqual(sorted(calculated), actual)
        self.assertEqual(sorted(evaluate("boss_xy")), actual)

    def test_interface_centers_follow_real_v411_pcb(self):
        board = pcb_footprints()
        result = evaluate("[j4_y, sw3_y, led_ys]")
        origins = evaluate("[j4_pcb, sw3_pcb, led_pcb[0], led_pcb[1]]")
        for actual, reference in zip(origins, ("J4", "SW3", "D8", "D9")):
            self.assertEqual(actual, [round(v - 50, 4) for v in board[reference]])
        self.assertAlmostEqual(result[0], 111 - board["J4"][1], places=4)
        self.assertAlmostEqual(result[1], 111 - board["SW3"][1], places=4)
        for actual, reference in zip(result[2], ("D8", "D9")):
            self.assertAlmostEqual(actual, 111 - board[reference][1], places=4)

    def test_fdm_defaults_and_switch_margin(self):
        values = evaluate("[fit_glass, foot_h, emb_sz, emb_h, sw3_gap_wc, sw3_need, sw3_proud, sw3_guide - sw3_bore_in - pl_flange_t, mount_plate]")
        self.assertEqual(values[:3], [0.25, 0, 2.5])
        self.assertLessEqual(values[3], 0.5)
        self.assertGreater(values[4], 0.1)
        self.assertLess(values[5], values[6])
        self.assertGreaterEqual(values[7], values[5])
        self.assertFalse(values[8])

    def test_j4_and_led_height_can_be_calibrated_independently(self):
        before = evaluate("[j4_z, led_z, sw3_z]")
        after = evaluate("[j4_z, led_z, sw3_z]", ["j4_ctr_z=2.7", "led_ctr_z=1.3"])
        board_z = evaluate("z_exp_top")
        self.assertAlmostEqual(after[0], board_z + 2.7)
        self.assertAlmostEqual(after[1], board_z + 1.3)
        self.assertEqual(after[2], before[2])

    def test_calibration_cannot_silently_remove_led_rims(self):
        for definition in ("j4_w=13", "sw3_rec_d=8", "led_window_d=2"):
            with self.subTest(definition=definition):
                with self.assertRaisesRegex(AssertionError, "ERROR: Assertion.*LED"):
                    evaluate("box_h", [definition])

    def test_expansion_support_adjusts_from_fixed_glass_datum(self):
        expr = "[z_glass_b, z_scr_bot, box_h, z_scr_bot+sd_zoff, z_scr_bot+usb_zoff, z_exp_bot, z_exp_top, bat_h, j4_z, sw3_z, led_z]"
        before = evaluate(expr)
        for change in (-0.5, 0.5):
            with self.subTest(adjust=change):
                after = evaluate(expr, [f"exp_support_adjust={change}"])
                self.assertEqual(after[:5], before[:5])
                for value, original in zip(after[5:], before[5:]):
                    self.assertAlmostEqual(value-original, change, places=4)

    def test_glass_to_screen_depth_is_an_independent_calibration(self):
        before = evaluate("[z_glass_b, z_scr_bot, z_exp_top, box_h]")
        depth = evaluate("glass_to_screen_back")
        self.assertIsNotNone(depth)
        after = evaluate("[z_glass_b, z_scr_bot, z_exp_top, box_h]", [f"glass_to_screen_back={depth+0.3}"])
        self.assertEqual(after[0], before[0])
        self.assertEqual(after[3], before[3])
        self.assertAlmostEqual(after[1], before[1]-0.3, places=4)
        self.assertAlmostEqual(after[2], before[2]-0.3, places=4)

    def test_excessive_support_raise_cannot_hit_screen_silently(self):
        with self.assertRaisesRegex(AssertionError, "ERROR: Assertion.*导向套顶"):
            evaluate("box_h", ["exp_support_adjust=2"])


class StageOneMeshTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="pilot-case-mesh-")
        cls.path = Path(cls.directory.name)
        scad_run(SCAD, cls.path / "box.stl", ['part="box"', "align_origin=false", "labels_enabled=false"])
        cls.mesh = triangles(cls.path / "box.stl")

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_usb_and_both_led_windows_are_open_in_real_wall(self):
        # 采样 J4 下半部，避免只测到与 H1 共用的开口而漏掉 J4 未开孔。
        wall_x, usb_y, usb_z, led_ys, led_z = evaluate("[(box_x1+cav_x1)/2, j4_y, j4_z-j4_h/2+0.6, led_ys, led_z]")
        for y, z in [(usb_y, usb_z), *[(y, led_z) for y in led_ys]]:
            with self.subTest(center=(y, z)):
                self.assertFalse(contains(self.mesh, (wall_x, y, z)))
        self.assertTrue(contains(self.mesh, (wall_x, led_ys[0], led_z - 2.0)))

    def test_support_adjust_really_raises_posts_without_raising_shell(self):
        scad_run(SCAD, self.path / "raised.stl", ['part="box"', "align_origin=false", "labels_enabled=false", "exp_support_adjust=0.5"])
        mesh = triangles(self.path / "raised.stl")
        x, y, top, wall_x, wall_y, glass_z = evaluate("[boss_xy[0][0]+2.2, boss_xy[0][1], z_exp_bot, (cav_x0+box_x0)/2, 30, z_glass_b]")
        self.assertFalse(contains(self.mesh, (x, y, top+0.25)))
        self.assertTrue(contains(mesh, (x, y, top+0.25)))
        for item in (self.mesh, mesh):
            self.assertTrue(contains(item, (wall_x, wall_y, glass_z+0.5)))
        self.assertEqual(validate_mesh(mesh)["bounds_mm"], validate_mesh(self.mesh)["bounds_mm"])

    def test_j4_cable_moulding_can_reach_recessed_socket(self):
        wall_x, usb_y, usb_z = evaluate("[(box_x1+cav_x1)/2, j4_y, j4_z]")
        # J4 缩进约 7.5 mm，须让公头包胶穿墙，不能只给金属头留高度。
        self.assertFalse(contains(self.mesh, (wall_x, usb_y, usb_z - 3)))
        self.assertFalse(contains(self.mesh, (wall_x, usb_y, usb_z + 3)))

    def test_both_led_windows_keep_solid_rims_clear_of_all_recesses(self):
        outer_x, inner_x, ys, z, diameter = evaluate("[box_x1-0.1, cav_x1+0.1, led_ys, led_z, led_window_d]")
        # 检查整圈实体，不能只检查孔中心为空。0.7 mm 为此布局的最小留边。
        for index, y in enumerate(ys):
            with self.subTest(led=index):
                missing = []
                for x in (outer_x, inner_x):
                    for rim in (0.05, 0.35, 0.68):
                        radius = diameter / 2 + rim
                        for angle in range(0, 360, 15):
                            a = math.radians(angle)
                            point = (x, y + radius * math.cos(a), z + radius * math.sin(a))
                            if not contains(self.mesh, point):
                                missing.append(point)
                self.assertFalse(missing, f"LED {index} 周围被切去的实体点：{missing[:3]}")

    def test_usb_recess_continues_around_lower_socket_with_floor_remaining(self):
        outer_x, depth, y, z, width, height, upper_z = evaluate("[box_x1, rec_depth, j4_y, j4_z, j4_w, j4_h, z_scr_bot+usb_zoff]")
        # 下沿和远离 LED 的整条侧沿，必须有连续浅槽且保留槽底。
        points = [(y + dy, z - height / 2 - 0.5) for dy in (-3, 0, 3)]
        points += [(y + width / 2 + 0.7, z + (upper_z-z)*i/8) for i in range(9)]
        for yy, zz in points:
            with self.subTest(point=(yy, zz)):
                self.assertFalse(contains(self.mesh, (outer_x - depth / 2, yy, zz)))
                self.assertTrue(contains(self.mesh, (outer_x - depth - 0.15, yy, zz)))

    def test_usb_has_same_straight_sides_at_both_socket_heights(self):
        x, y1, z1, w1, y2, z2, w2 = evaluate("[(cav_x1+box_x1)/2, h1_y, z_scr_bot+usb_zoff, usb_w, j4_y, j4_z, j4_w]")
        left, right = min(y1-w1/2, y2-w2/2), max(y1+w1/2, y2+w2/2)
        for z in (z1, z2):
            for edge, direction in ((left, 1), (right, -1)):
                self.assertFalse(contains(self.mesh, (x, edge+direction*0.04, z)))
                self.assertTrue(contains(self.mesh, (x, edge-direction*0.04, z)))

    def test_power_and_usb_engraving_share_actual_vertical_center(self):
        path = self.path / "aligned-labels.stl"
        scad_run(SCAD, path, ['part="box"', "align_origin=false"])
        mesh = triangles(path)
        x, usb_y, power_y = evaluate("[box_x1, h1_y, sw3_y]")
        centers = []
        for y in (usb_y, power_y):
            zs = [p[2] for tri in mesh for p in tri
                  if x-0.4 < p[0] < x-0.1 and abs(p[1]-y) < 4 and 6 < p[2] < 11]
            self.assertTrue(zs)
            centers.append((min(zs)+max(zs))/2)
        self.assertAlmostEqual(centers[0], centers[1], delta=0.05)

    def test_default_sw3_plungers_exist_without_optional_screen_buttons(self):
        scad_run(SCAD, self.path / "plungers.stl", ['part="plungers"'])
        mesh = triangles(self.path / "plungers.stl")
        count = evaluate("n_plungers")
        validate_mesh(mesh, expected_components=count)
        for i in range(count):
            self.assertTrue(contains(mesh, (i * 12, 0, 0.4)))
        self.assertEqual(min(p[2] for triangle in mesh for p in triangle), 0)

    def test_box_is_one_closed_body_and_print_has_no_attached_parts(self):
        info = validate_mesh(self.mesh)
        self.assertEqual(info["bounds_mm"][0][2], 0)
        scad_run(SCAD, self.path / "print.stl", ['part="print"', "align_origin=false", "labels_enabled=false"])
        printed = validate_mesh(triangles(self.path / "print.stl"))
        self.assertAlmostEqual(printed["volume_mm3"], info["volume_mm3"], places=3)

    def test_labels_can_be_disabled_without_changing_outer_dimensions(self):
        scad_run(SCAD, self.path / "labelled.stl", ['part="box"', "align_origin=false"])
        labelled = validate_mesh(triangles(self.path / "labelled.stl"))
        plain = validate_mesh(self.mesh)
        self.assertEqual(labelled["size_mm"], plain["size_mm"])
        self.assertGreater(plain["volume_mm3"] - labelled["volume_mm3"], 1)

    def test_engraving_depth_matches_configured_shallow_depth(self):
        scad_run(SCAD, self.path / "text-depth.stl", ['part="box"', "align_origin=false"])
        mesh = triangles(self.path / "text-depth.stl")
        outer_x, y, z, depth = evaluate("[box_x1, h1_y, usb_lbl_z, emb_h]")
        recesses = [outer_x - p[0] for triangle in mesh for p in triangle
                    if abs(p[1] - y) < 6 and abs(p[2] - z) < 2 and p[0] > outer_x - 1]
        self.assertAlmostEqual(max(recesses), depth, places=4)


class TopAntennaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="pilot-case-top-")
        cls.path = Path(cls.directory.name)
        cls.meshes = {}
        for name, definitions in {
            "patch": ['gps_mode="patch"'],
            "ceramic46": ['gps_mode="ceramic"', "gps_ceramic_h=4.6"],
            "ceramic66": ['gps_mode="ceramic"', "gps_ceramic_h=6.6"],
            "none": ['gps_mode="none"'],
        }.items():
            scad_run(SCAD, cls.path / f"{name}.stl", ['part="box"', "align_origin=false", "labels_enabled=false", *definitions])
            cls.meshes[name] = triangles(cls.path / f"{name}.stl")

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_three_top_sma_holes_and_closed_old_side(self):
        xs, z, outer_y, inner_y, left_x = evaluate("[sma_x, sma_z, top_y1, top_inner_y1, (box_x0+cav_x0)/2]")
        self.assertEqual(len(xs), 3)
        for mesh in self.meshes.values():
            for x in xs:
                for y in (inner_y + 0.2, outer_y - 0.2):
                    self.assertFalse(contains(mesh, (x, y, z)))
                self.assertTrue(contains(mesh, (x + 6, outer_y - 0.8, z)))
            for old_y in (19, 41):
                self.assertTrue(contains(mesh, (left_x, old_y, 19)))

    def test_patch_has_thin_wall_and_room_for_real_40_by_6_antenna(self):
        x, z, outer_y, wall, width, height = evaluate("[gps_cx, gps_patch_z, top_y1, gps_wall_t, gps_patch_size[0], gps_patch_size[1]]")
        self.assertEqual([width, height], [40, 6])
        self.assertEqual(wall, 0.8)
        mesh = self.meshes["patch"]
        for xx in (x - width / 2 + 0.3, x, x + width / 2 - 0.3):
            for zz in (z - height / 2 + 0.3, z, z + height / 2 - 0.3):
                self.assertTrue(contains(mesh, (xx, outer_y - wall / 2, zz)))
                self.assertFalse(contains(mesh, (xx, outer_y - wall - 0.2, zz)))
        self.assertTrue(contains(mesh, (x + width / 2 + 2, outer_y - 1.3, z)))
        self.assertTrue(contains(self.meshes["none"], (x, outer_y - 1.3, z)))

    def test_ceramic_sizes_have_installation_opening_and_full_depth(self):
        for name, height in (("ceramic46", 4.6), ("ceramic66", 6.6)):
            with self.subTest(preset=name):
                definitions = ['gps_mode="ceramic"', f"gps_ceramic_h={height}"]
                x, z, side, back_y, front_y, old_wall, cap, outer_y = evaluate("[gps_cx, gps_ceramic_z, gps_ceramic_size, gps_back_y, gps_ceramic_face_y, box_y1, gps_ceramic_cap_t, gps_ceramic_y1]", definitions)
                self.assertAlmostEqual(front_y - back_y, height)
                mesh = self.meshes[name]
                for y in (old_wall - 0.3, old_wall + 0.3, back_y, front_y - 0.1):
                    for xx in (x - side / 2, x, x + side / 2):
                        for zz in (z - side / 2, z, z + side / 2):
                            self.assertFalse(contains(mesh, (xx, y, zz)), (name, xx, y, zz))
                self.assertTrue(contains(mesh, (x, outer_y - cap / 2, z)))

    def test_sma_nuts_tails_and_gps_keep_clear_of_board_and_each_other(self):
        values = evaluate("[sma_z-sma_cb_d/2-floor_t, top_h-top_roof_t-sma_z-sma_cb_d/2, top_inner_y1-sma_tail_l-exp_y(0), gps_back_y-exp_y(0), min(sma_x)-sma_cb_d/2-gps_cx-gps_patch_size[0]/2, gps_clearance]")
        self.assertGreaterEqual(values[0], 0.25)
        self.assertGreaterEqual(values[1], 0.25)
        self.assertGreater(values[2], 0.5)
        self.assertGreaterEqual(values[3], values[5])
        self.assertGreaterEqual(values[4], values[5])

    def test_sd_path_remains_open_and_top_bay_is_connected(self):
        x, z, old_wall, end_y, roof_z = evaluate("[sd_cx, z_scr_bot+sd_zoff, box_y1, top_y1, top_h-top_roof_t/2]")
        for mesh in self.meshes.values():
            validate_mesh(mesh)
            for y in (old_wall - 0.3, old_wall + 2, end_y - 3, end_y, end_y + 12):
                for xx in (x - 4, x, x + 4):
                    self.assertFalse(contains(mesh, (xx, y, z)))
            self.assertFalse(contains(mesh, (78, old_wall, 7.5)))
            self.assertTrue(contains(mesh, (78, old_wall + 5, roof_z)))

    def test_invalid_modes_or_colliding_envelopes_are_rejected(self):
        for definition in ('gps_mode="typo"', 'sma_tail_l=25', 'sma_x=[17,78,98]', 'gps_ceramic_h=-1'):
            with self.subTest(definition=definition):
                with self.assertRaisesRegex(AssertionError, "ERROR: Assertion"):
                    evaluate("box_h", [definition])


if __name__ == "__main__":
    unittest.main()
