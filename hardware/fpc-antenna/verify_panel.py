#!/usr/bin/env python3
"""Numerically verify the frozen REV-A3.1 ADS-B FPC antenna panel."""

import math
from collections import Counter
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

import wx

_WX_APP = wx.App(False)
wx.Log.SetLogLevel(wx.LOG_Error)

import pcbnew  # noqa: E402  (wx.App must exist before pcbnew is imported)


TOL = 0.01
PIECE_LEFT = 5.0
PIECE_RIGHT = 95.0
PIECE_HEIGHT = 16.0

# Independent acceptance data. Do not import or derive these values from the
# generator: this table is intended to detect generator geometry regressions.
PIECES = (
    {"name": "S0", "top": 6.0, "tw": 2.0, "a": 30.0, "b": 13.0,
     "c": 12.5, "tabs": True, "expected_arm": 70.5,
     "cut_x": (17.5, 19.0, 20.5, 79.5, 81.0, 82.5)},
    {"name": "S-4", "top": 29.0, "tw": 2.0, "a": 28.0, "b": 12.5,
     "c": 10.0, "tabs": False, "expected_arm": 65.5, "cut_x": ()},
    {"name": "D0", "top": 52.0, "tw": 2.5, "a": 30.0, "b": 15.0,
     "c": 15.5, "tabs": True, "expected_arm": 75.5,
     "cut_x": (16.5, 18.0, 19.5, 80.5, 82.0, 83.5)},
    {"name": "D-5", "top": 75.0, "tw": 2.5, "a": 29.0, "b": 13.0,
     "c": 13.0, "tabs": False, "expected_arm": 70.0, "cut_x": ()},
)


def mm(value):
    return pcbnew.ToMM(value)


def point(item, getter):
    value = getter()
    return (mm(value.x), mm(value.y))


def segment_key(item):
    start = point(item, item.GetStart)
    end = point(item, item.GetEnd)
    return tuple(sorted((start, end)))


def segment_error(expected, actual):
    return max(abs(a - b) for ep, ap in zip(expected, actual) for a, b in zip(ep, ap))


def expected_piece_tracks(spec):
    top = spec["top"]
    yt, ym, yb = top + 3.0, top + 8.0, top + 13.0
    a, b, c = spec["a"], spec["b"], spec["c"]
    tab_length = 4.5 if spec["tabs"] else 0.0
    bend_x = 47.0 - a + b
    solid_end_x = bend_x - (c - tab_length)
    left = [
        ((47.0, ym), (47.0, yt)),
        ((47.0, yt), (47.0 - a, yt)),
        ((47.0 - a, yt), (47.0 - a, ym)),
        ((47.0 - a, ym), (bend_x, ym)),
        ((bend_x, ym), (bend_x, yb)),
        ((bend_x, yb), (solid_end_x, yb)),
    ]
    if spec["tabs"]:
        left.append(((solid_end_x, yb), (bend_x - c, yb)))
    right = [tuple((100.0 - x, y) for x, y in endpoints) for endpoints in left]
    return [(tuple(sorted(endpoints)), spec["tw"]) for endpoints in left + right]


def actual_track_spec(item):
    return segment_key(item), mm(item.GetWidth()), type(item).__name__


def match_frozen_tracks(actual_items, expected):
    remaining = list(actual_items)
    missing = []
    for expected_endpoints, expected_width in expected:
        match_index = None
        for index, item in enumerate(remaining):
            endpoints, width, item_type = actual_track_spec(item)
            if (item_type == "PCB_TRACK"
                    and segment_error(expected_endpoints, endpoints) <= TOL
                    and abs(width - expected_width) <= TOL):
                match_index = index
                break
        if match_index is None:
            missing.append((expected_endpoints, expected_width))
        else:
            remaining.pop(match_index)
    return missing, remaining


def expected_front_rectangles():
    rectangles = []
    for spec in PIECES:
        top = spec["top"]
        rectangles.extend((
            tuple(sorted(((42.0, top + 0.5), (50.4, top + 5.5)))),
            tuple(sorted(((51.6, top + 0.5), (58.0, top + 5.5)))),
        ))
    return rectangles


