#!/usr/bin/env python3
"""Real PCB / KiCad Gerber-job regression tests for the FPC construction."""

import contextlib
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

import verify_panel


pcbnew = verify_panel.pcbnew
BASE_BOARD = Path(__file__).with_name("adsb-fpc-antenna.kicad_pcb")
KICAD_CLI = Path.home() / "Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"
EXPECTED_FOOTER = (
    "ADSB-FPC REV-A3.1F  JLC 2L 0.11mm PI / 1/3oz ED / "
    "YEL COVERLAY 25um x2 / ENIG 1uin / NO SHIELD"
)


class FabricationMetadataTests(unittest.TestCase):
    def test_all_front_silkscreen_bounding_boxes_are_inside_panel(self):
        board = pcbnew.LoadBoard(str(BASE_BOARD))
        items = [item for item in board.GetDrawings() if item.IsOnLayer(pcbnew.F_SilkS)]
        self.assertEqual(len(items), 41)
        for item in items:
            box = item.GetBoundingBox()
            bounds = tuple(pcbnew.ToMM(value) for value in (
                box.GetX(), box.GetY(), box.GetX() + box.GetWidth(),
                box.GetY() + box.GetHeight(),
            ))
            label = item.GetText() if type(item).__name__ == "PCB_TEXT" else "PCB_SHAPE"
            with self.subTest(item=label, bounds_mm=bounds):
                self.assertTrue(0 <= bounds[0] <= bounds[2] <= 100
                                and 0 <= bounds[1] <= bounds[3] <= 100,
                                f"F.SilkS bbox outside 100 x 100 mm panel: {bounds}")

    def test_actual_export_front_silkscreen_coordinates_are_inside_panel(self):
        directory = Path(tempfile.mkdtemp(prefix="fpc-silk-bounds-export-"))
        result = subprocess.run(
            [str(KICAD_CLI), "pcb", "export", "gerbers", str(BASE_BOARD),
             "--output", str(directory), "--layers", "F.Cu,B.Cu,F.Mask,F.SilkS,Edge.Cuts"],
            capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        paths = list(directory.glob("*-F_Silkscreen.gto"))
        self.assertEqual(len(paths), 1, str(directory))
        source = paths[0].read_text()
        self.assertIn("%FSLAX46Y46*%", source)
        self.assertIn("%MOMM*%", source)
        coordinates = []
        x = y = None
        for line in source.splitlines():
            if not line.startswith(("X", "Y")):
                continue
            command = re.fullmatch(r"(?:X(-?\d+))?(?:Y(-?\d+))?D0[123]\*", line)
            self.assertIsNotNone(command, f"Unexpected coordinate command: {line}")
            if command.group(1) is not None:
                x = int(command.group(1)) / 1_000_000
            if command.group(2) is not None:
                y = int(command.group(2)) / 1_000_000
            self.assertIsNotNone(x)
            self.assertIsNotNone(y)
            coordinates.append((x, y))
        self.assertTrue(coordinates, f"No drawing coordinates in {paths[0]}")
        outside = [(x, y) for x, y in coordinates if not (0 <= x <= 100 and -100 <= y <= 0)]
        self.assertFalse(outside,
                         f"{paths[0]}: {len(outside)} out-of-panel commands; "
                         f"X range={min(x for x, _ in coordinates)}.."
                         f"{max(x for x, _ in coordinates)} mm; first={outside[:3]}")

    def test_source_board_has_fpc_thickness_and_two_copper_layers(self):
        board = pcbnew.LoadBoard(str(BASE_BOARD))
        self.assertEqual(board.GetCopperLayerCount(), 2)
        self.assertEqual(board.GetDesignSettings().GetBoardThickness(), pcbnew.FromMM(0.11))

    def test_actual_export_has_fpc_construction(self):
        # Retain temporary evidence; never touch release Gerbers or remove files.
        directory = Path(tempfile.mkdtemp(prefix="fpc-metadata-export-"))
        result = subprocess.run(
            [str(KICAD_CLI), "pcb", "export", "gerbers", str(BASE_BOARD),
             "--output", str(directory), "--layers", "F.Cu,B.Cu,F.Mask,F.SilkS,Edge.Cuts"],
            capture_output=True, text=True, timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        jobs = list(directory.glob("*.gbrjob"))
        self.assertEqual(len(jobs), 1)
        job = json.loads(jobs[0].read_text())
        general = job["GeneralSpecs"]
        copper = [item for item in job["MaterialStackup"] if item["Type"] == "Copper"]
        dielectric = [item for item in job["MaterialStackup"] if item["Type"] == "Dielectric"]
        mask = [item for item in job["MaterialStackup"] if item["Type"] == "SolderMask"]
        for key, expected in (("BoardThickness", 0.11), ("LayerNumber", 2), ("Finish", "ENIG")):
            with self.subTest(field=key):
                self.assertEqual(general.get(key), expected)
        with self.subTest(field="copper"):
            self.assertEqual([(item["Name"], item["Thickness"]) for item in copper],
                             [("F.Cu", 0.012), ("B.Cu", 0.012)])
        with self.subTest(field="dielectric"):
            self.assertEqual([(item["Material"], item["Thickness"]) for item in dielectric],
                             [("Polyimide", 0.036)])
        with self.subTest(field="coverlay"):
            self.assertEqual([(item["Name"], item["Thickness"]) for item in mask],
                             [("Top Solder Mask", 0.025), ("Bottom Solder Mask", 0.025)])
        with self.subTest(field="total_stackup"):
            self.assertAlmostEqual(sum(item.get("Thickness", 0) for item in job["MaterialStackup"]),
                                   0.11, places=6)
        with self.subTest(field="no_FR4"):
            self.assertNotIn("FR4", json.dumps(job))

    def test_footer_states_known_fabrication_parameters(self):
        board = pcbnew.LoadBoard(str(BASE_BOARD))
        footer = next(item for item in board.GetDrawings()
                      if type(item).__name__ == "PCB_TEXT"
                      and item.GetPosition() == pcbnew.VECTOR2I(pcbnew.FromMM(5), pcbnew.FromMM(94.6)))
        self.assertEqual(footer.GetText(), EXPECTED_FOOTER)

    def test_generator_produces_an_accepted_fpc_board(self):
        directory = Path(tempfile.mkdtemp(prefix="fpc-metadata-generated-"))
        path = directory / "generated.kicad_pcb"
        result = subprocess.run(
            [sys.executable, str(BASE_BOARD.with_name("gen_panel.py")), str(path)],
            capture_output=True, text=True, timeout=60,
        )
        diagnostics = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, diagnostics)
        self.assertNotIn("create wxApp before calling this", diagnostics)
        self.assertNotIn("assert \"\"traits\"\" failed", diagnostics)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = verify_panel.verify(str(path))
        self.assertEqual(status, 0, output.getvalue())

    def test_verifier_rejects_manufacturing_metadata_mutations(self):
        # Independent compliant construction fixture, needed to show that each
        # single-field mutation is rejected without relying on another failure.
        source = BASE_BOARD.read_text()
        source = re.sub(r"\n\t\t\(stackup\b.*?\n\t\t\)", "", source, flags=re.S)
        source, count = re.subn(r"(\(general\s+\(thickness )[^)]+", r"\g<1>0.11", source)
        self.assertEqual(count, 1)
        stackup = '''
		(stackup
			(layer "F.SilkS" (type "Top Silk Screen"))
			(layer "F.Paste" (type "Top Solder Paste"))
			(layer "F.Mask" (type "Top Solder Mask") (thickness 0.025))
			(layer "F.Cu" (type "copper") (thickness 0.012))
			(layer "dielectric 1" (type "core") (thickness 0.036) (material "Polyimide"))
			(layer "B.Cu" (type "copper") (thickness 0.012))
			(layer "B.Mask" (type "Bottom Solder Mask") (thickness 0.025))
			(layer "B.Paste" (type "Bottom Solder Paste"))
			(layer "B.SilkS" (type "Bottom Silk Screen"))
			(copper_finish "ENIG")
		)'''
        self.assertEqual(source.count("\n\t(setup"), 1)
        source = source.replace("\n\t(setup", "\n\t(setup" + stackup, 1)
        directory = Path(tempfile.mkdtemp(prefix="fpc-metadata-mutants-"))
        control = directory / "control.kicad_pcb"
        control.write_text(source)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            status = verify_panel.verify(str(control))
        self.assertEqual(status, 0, output.getvalue())
        for name, old, new in (
            ("board thickness", "(thickness 0.11)", "(thickness 1.6)"),
            ("material", '(material "Polyimide")', '(material "FR4")'),
            ("front copper", '(layer "F.Cu" (type "copper") (thickness 0.012))',
             '(layer "F.Cu" (type "copper") (thickness 0.035))'),
            ("back copper", '(layer "B.Cu" (type "copper") (thickness 0.012))',
             '(layer "B.Cu" (type "copper") (thickness 0.035))'),
            ("finish", '(copper_finish "ENIG")', '(copper_finish "None")'),
            ("dielectric thickness", '(thickness 0.036)', '(thickness 1.51)'),
            ("front coverlay", '(layer "F.Mask" (type "Top Solder Mask") (thickness 0.025))',
             '(layer "F.Mask" (type "Top Solder Mask") (thickness 0))'),
            ("back coverlay", '(layer "B.Mask" (type "Bottom Solder Mask") (thickness 0.025))',
             '(layer "B.Mask" (type "Bottom Solder Mask") (thickness 0))'),
        ):
            with self.subTest(mutation=name):
                self.assertEqual(source.count(old), 1)
                path = directory / (name.replace(" ", "-") + ".kicad_pcb")
                path.write_text(source.replace(old, new, 1))
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    result = verify_panel.verify(str(path))
                self.assertNotEqual(result, 0, output.getvalue())
                self.assertIn("\n  - fabrication:", output.getvalue())


if __name__ == "__main__":
    unittest.main()
