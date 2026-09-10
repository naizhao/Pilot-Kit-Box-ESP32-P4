#!/usr/bin/env python3
"""Mutation regression tests for verify_panel.py using real pcbnew objects."""

import contextlib
import io
from pathlib import Path
import tempfile
import unittest

import verify_panel


pcbnew = verify_panel.pcbnew
BASE_BOARD = Path(__file__).with_name("adsb-fpc-antenna.kicad_pcb")
MM = pcbnew.FromMM


def filled_rect(parent, layer, x1, y1, x2, y2):
    shape = pcbnew.PCB_SHAPE(parent)
    shape.SetShape(pcbnew.SHAPE_T_RECT)
    shape.SetStart(pcbnew.VECTOR2I(MM(x1), MM(y1)))
    shape.SetEnd(pcbnew.VECTOR2I(MM(x2), MM(y2)))
    shape.SetFilled(True)
    shape.SetWidth(0)
    shape.SetLayer(layer)
    return shape


def segment(parent, layer, x1, y1, x2, y2, width):
    shape = pcbnew.PCB_SHAPE(parent)
    shape.SetShape(pcbnew.SHAPE_T_SEGMENT)
    shape.SetStart(pcbnew.VECTOR2I(MM(x1), MM(y1)))
    shape.SetEnd(pcbnew.VECTOR2I(MM(x2), MM(y2)))
    shape.SetWidth(MM(width))
    shape.SetLayer(layer)
    return shape