def match_frozen_front_drawings(actual_items, expected_rectangles):
    remaining = list(actual_items)
    missing = []
    for expected in expected_rectangles:
        match_index = None
        for index, item in enumerate(remaining):
            if (type(item).__name__ == "PCB_SHAPE"
                    and item.GetShape() == pcbnew.SHAPE_T_RECT
                    and item.IsSolidFill()
                    and mm(item.GetWidth()) <= TOL
                    and segment_error(expected, segment_key(item)) <= TOL):
                match_index = index
                break
        if match_index is None:
            missing.append(expected)
        else:
            remaining.pop(match_index)
    return missing, remaining


def expected_cut_lines(spec):
    center_y = spec["top"] + 13.0
    return [
        (tuple(sorted(((x, center_y - 1.8), (x, center_y + 1.8)))), 0.15)
        for x in spec["cut_x"]
    ]


def match_frozen_cut_lines(actual_items, expected):
    remaining = list(actual_items)
    missing = []
    for expected_endpoints, expected_width in expected:
        match_index = None
        for index, item in enumerate(remaining):
            if (type(item).__name__ == "PCB_SHAPE"
                    and item.GetShape() == pcbnew.SHAPE_T_SEGMENT
                    and segment_error(expected_endpoints, segment_key(item)) <= TOL
                    and abs(mm(item.GetWidth()) - expected_width) <= TOL):
                match_index = index
                break
        if match_index is None:
            missing.append((expected_endpoints, expected_width))
        else:
            remaining.pop(match_index)
    return missing, remaining


def shape_contract(shape, start, end, width, filled):
    """Exact internal-unit geometry, independent of generator implementation."""
    endpoints = tuple(sorted(tuple(pcbnew.FromMM(v) for v in xy) for xy in (start, end)))
    return ("PCB_SHAPE", shape, endpoints, pcbnew.FromMM(width),
            "yes" if filled else "no", "default")


def actual_shape_contract(item):
    if type(item).__name__ != "PCB_SHAPE":
        return (type(item).__name__,)
    # KiCad 10's LINE_STYLE getter returns an opaque SWIG pointer. Serialize
    # this actual object through KiCad to read its stroke/fill enums safely.
    formatter = pcbnew.PCB_IO_KICAD_SEXPR()
    formatter.Format(item)
    serialized = formatter.GetStringOutput(True)
    stroke = re.search(r"\(stroke\s+\(width\s+[^)]+\)\s+\(type\s+([^)]+)\)", serialized)
    fill = re.search(r"\(fill\s+([^)]+)\)", serialized)
    endpoints = tuple(sorted((tuple(item.GetStart()), tuple(item.GetEnd()))))
    return ("PCB_SHAPE", item.GetShape(), endpoints, item.GetWidth(),
            fill.group(1) if fill else "no", stroke.group(1) if stroke else None)


def expected_layer_shapes(layer):
    rect = pcbnew.SHAPE_T_RECT
    expected = []
    if layer == pcbnew.Edge_Cuts:
        # Outer-frame 0.05 mm stroke is frozen existing presentation.
        expected.append(shape_contract(rect, (0.0, 0.0), (100.0, 100.0), 0.05, False))
    for spec in PIECES:
        top = spec["top"]
        if layer == pcbnew.F_Mask:
            expected.extend((
                shape_contract(rect, (42.5, top + 0.7), (50.0, top + 3.4), 0.0, True),
                shape_contract(rect, (52.0, top + 0.7), (57.5, top + 3.4), 0.0, True),
            ))
        elif layer == pcbnew.Edge_Cuts:
            expected.append(shape_contract(
                rect, (50.4, top), (51.6, top + 3.4), 0.0, True))
        elif layer == pcbnew.F_SilkS:
            expected.extend((
                shape_contract(rect, (5.0, top), (95.0, top + 16.0), 0.15, False),
                # Window-frame 0.12 mm stroke is frozen existing presentation.
                shape_contract(rect, (42.3, top + 0.3), (50.2, top + 3.6), 0.12, False),
                shape_contract(rect, (51.8, top + 0.3), (57.7, top + 3.6), 0.12, False),
            ))
            expected.extend(shape_contract(pcbnew.SHAPE_T_SEGMENT, *ends, width, False)
                            for ends, width in expected_cut_lines(spec))
    return expected


