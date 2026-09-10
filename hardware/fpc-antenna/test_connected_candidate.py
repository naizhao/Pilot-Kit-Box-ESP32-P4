#!/usr/bin/env python3
"""Contract and mutation tests for the connected-candidate PCB."""

from collections import Counter
import contextlib
import io
from pathlib import Path
import re
import tempfile
import unittest

import gen_connected_candidate
import verify_connected_candidate


pcbnew = verify_connected_candidate.pcbnew
MM = pcbnew.FromMM
BASELINE = Path(__file__).with_name("adsb-fpc-antenna.kicad_pcb")

# Independent acceptance values: never import these from the generator.
TRUNK = ((97.0, 5.6), (97.0, 92.975), 0.20)
TOPS = (6.0, 29.0, 52.0, 75.0)
BRANCHES = tuple(((57.8, top - 0.4), (97.0, top - 0.4), 0.20) for top in TOPS)
NECKS = tuple(((57.8, top - 0.4), (57.8, top + 0.7), 0.20) for top in TOPS)
MASK_WINDOWS = tuple((57.4, top + 0.1, 58.05, top + 0.8) for top in TOPS)
RESISTOR_COPPER = tuple(
    (cx - 0.5, top - 1.0, cx + 0.5, top + 0.2)
    for top in TOPS for cx in (90.6, 92.4)
)
RESISTOR_MASK = tuple(
    (x1 - 0.05, y1 - 0.05, x2 + 0.05, y2 + 0.05)
    for x1, y1, x2, y2 in RESISTOR_COPPER
)
IPEX_COPPER = (
    (94.95, 91.0, 96.0, 92.0),
    (95.9, 89.5, 98.1, 90.55),
    (95.9, 92.45, 98.1, 93.5),
)
IPEX_MASK = tuple(
    (x1 - 0.05, y1 - 0.05, x2 + 0.05, y2 + 0.05)
    for x1, y1, x2, y2 in IPEX_COPPER
)


def _points(item):
    return tuple(sorted((tuple(item.GetStart()), tuple(item.GetEnd()))))


def _signature(item):
    kind = type(item).__name__
    if kind == "PCB_TRACK":
        return ("track", item.GetLayer(), _points(item), item.GetWidth(), item.GetNetCode())
    if kind == "PCB_SHAPE":
        return ("shape", item.GetLayer(), item.GetShape(), _points(item),
                item.GetWidth(), item.IsSolidFill(), _shape_source(item))
    if kind == "PCB_TEXT":
        font = item.GetFont()
        return ("text", item.GetLayer(), item.GetText(), tuple(item.GetPosition()),
                tuple(item.GetTextSize()), item.GetTextThickness(),
                item.GetHorizJustify(), item.GetVertJustify(),
                item.GetTextAngle().AsDegrees(), item.IsMirrored(),
                item.IsItalic(), item.IsBold(), item.IsVisible(),
                font.GetName() if font else None)
    formatter = pcbnew.PCB_IO_KICAD_SEXPR()
    formatter.Format(item)
    return ("other", kind, formatter.GetStringOutput(True))


def _shape_source(item):
    formatter = pcbnew.PCB_IO_KICAD_SEXPR()
    formatter.Format(item)
    source = formatter.GetStringOutput(True)
    return re.sub(r'\(uuid "[^"]+"\)', "", source)


def _board_signatures(board):
    items = list(board.GetTracks()) + list(board.GetDrawings())
    items += list(board.GetFootprints()) + list(board.Zones())
    return Counter(_signature(item) for item in items)


def _track_signature(spec):
    start, end, width = spec
    return ("track", pcbnew.F_Cu,
            tuple(sorted(((MM(start[0]), MM(start[1])),
                          (MM(end[0]), MM(end[1]))))), MM(width), 0)


def _shape_signature(layer, shape, start, end, width, filled):
    item = pcbnew.PCB_SHAPE()
    item.SetShape(shape)
    item.SetStart(pcbnew.VECTOR2I(MM(start[0]), MM(start[1])))
    item.SetEnd(pcbnew.VECTOR2I(MM(end[0]), MM(end[1])))
    item.SetWidth(MM(width))
    item.SetFilled(filled)
    item.SetLayer(layer)
    return _signature(item)