class VerifyPanelMutationTests(unittest.TestCase):
    def verify_board(self, mutate=None):
        board = pcbnew.LoadBoard(str(BASE_BOARD))
        if mutate is not None:
            mutate(board)
        with tempfile.TemporaryDirectory(prefix="verify-panel-") as temp_dir:
            path = Path(temp_dir) / "mutant.kicad_pcb"
            pcbnew.SaveBoard(str(path), board)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                result = verify_panel.verify(str(path))
        return result, output.getvalue()

    def assert_rejected(self, mutate):
        result, output = self.verify_board(mutate)
        self.assertNotEqual(result, 0, output)

    def test_accepts_frozen_baseline_board(self):
        result, output = self.verify_board()
        self.assertEqual(result, 0, output)

    def test_baseline_text_obeys_authoritative_positions_and_alignment(self):
        board = pcbnew.LoadBoard(str(BASE_BOARD))
        texts = [item for item in board.GetDrawings()
                 if type(item).__name__ == "PCB_TEXT"]
        for top in (6.0, 29.0, 52.0, 75.0):
            for content, x in (("SIG", 46.0), ("GND", 54.5)):
                with self.subTest(top=top, text=content):
                    self.assertTrue(any(
                        item.GetText() == content
                        and item.GetPosition() == pcbnew.VECTOR2I(MM(x), MM(top + 4.5))
                        for item in texts
                    ), f"{content} must be at ({x}, {top + 4.5})")
            label = next(item for item in texts
                         if item.GetPosition() == pcbnew.VECTOR2I(MM(20.0), MM(top + 1.15)))
            with self.subTest(top=top, text="variant label"):
                self.assertEqual(label.GetHorizJustify(), pcbnew.GR_TEXT_H_ALIGN_LEFT)

    def assert_layer_mutations_rejected(self, layer, select=lambda item: True):
        for mutation in ("missing", "wrong_layer", "size", "fill", "stroke", "extra"):
            with self.subTest(layer=layer, mutation=mutation):
                def mutate(board):
                    items = [item for item in board.GetDrawings()
                             if item.GetLayer() == layer and select(item)]
                    self.assertTrue(items)
                    item = items[0]
                    if mutation == "missing":
                        for item in items:
                            board.Delete(item)
                    elif mutation == "wrong_layer":
                        for item in items:
                            item.SetLayer(pcbnew.Dwgs_User)
                    elif mutation == "size":
                        end = item.GetEnd()
                        item.SetEnd(pcbnew.VECTOR2I(end.x + MM(0.2), end.y))
                    elif mutation == "fill":
                        item.SetFilled(not item.IsSolidFill())
                    elif mutation == "stroke":
                        item.SetWidth(MM(0.3))
                    else:
                        board.Add(filled_rect(board, layer, 10.0, 8.0, 90.0, 10.0))

                self.assert_rejected(mutate)

    def test_rejects_invalid_mask_windows(self):
        self.assert_layer_mutations_rejected(pcbnew.F_Mask)

    def test_rejects_invalid_edge_cuts(self):
        self.assert_layer_mutations_rejected(pcbnew.Edge_Cuts)

    def test_rejects_invalid_isolation_slots(self):
        self.assert_layer_mutations_rejected(pcbnew.Edge_Cuts, lambda item: item.IsSolidFill())

    def test_rejects_invalid_silk_frames(self):
        self.assert_layer_mutations_rejected(
            pcbnew.F_SilkS,
            lambda item: type(item).__name__ == "PCB_SHAPE"
            and item.GetShape() == pcbnew.SHAPE_T_RECT,
        )

    def test_rejects_extra_silk_text(self):
        def mutate(board):
            item = pcbnew.PCB_TEXT(board)
            item.SetText("UNAUTHORIZED SILK")
            item.SetPosition(pcbnew.VECTOR2I(MM(30.0), MM(10.0)))
            item.SetLayer(pcbnew.F_SilkS)
            board.Add(item)

        self.assert_rejected(mutate)

    def test_rejects_invalid_silk_text_attributes(self):
        for mutation in ("missing", "layer", "content", "position", "size", "thickness",
                         "horizontal", "vertical", "angle", "mirrored", "italic", "bold"):
            with self.subTest(mutation=mutation):
                def mutate(board):
                    item = next(item for item in board.GetDrawings()
                                if type(item).__name__ == "PCB_TEXT" and item.GetText() == "SIG")
                    if mutation == "missing":
                        board.Delete(item)
                    elif mutation == "layer":
                        item.SetLayer(pcbnew.Dwgs_User)
                    elif mutation == "content":
                        item.SetText("GND")
                    elif mutation == "position":
                        pos = item.GetPosition()
                        item.SetPosition(pcbnew.VECTOR2I(pos.x, pos.y + MM(0.1)))
                    elif mutation == "size":
                        item.SetTextSize(pcbnew.VECTOR2I(MM(1.0), MM(0.8)))
                    elif mutation == "thickness":
                        item.SetTextThickness(MM(0.3))
                    elif mutation == "horizontal":
                        item.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT)
                    elif mutation == "vertical":
                        item.SetVertJustify(pcbnew.GR_TEXT_V_ALIGN_TOP)
                    elif mutation == "angle":
                        item.SetTextAngle(pcbnew.EDA_ANGLE(90.0, pcbnew.DEGREES_T))
                    elif mutation == "mirrored":
                        item.SetMirrored(True)
                    elif mutation == "italic":
                        item.SetItalic(True)
                    elif mutation == "bold":
                        item.SetBold(True)

                self.assert_rejected(mutate)

    def test_rejects_centered_variant_label(self):
        def mutate(board):
            item = next(item for item in board.GetDrawings()
                        if type(item).__name__ == "PCB_TEXT" and item.GetText().startswith("S0  "))
            item.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_CENTER)

        self.assert_rejected(mutate)

    def test_rejects_extra_front_copper_bridge(self):
        def mutate(board):
            board.Add(filled_rect(board, pcbnew.F_Cu, 49.0, 10.0, 53.0, 11.0))

        self.assert_rejected(mutate)

    def test_rejects_back_copper_graphic_inside_footprint(self):
        def mutate(board):
            footprint = pcbnew.FOOTPRINT(board)
            footprint.SetReference("MUTANT")
            footprint.Add(filled_rect(footprint, pcbnew.B_Cu, 10.0, 10.0, 12.0, 12.0))
            board.Add(footprint)

        self.assert_rejected(mutate)

    def test_rejects_wrong_track_widths(self):
        def mutate(board):
            for track in board.GetTracks():
                if track.GetLayer() == pcbnew.F_Cu:
                    track.SetWidth(MM(0.1))

        self.assert_rejected(mutate)

    def test_rejects_symmetric_equal_length_endpoint_drift(self):
        def mutate(board):
            dy = MM(0.2)
            for track in board.GetTracks():
                start = track.GetStart()
                end = track.GetEnd()
                start_y = pcbnew.ToMM(start.y)
                end_y = pcbnew.ToMM(end.y)
                if (track.GetLayer() == pcbnew.F_Cu
                        and 29.0 <= start_y <= 45.0
                        and 29.0 <= end_y <= 45.0):
                    track.SetStart(pcbnew.VECTOR2I(start.x, start.y + dy))
                    track.SetEnd(pcbnew.VECTOR2I(end.x, end.y + dy))

        self.assert_rejected(mutate)

    def test_rejects_slanted_cut_lines_with_correct_centers_and_y_ranges(self):
        def mutate(board):
            for item in board.GetDrawings():
                if (item.GetLayer() == pcbnew.F_SilkS
                        and hasattr(item, "GetShape")
                        and item.GetShape() == pcbnew.SHAPE_T_SEGMENT):
                    start = item.GetStart()
                    end = item.GetEnd()
                    item.SetStart(pcbnew.VECTOR2I(start.x - MM(5.0), start.y))
                    item.SetEnd(pcbnew.VECTOR2I(end.x + MM(5.0), end.y))

        self.assert_rejected(mutate)

    def test_rejects_wrong_cut_line_widths(self):
        def mutate(board):
            for item in board.GetDrawings():
                if (item.GetLayer() == pcbnew.F_SilkS
                        and hasattr(item, "GetShape")
                        and item.GetShape() == pcbnew.SHAPE_T_SEGMENT):
                    item.SetWidth(MM(3.0))

        self.assert_rejected(mutate)

    def test_rejects_extra_cut_line_crossing_the_full_panel(self):
        def mutate(board):
            board.Add(segment(board, pcbnew.F_SilkS, 19.0, 0.0, 19.0, 100.0, 0.15))

        self.assert_rejected(mutate)

    def test_rejects_extra_cut_line_entirely_in_piece_corridor(self):
        def mutate(board):
            board.Add(segment(board, pcbnew.F_SilkS, 19.0, 24.0, 19.0, 27.0, 0.15))

        self.assert_rejected(mutate)


if __name__ == "__main__":
    unittest.main(verbosity=2)