def text_contract(content, x, y, size, thickness, horizontal):
    return ("PCB_TEXT", content, (pcbnew.FromMM(x), pcbnew.FromMM(y)),
            (pcbnew.FromMM(size), pcbnew.FromMM(size)), pcbnew.FromMM(thickness),
            horizontal, pcbnew.GR_TEXT_V_ALIGN_CENTER, 0.0,
            False, False, False, True, None)


def expected_silk_texts():
    center = pcbnew.GR_TEXT_H_ALIGN_CENTER
    left = pcbnew.GR_TEXT_H_ALIGN_LEFT
    expected = []
    for spec in PIECES:
        top = spec["top"]
        expected.extend((
            text_contract("SIG", 46.0, top + 4.5, 0.8, 0.16, center),
            text_contract("GND", 54.5, top + 4.5, 0.8, 0.16, center),
        ))
        label = f"{spec['name']}  w{spec['tw']} ARM={spec['expected_arm']}"
        if spec["tabs"]:
            label += "  3CUTS->-4.5mm"
            # CUT helpers, label font metrics and footer lines are unchanged
            # whitelist data where HANDOFF supplies no replacement values.
            expected.append(text_contract("CUT = FREQ UP", 60.0, top + 13.0, 0.7, 0.14, center))
        expected.append(text_contract(label, 20.0, top + 1.15, 0.7, 0.14, left))
    expected.extend((
        text_contract("ADSB-FPC REV-A3.1F  JLC 2L 0.11mm PI / 1/3oz ED / YEL COVERLAY 25um x2 / ENIG 1uin / NO SHIELD",
                      5.0, 94.6, 1.0, 0.20, left),
        text_contract("SYMMETRIC MEANDERED DIPOLE (SIM-VALIDATED). PIGTAIL RF137 ALONG TOP EDGE: TIP->SIG, BRAID->GND, EPOXY 10MM.",
                      5.0, 96.6, 0.8, 0.16, left),
        text_contract("CUT PIECES APART WITH SCISSORS ALONG SILK OUTLINES. KEEP 10MM CLEAR OF METAL/BATTERY.",
                      5.0, 98.2, 0.8, 0.16, left),
    ))
    return expected


def actual_text_contract(item):
    font = item.GetFont()
    return ("PCB_TEXT", item.GetText(), tuple(item.GetPosition()), tuple(item.GetTextSize()),
            item.GetTextThickness(), item.GetHorizJustify(), item.GetVertJustify(),
            item.GetTextAngle().AsDegrees(), item.IsMirrored(), item.IsItalic(),
            item.IsBold(), item.IsVisible(), font.GetName() if font else None)


def check_frozen_drawing_layers(board, check):
    for layer, name in ((pcbnew.F_Mask, "F.Mask"), (pcbnew.Edge_Cuts, "Edge.Cuts"),
                        (pcbnew.F_SilkS, "F.SilkS")):
        items = [item for item in board.GetDrawings() if item.IsOnLayer(layer)]
        expected = expected_layer_shapes(layer)
        if layer == pcbnew.F_SilkS:
            expected += expected_silk_texts()
        actual = [actual_text_contract(item) if type(item).__name__ == "PCB_TEXT"
                  else actual_shape_contract(item) for item in items]
        missing = Counter(expected) - Counter(actual)
        extra = Counter(actual) - Counter(expected)
        print(f"{name} frozen drawings: actual={len(items)} expected={len(expected)} "
              f"missing={sum(missing.values())} extra={sum(extra.values())}")
        check(not missing and not extra,
              f"{name} drawings: missing={sum(missing.values())} "
              f"extra/non-frozen={sum(extra.values())}")
        for contract in missing:
            check(False, f"{name} missing contract (nm): {contract}")
        for contract in extra:
            check(False, f"{name} extra/non-frozen contract (nm): {contract}")