def _text_signature(top):
    return ("text", pcbnew.F_SilkS, "CUT LINK",
            (MM(59.0), MM(top + 0.35)), (MM(0.6), MM(0.6)), MM(0.12),
            pcbnew.GR_TEXT_H_ALIGN_LEFT, pcbnew.GR_TEXT_V_ALIGN_CENTER,
            0.0, False, False, False, True, None)


def _plain_text_signature(content, x, y, size):
    return ("text", pcbnew.F_SilkS, content,
            (MM(x), MM(y)), (MM(size), MM(size)), MM(0.12),
            pcbnew.GR_TEXT_H_ALIGN_CENTER, pcbnew.GR_TEXT_V_ALIGN_CENTER,
            0.0, False, False, False, True, None)


def _expected_additions():
    result = Counter(_track_signature(spec) for spec in (TRUNK,) + BRANCHES + NECKS)
    for rectangle in RESISTOR_COPPER + IPEX_COPPER:
        x1, y1, x2, y2 = rectangle
        result[_shape_signature(
            pcbnew.F_Cu, pcbnew.SHAPE_T_RECT, (x1, y1), (x2, y2), 0.0, True
        )] += 1
    for window in MASK_WINDOWS:
        x1, y1, x2, y2 = window
        result[_shape_signature(
            pcbnew.F_Mask, pcbnew.SHAPE_T_RECT, (x1, y1), (x2, y2), 0.0, True
        )] += 1
    for window in RESISTOR_MASK + IPEX_MASK:
        x1, y1, x2, y2 = window
        result[_shape_signature(
            pcbnew.F_Mask, pcbnew.SHAPE_T_RECT, (x1, y1), (x2, y2), 0.0, True
        )] += 1
    for index, top in enumerate(TOPS, start=1):
        result[_shape_signature(
            pcbnew.F_SilkS, pcbnew.SHAPE_T_SEGMENT,
            (57.2, top + 0.35), (58.4, top + 0.35), 0.15, False
        )] += 1
        result[_text_signature(top)] += 1
        result[_plain_text_signature(f"R{index} 0R 0804", 91.5, top + 0.75, 0.45)] += 1
    result[_shape_signature(
        pcbnew.F_SilkS, pcbnew.SHAPE_T_RECT,
        (94.4, 89.2), (99.6, 93.8), 0.15, False
    )] += 1
    result[_plain_text_signature("IPEX GND", 97.0, 88.65, 0.55)] += 1
    return result


def _components(items):
    shapes = [item.GetEffectiveShape() for item in items]
    graph = [set() for _ in items]
    for left in range(len(items)):
        for right in range(left + 1, len(items)):
            if shapes[left].Collide(shapes[right]):
                graph[left].add(right)
                graph[right].add(left)
    labels = []
    for root in range(len(items)):
        seen = {root}
        pending = [root]
        while pending:
            node = pending.pop()
            for neighbor in graph[node] - seen:
                seen.add(neighbor)
                pending.append(neighbor)
        labels.append(frozenset(seen))
    return labels


def _find_rect(items, x1, y1, x2, y2):
    expected = tuple(sorted(((MM(x1), MM(y1)), (MM(x2), MM(y2)))))
    return next(index for index, item in enumerate(items)
                if type(item).__name__ == "PCB_SHAPE"
                and item.GetShape() == pcbnew.SHAPE_T_RECT
                and _points(item) == expected)


class ConnectedCandidateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = Path(tempfile.mkdtemp(prefix="fpc-connected-candidate-test-"))
        cls.candidate = cls.directory / "candidate.kicad_pcb"
        cls.baseline_before = BASELINE.read_bytes()
        generated = gen_connected_candidate.generate_candidate(cls.candidate)
        cls.baseline_after = BASELINE.read_bytes()
        cls.generated = Path(generated)

    def test_generation_does_not_overwrite_frozen_baseline(self):
        self.assertEqual(self.generated, self.candidate)
        self.assertTrue(self.candidate.is_file())
        self.assertEqual(self.baseline_after, self.baseline_before)

    def test_candidate_additions_are_exact_on_every_layer(self):
        baseline = pcbnew.LoadBoard(str(BASELINE))
        candidate = pcbnew.LoadBoard(str(self.candidate))
        actual = _board_signatures(candidate)
        frozen = _board_signatures(baseline)
        expected = _expected_additions()
        self.assertEqual(actual, frozen + expected)

        added_tracks = Counter(_signature(item) for item in candidate.GetTracks())
        added_tracks.subtract(Counter(_signature(item) for item in baseline.GetTracks()))
        added_tracks = +added_tracks
        self.assertEqual(sum(added_tracks.values()), 9)
        self.assertEqual(added_tracks, Counter(
            _track_signature(spec) for spec in (TRUNK,) + BRANCHES + NECKS
        ))

        added_mask = Counter(_signature(item) for item in candidate.GetDrawings()
                             if item.IsOnLayer(pcbnew.F_Mask))
        added_mask.subtract(Counter(_signature(item) for item in baseline.GetDrawings()
                                    if item.IsOnLayer(pcbnew.F_Mask)))
        added_mask = +added_mask
        self.assertEqual(sum(added_mask.values()), 15)

    def test_requested_0804_and_ipex_pads_are_on_the_existing_gnd_bus(self):
        board = pcbnew.LoadBoard(str(self.candidate))
        copper = [item for item in list(board.GetTracks()) + list(board.GetDrawings())
                  if item.IsOnLayer(pcbnew.F_Cu)]
        components = _components(copper)
        gnd = _find_rect(copper, 51.6, 6.5, 58.0, 11.5)
        resistor_pads = [_find_rect(copper, *rectangle) for rectangle in RESISTOR_COPPER]
        ipex_ground = [_find_rect(copper, *rectangle) for rectangle in IPEX_COPPER[1:]]
        ipex_signal = _find_rect(copper, *IPEX_COPPER[0])

        self.assertTrue(all(components[index] == components[gnd]
                            for index in resistor_pads + ipex_ground))
        self.assertNotEqual(components[ipex_signal], components[gnd])

    def test_actual_copper_connects_all_gnd_but_keeps_sig_ports_isolated(self):
        board = pcbnew.LoadBoard(str(self.candidate))
        copper = [item for item in list(board.GetTracks()) + list(board.GetDrawings())
                  if item.IsOnLayer(pcbnew.F_Cu)]
        components = _components(copper)
        gnd = [_find_rect(copper, 51.6, top + 0.5, 58.0, top + 5.5)
               for top in TOPS]
        sig = [_find_rect(copper, 42.0, top + 0.5, 50.4, top + 5.5)
               for top in TOPS]
        self.assertTrue(all(components[index] == components[gnd[0]] for index in gnd))
        self.assertTrue(all(components[index] != components[gnd[0]] for index in sig))
        self.assertEqual(len({components[index] for index in sig}), 4)

    def test_panel_outline_and_back_copper_remain_valid(self):
        board = pcbnew.LoadBoard(str(self.candidate))
        outlines = [item for item in board.GetDrawings()
                    if item.IsOnLayer(pcbnew.Edge_Cuts)
                    and type(item).__name__ == "PCB_SHAPE"
                    and item.GetShape() == pcbnew.SHAPE_T_RECT
                    and not item.IsSolidFill()
                    and _points(item) == ((MM(0.0), MM(0.0)), (MM(100.0), MM(100.0)))]
        self.assertEqual(len(outlines), 1)
        back_items = [item for item in list(board.GetTracks()) + list(board.GetDrawings())
                      if item.IsOnLayer(pcbnew.B_Cu)]
        self.assertEqual(back_items, [])

    def _mutant_failures(self, name, mutate):
        board = pcbnew.LoadBoard(str(self.candidate))
        mutate(board)
        path = self.directory / f"{name}.kicad_pcb"
        self.assertTrue(pcbnew.SaveBoard(str(path), board))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            failures = verify_connected_candidate.collect_failures(path, BASELINE)
        self.assertTrue(failures, output.getvalue())
        return failures

    def test_verifier_rejects_perturbed_added_coordinate(self):
        expected = _track_signature(TRUNK)

        def mutate(board):
            item = next(item for item in board.GetTracks() if _signature(item) == expected)
            end = item.GetEnd()
            item.SetEnd(pcbnew.VECTOR2I(end.x + MM(0.1), end.y))

        self._mutant_failures("perturbed-trunk", mutate)

    def test_verifier_rejects_deleted_added_object(self):
        expected = _track_signature(BRANCHES[1])

        def mutate(board):
            item = next(item for item in board.GetTracks() if _signature(item) == expected)
            board.Delete(item)

        self._mutant_failures("deleted-branch", mutate)

    def test_verifier_rejects_rounded_gnd_tongue_that_misses_neck(self):
        board = pcbnew.LoadBoard(str(self.candidate))
        copper = [item for item in list(board.GetTracks()) + list(board.GetDrawings())
                  if item.IsOnLayer(pcbnew.F_Cu)]
        gnd_index = _find_rect(copper, 51.6, 6.5, 58.0, 11.5)
        neck = next(item for item in board.GetTracks()
                    if _points(item) == ((MM(57.8), MM(5.6)),
                                         (MM(57.8), MM(6.7))))
        copper[gnd_index].SetCornerRadius(MM(2.5))
        path = self.directory / "rounded-gnd-tongue.kicad_pcb"
        self.assertTrue(pcbnew.SaveBoard(str(path), board))

        reloaded = pcbnew.LoadBoard(str(path))
        reloaded_copper = [
            item for item in list(reloaded.GetTracks()) + list(reloaded.GetDrawings())
            if item.IsOnLayer(pcbnew.F_Cu)
        ]
        rounded = reloaded_copper[_find_rect(reloaded_copper, 51.6, 6.5, 58.0, 11.5)]
        reloaded_neck = next(item for item in reloaded.GetTracks()
                             if _points(item) == ((MM(57.8), MM(5.6)),
                                                  (MM(57.8), MM(6.7))))
        self.assertEqual(rounded.GetCornerRadius(), MM(2.5))
        self.assertFalse(
            rounded.GetEffectiveShape().Collide(reloaded_neck.GetEffectiveShape()),
            "rounded GND tongue must be a real open circuit at the neck",
        )

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            failures = verify_connected_candidate.collect_failures(path, BASELINE)
        self.assertTrue(failures, output.getvalue())
        self.assertTrue(
            any("GND tongues are not" in failure for failure in failures),
            failures,
        )

    def test_verifier_rejects_saved_and_reloaded_board_thickness_mutation(self):
        board = pcbnew.LoadBoard(str(self.candidate))
        board.GetDesignSettings().SetBoardThickness(MM(1.6))
        path = self.directory / "thickness-1.6.kicad_pcb"
        self.assertTrue(pcbnew.SaveBoard(str(path), board))
        reloaded = pcbnew.LoadBoard(str(path))
        self.assertEqual(reloaded.GetDesignSettings().GetBoardThickness(), MM(1.6))

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            failures = verify_connected_candidate.collect_failures(path, BASELINE)
        self.assertTrue(failures, output.getvalue())
        self.assertTrue(any(failure.startswith("fabrication:") for failure in failures),
                        failures)

    def test_verifier_rejects_candidate_stackup_material_mutation(self):
        source = self.candidate.read_text()
        old = '(material "Polyimide")'
        new = '(material "FR4")'
        self.assertEqual(source.count(old), 1)
        path = self.directory / "material-fr4.kicad_pcb"
        path.write_text(source.replace(old, new, 1))
        reloaded = pcbnew.LoadBoard(str(path))
        self.assertEqual(reloaded.GetDesignSettings().GetBoardThickness(), MM(0.11))
        self.assertIn(new, path.read_text())

        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            failures = verify_connected_candidate.collect_failures(path, BASELINE)
        self.assertTrue(failures, output.getvalue())
        self.assertTrue(any(failure.startswith("fabrication:") for failure in failures),
                        failures)

    def test_verifier_rejects_non_copper_baseline_drift(self):
        def mutate(board):
            item = next(item for item in board.GetDrawings()
                        if type(item).__name__ == "PCB_TEXT" and item.GetText() == "SIG")
            position = item.GetPosition()
            item.SetPosition(pcbnew.VECTOR2I(position.x + MM(0.1), position.y))

        self._mutant_failures("silk-drift", mutate)

    def test_independent_verifier_accepts_candidate(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = verify_connected_candidate.verify(self.candidate, BASELINE)
        self.assertEqual(result, 0, output.getvalue())
        self.assertIn("PASS", output.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