def piece_tracks(board, top):
    bottom = top + PIECE_HEIGHT
    result = []
    for item in board.GetTracks():
        if item.GetLayer() != pcbnew.F_Cu:
            continue
        y1 = point(item, item.GetStart)[1]
        y2 = point(item, item.GetEnd)[1]
        if top - TOL <= y1 <= bottom + TOL and top - TOL <= y2 <= bottom + TOL:
            result.append(item)
    return result


def track_length(item):
    (x1, y1), (x2, y2) = segment_key(item)
    return math.hypot(x2 - x1, y2 - y1)


def mirror_error(left, right):
    mirrored = []
    for item in left:
        endpoints = tuple(sorted((
            (100.0 - point(item, item.GetStart)[0], point(item, item.GetStart)[1]),
            (100.0 - point(item, item.GetEnd)[0], point(item, item.GetEnd)[1]),
        )))
        mirrored.append(endpoints)
    actual = [segment_key(item) for item in right]
    if len(mirrored) != len(actual):
        return float("inf")
    remaining = list(actual)
    worst = 0.0
    for expected in mirrored:
        best_index, best_error = min(
            enumerate(segment_error(expected, candidate) for candidate in remaining),
            key=lambda pair: pair[1],
        )
        worst = max(worst, best_error)
        remaining.pop(best_index)
    return worst


def piece_copper(board, top):
    bottom = top + PIECE_HEIGHT
    items = list(piece_tracks(board, top))
    for item in board.GetDrawings():
        if item.GetLayer() != pcbnew.F_Cu:
            continue
        box = item.GetBoundingBox()
        cy = mm(box.GetY()) + mm(box.GetHeight()) / 2.0
        if top - TOL <= cy <= bottom + TOL:
            items.append(item)
    return items


def copper_margins(items, top):
    boxes = [item.GetBoundingBox() for item in items]
    min_x = min(mm(box.GetX()) for box in boxes)
    max_x = max(mm(box.GetX()) + mm(box.GetWidth()) for box in boxes)
    min_y = min(mm(box.GetY()) for box in boxes)
    max_y = max(mm(box.GetY()) + mm(box.GetHeight()) for box in boxes)
    return (min_x - PIECE_LEFT, PIECE_RIGHT - max_x, min_y - top,
            top + PIECE_HEIGHT - max_y)


def tongue_gap(board, top):
    rectangles = []
    for item in board.GetDrawings():
        if item.GetLayer() != pcbnew.F_Cu or item.GetShape() != pcbnew.SHAPE_T_RECT:
            continue
        box = item.GetBoundingBox()
        min_y = mm(box.GetY())
        max_y = min_y + mm(box.GetHeight())
        if abs(min_y - (top + 0.5)) < TOL and abs(max_y - (top + 5.5)) < TOL:
            rectangles.append((mm(box.GetX()), mm(box.GetX()) + mm(box.GetWidth())))
    rectangles.sort()
    if len(rectangles) != 2:
        return float("nan"), len(rectangles)
    return rectangles[1][0] - rectangles[0][1], len(rectangles)


def cut_lines(board, top):
    bottom = top + PIECE_HEIGHT
    result = []
    for item in board.GetDrawings():
        if (item.GetLayer() != pcbnew.F_SilkS
                or not hasattr(item, "GetShape")
                or item.GetShape() != pcbnew.SHAPE_T_SEGMENT):
            continue
        start = point(item, item.GetStart)
        end = point(item, item.GetEnd)
        center_y = (start[1] + end[1]) / 2.0
        if top - TOL <= center_y <= bottom + TOL:
            result.append(item)
    return result


def back_copper_counts(board):
    footprints = list(board.GetFootprints())
    return {
        "tracks": sum(item.IsOnLayer(pcbnew.B_Cu) for item in board.GetTracks()),
        "drawings": sum(item.IsOnLayer(pcbnew.B_Cu) for item in board.GetDrawings()),
        "zones": sum(item.IsOnLayer(pcbnew.B_Cu) for item in board.Zones()),
        "pads": sum(pad.IsOnLayer(pcbnew.B_Cu) for fp in footprints for pad in fp.Pads()),
        "footprint_graphics": sum(
            item.IsOnLayer(pcbnew.B_Cu) for fp in footprints for item in fp.GraphicalItems()
        ),
        "footprint_zones": sum(
            zone.IsOnLayer(pcbnew.B_Cu) for fp in footprints for zone in fp.Zones()
        ),
        "footprint_fields": sum(
            field.IsOnLayer(pcbnew.B_Cu) for fp in footprints for field in fp.GetFields()
        ),
    }


def fabrication_failures(path, board):
    """Check source thickness and a fresh KiCad export, never the release job."""
    failures = []

    def check(condition, message):
        if not condition:
            failures.append("fabrication: " + message)

    thickness = mm(board.GetDesignSettings().GetBoardThickness())
    count = board.GetCopperLayerCount()
    print(f"Fabrication PCB: thickness={thickness:.3f} mm copper_layers={count}")
    check(board.GetDesignSettings().GetBoardThickness() == pcbnew.FromMM(0.11),
          f"PCB thickness {thickness} != 0.11 mm")
    check(count == 2, f"PCB copper layers {count} != 2")

    cli = shutil.which("kicad-cli") or str(
        Path.home() / "Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"
    )
    # Preserve isolated CAM evidence; no release files are overwritten/deleted.
    directory = Path(tempfile.mkdtemp(prefix="verify-fpc-job-"))
    try:
        result = subprocess.run(
            [cli, "pcb", "export", "gerbers", str(Path(path).resolve()),
             "--output", str(directory), "--layers", "F.Cu,B.Cu,F.Mask,F.SilkS,Edge.Cuts"],
            capture_output=True, text=True, timeout=60,
        )
        if result.returncode:
            raise ValueError(f"KiCad export exit {result.returncode}: {result.stdout}{result.stderr}")
        jobs = list(directory.glob("*.gbrjob"))
        if len(jobs) != 1:
            raise ValueError(f"expected one fresh .gbrjob, found {len(jobs)} in {directory}")
        job = json.loads(jobs[0].read_text())
        general = job["GeneralSpecs"]
        layers = job["MaterialStackup"]
        copper = [(item["Name"], item["Thickness"]) for item in layers if item["Type"] == "Copper"]
        dielectric = [(item["Material"], item["Thickness"]) for item in layers
                      if item["Type"] == "Dielectric"]
        mask = [(item["Name"], item["Thickness"]) for item in layers if item["Type"] == "SolderMask"]
        total = sum(item.get("Thickness", 0) for item in layers)
        print(f"Fabrication job: {jobs[0]}")
        print("Fabrication job fields: " + json.dumps({
            "BoardThickness": general.get("BoardThickness"),
            "LayerNumber": general.get("LayerNumber"), "Finish": general.get("Finish"),
            "Copper": copper, "Dielectric": dielectric, "Coverlay": mask,
            "StackupThickness": round(total, 6),
        }, sort_keys=True))
        check(general.get("BoardThickness") == 0.11,
              f"job BoardThickness {general.get('BoardThickness')} != 0.11 mm")
        check(general.get("BoardThickness") == thickness, "PCB/job thickness mismatch")
        check(general.get("LayerNumber") == count == 2, "PCB/job copper-layer count mismatch")
        check(general.get("Finish") == "ENIG", f"job Finish {general.get('Finish')} != ENIG")
        check(copper == [("F.Cu", 0.012), ("B.Cu", 0.012)], f"job copper {copper} != two 0.012 mm layers")
        # Round-5 ruling retains 25 um/face coverlay from the original footer.
        # 0.036 is only the remaining flex dielectric balance, not a vendor
        # specification of separate core/adhesive sublayers.
        check(dielectric == [("Polyimide", 0.036)],
              f"job dielectric {dielectric} != Polyimide 0.036 mm aggregate")
        check(mask == [("Top Solder Mask", 0.025), ("Bottom Solder Mask", 0.025)],
              f"job coverlay {mask} != two 0.025 mm layers")
        check(math.isclose(total, 0.11, abs_tol=0.000001),
              f"job stackup total {total} != 0.11 mm")
        check("FR4" not in json.dumps(job).upper(), "job contains default FR4 material")
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        failures.append(f"fabrication: cannot verify fresh job in {directory}: {error}")
    return failures


def verify(path):
    board = pcbnew.LoadBoard(path)
    failures = fabrication_failures(path, board)

    def check(condition, message):
        if not condition:
            failures.append(message)

    all_front_tracks = [item for item in board.GetTracks() if item.IsOnLayer(pcbnew.F_Cu)]
    expected_track_total = sum(len(expected_piece_tracks(spec)) for spec in PIECES)
    print(f"F.Cu frozen tracks: actual={len(all_front_tracks)} expected={expected_track_total}")
    check(len(all_front_tracks) == expected_track_total,
          f"F.Cu: found {len(all_front_tracks)} tracks/vias, expected {expected_track_total} tracks")

    front_drawings = [item for item in board.GetDrawings() if item.IsOnLayer(pcbnew.F_Cu)]
    expected_rectangles = expected_front_rectangles()
    missing_rectangles, extra_drawings = match_frozen_front_drawings(front_drawings, expected_rectangles)
    print(f"F.Cu frozen drawings: actual={len(front_drawings)} expected={len(expected_rectangles)} "
          f"missing={len(missing_rectangles)} extra={len(extra_drawings)}")
    check(not missing_rectangles and not extra_drawings,
          f"F.Cu drawings: missing={len(missing_rectangles)} extra/non-frozen={len(extra_drawings)}")

    all_silk_segments = [
        item for item in board.GetDrawings()
        if (item.IsOnLayer(pcbnew.F_SilkS)
            and hasattr(item, "GetShape")
            and item.GetShape() == pcbnew.SHAPE_T_SEGMENT)
    ]
    expected_silk_segments = [
        expected
        for spec in PIECES
        for expected in expected_cut_lines(spec)
    ]
    missing_silk_segments, extra_silk_segments = match_frozen_cut_lines(
        all_silk_segments, expected_silk_segments
    )
    print(f"F.SilkS frozen segments: actual={len(all_silk_segments)} "
          f"expected={len(expected_silk_segments)} missing={len(missing_silk_segments)} "
          f"extra={len(extra_silk_segments)} width=0.150 mm")
    check(not missing_silk_segments and not extra_silk_segments,
          f"F.SilkS segments: missing={len(missing_silk_segments)} "
          f"extra/non-frozen={len(extra_silk_segments)}")

    check_frozen_drawing_layers(board, check)

    footprints = list(board.GetFootprints())
    zones = list(board.Zones())
    print(f"Frozen panel containers: footprints={len(footprints)} zones={len(zones)}")
    check(not footprints, f"panel: found {len(footprints)} footprints, expected 0")
    check(not zones, f"panel: found {len(zones)} zones, expected 0")

    for spec in PIECES:
        name = spec["name"]
        top = spec["top"]
        tracks = piece_tracks(board, top)
        left = [item for item in tracks if sum(p[0] for p in segment_key(item)) / 2.0 < 50.0]
        right = [item for item in tracks if sum(p[0] for p in segment_key(item)) / 2.0 > 50.0]
        left_length = sum(track_length(item) for item in left)
        right_length = sum(track_length(item) for item in right)
        symmetry = mirror_error(left, right)
        margins = copper_margins(piece_copper(board, top), top)
        gap, tongue_count = tongue_gap(board, top)
        expected_tracks = expected_piece_tracks(spec)
        missing_tracks, extra_tracks = match_frozen_tracks(tracks, expected_tracks)

        print(f"{name} arm: left={left_length:.3f} mm right={right_length:.3f} mm "
              f"expected={spec['expected_arm']:.3f} mm")
        print(f"{name} frozen track geometry: actual={len(tracks)} expected={len(expected_tracks)} "
              f"missing={len(missing_tracks)} extra={len(extra_tracks)} width={spec['tw']:.3f} mm")
        print(f"{name} mirror error: {symmetry:.3f} mm")
        print(f"{name} copper margins L/R/T/B: "
              f"{margins[0]:.3f}/{margins[1]:.3f}/{margins[2]:.3f}/{margins[3]:.3f} mm")
        print(f"{name} tongue gap: {gap:.3f} mm (rectangles={tongue_count})")

        check(abs(left_length - spec["expected_arm"]) <= TOL,
              f"{name}: left arm {left_length:.3f} != {spec['expected_arm']:.3f} mm")
        check(abs(right_length - spec["expected_arm"]) <= TOL,
              f"{name}: right arm {right_length:.3f} != {spec['expected_arm']:.3f} mm")
        check(not missing_tracks and not extra_tracks,
              f"{name}: frozen track geometry missing={len(missing_tracks)} extra={len(extra_tracks)}")
        check(symmetry < TOL, f"{name}: mirror error {symmetry:.3f} mm is not < {TOL:.2f} mm")
        check(min(margins) >= 0.5 - TOL,
              f"{name}: copper edge margin {min(margins):.3f} mm is below 0.5 mm")
        check(tongue_count == 2, f"{name}: found {tongue_count} F.Cu tongue rectangles, expected 2")
        check(abs(gap - 1.2) <= TOL, f"{name}: tongue gap {gap:.3f} != 1.200 mm")

        lines = cut_lines(board, top)
        line_endpoints = [(point(item, item.GetStart), point(item, item.GetEnd)) for item in lines]
        actual_x = sorted((start[0] + end[0]) / 2.0 for start, end in line_endpoints)
        actual_centers = sorted((start[1] + end[1]) / 2.0 for start, end in line_endpoints)
        actual_y_ranges = sorted(
            (min(start[1], end[1]), max(start[1], end[1])) for start, end in line_endpoints
        )
        expected_x = list(spec["cut_x"])
        expected_center = top + 13.0
        expected_range = (expected_center - 1.8, expected_center + 1.8)
        frozen_cut_lines = expected_cut_lines(spec)
        missing_cut_lines, extra_cut_lines = match_frozen_cut_lines(lines, frozen_cut_lines)
        print(f"{name} cut lines: x={actual_x} y_centers={actual_centers} y_ranges={actual_y_ranges}")
        print(f"{name} frozen cut geometry: actual={len(lines)} expected={len(frozen_cut_lines)} "
              f"missing={len(missing_cut_lines)} extra={len(extra_cut_lines)} width=0.150 mm")
        check(len(actual_x) == len(expected_x),
              f"{name}: found {len(actual_x)} cut lines, expected {len(expected_x)}")
        check(not missing_cut_lines and not extra_cut_lines,
              f"{name}: frozen cut geometry missing={len(missing_cut_lines)} "
              f"extra={len(extra_cut_lines)}")
        if len(actual_x) == len(expected_x):
            check(all(abs(actual - expected) <= TOL for actual, expected in zip(actual_x, expected_x)),
                  f"{name}: cut x {actual_x} != {expected_x}")
        if spec["tabs"]:
            check(all(abs(center - expected_center) <= TOL for center in actual_centers),
                  f"{name}: cut y centers {actual_centers} != {expected_center:.3f} mm")
            check(all(abs(lo - expected_range[0]) <= TOL and abs(hi - expected_range[1]) <= TOL
                      for lo, hi in actual_y_ranges),
                  f"{name}: cut y ranges {actual_y_ranges} != {expected_range}")

    back_counts = back_copper_counts(board)
    back_total = sum(back_counts.values())
    print("B.Cu items: " + " ".join(f"{name}={count}" for name, count in back_counts.items())
          + f" total={back_total}")
    check(back_total == 0, f"B.Cu: found {back_total} copper items")

    if failures:
        print("FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("PASS: all panel geometry and fabrication checks passed")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} PCB_PATH", file=sys.stderr)
        raise SystemExit(2)
    raise SystemExit(verify(sys.argv[1]))
