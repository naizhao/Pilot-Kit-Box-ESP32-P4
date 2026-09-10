#!/usr/bin/env python3
"""Independently verify a connected-candidate against a frozen baseline."""

import argparse
from collections import Counter
from pathlib import Path
import re
import sys

import verify_panel


pcbnew = verify_panel.pcbnew
MM = pcbnew.FromMM


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
    """Serialize every shape-specific field while ignoring only its UUID."""
    formatter = pcbnew.PCB_IO_KICAD_SEXPR()
    formatter.Format(item)
    source = formatter.GetStringOutput(True)
    return re.sub(r'\(uuid "[^"]+"\)', "", source)


def _board_signatures(board):
    items = list(board.GetTracks()) + list(board.GetDrawings())
    items += list(board.GetFootprints()) + list(board.Zones())
    return Counter(_signature(item) for item in items)


def _track_signature(start, end, width):
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
    # These literals are deliberately independent from gen_connected_candidate.
    tracks = (
        ((97.0, 5.6), (97.0, 92.975), 0.20),
        ((57.8, 5.6), (97.0, 5.6), 0.20),
        ((57.8, 28.6), (97.0, 28.6), 0.20),
        ((57.8, 51.6), (97.0, 51.6), 0.20),
        ((57.8, 74.6), (97.0, 74.6), 0.20),
        ((57.8, 5.6), (57.8, 6.7), 0.20),
        ((57.8, 28.6), (57.8, 29.7), 0.20),
        ((57.8, 51.6), (57.8, 52.7), 0.20),
        ((57.8, 74.6), (57.8, 75.7), 0.20),
    )
    result = Counter(_track_signature(start, end, width)
                     for start, end, width in tracks)
    resistor_copper = (
        (90.1, 5.0, 91.1, 6.2), (91.9, 5.0, 92.9, 6.2),
        (90.1, 28.0, 91.1, 29.2), (91.9, 28.0, 92.9, 29.2),
        (90.1, 51.0, 91.1, 52.2), (91.9, 51.0, 92.9, 52.2),
        (90.1, 74.0, 91.1, 75.2), (91.9, 74.0, 92.9, 75.2),
    )
    ipex_copper = (
        (94.95, 91.0, 96.0, 92.0),
        (95.9, 89.5, 98.1, 90.55),
        (95.9, 92.45, 98.1, 93.5),
    )
    for x1, y1, x2, y2 in resistor_copper + ipex_copper:
        result[_shape_signature(
            pcbnew.F_Cu, pcbnew.SHAPE_T_RECT,
            (x1, y1), (x2, y2), 0.0, True
        )] += 1
    for start, end in (
        ((57.4, 6.1), (58.05, 6.8)),
        ((57.4, 29.1), (58.05, 29.8)),
        ((57.4, 52.1), (58.05, 52.8)),
        ((57.4, 75.1), (58.05, 75.8)),
    ):
        result[_shape_signature(
            pcbnew.F_Mask, pcbnew.SHAPE_T_RECT, start, end, 0.0, True
        )] += 1
    for x1, y1, x2, y2 in resistor_copper + ipex_copper:
        result[_shape_signature(
            pcbnew.F_Mask, pcbnew.SHAPE_T_RECT,
            (x1 - 0.05, y1 - 0.05), (x2 + 0.05, y2 + 0.05), 0.0, True
        )] += 1
    for index, top in enumerate((6.0, 29.0, 52.0, 75.0), start=1):
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


def _component_labels(items):
    shapes = [item.GetEffectiveShape() for item in items]
    parents = list(range(len(items)))

    def find(node):
        while parents[node] != node:
            parents[node] = parents[parents[node]]
            node = parents[node]
        return node

    def union(left, right):
        left_root, right_root = find(left), find(right)
        if left_root != right_root:
            parents[right_root] = left_root

    for left in range(len(items)):
        for right in range(left + 1, len(items)):
            if shapes[left].Collide(shapes[right]):
                union(left, right)
    return [find(index) for index in range(len(items))]


def _find_rect(items, x1, y1, x2, y2):
    expected = tuple(sorted(((MM(x1), MM(y1)), (MM(x2), MM(y2)))))
    for index, item in enumerate(items):
        if (type(item).__name__ == "PCB_SHAPE"
                and item.GetShape() == pcbnew.SHAPE_T_RECT
                and item.IsSolidFill()
                and _points(item) == expected):
            return index
    return None


def _back_copper_count(board):
    footprints = list(board.GetFootprints())
    return sum((
        sum(item.IsOnLayer(pcbnew.B_Cu) for item in board.GetTracks()),
        sum(item.IsOnLayer(pcbnew.B_Cu) for item in board.GetDrawings()),
        sum(item.IsOnLayer(pcbnew.B_Cu) for item in board.Zones()),
        sum(pad.IsOnLayer(pcbnew.B_Cu) for fp in footprints for pad in fp.Pads()),
        sum(item.IsOnLayer(pcbnew.B_Cu) for fp in footprints for item in fp.GraphicalItems()),
        sum(zone.IsOnLayer(pcbnew.B_Cu) for fp in footprints for zone in fp.Zones()),
        sum(field.IsOnLayer(pcbnew.B_Cu) for fp in footprints for field in fp.GetFields()),
    ))


def collect_failures(candidate_path, baseline_path):
    """Return every detected structural failure without mutating either PCB."""
    candidate = pcbnew.LoadBoard(str(Path(candidate_path)))
    baseline = pcbnew.LoadBoard(str(Path(baseline_path)))
    failures = verify_panel.fabrication_failures(candidate_path, candidate)

    actual = _board_signatures(candidate)
    expected_additions = _expected_additions()
    expected = _board_signatures(baseline) + expected_additions
    missing = expected - actual
    extra = actual - expected
    print("All-layer primitive diff: "
          f"baseline={sum(_board_signatures(baseline).values())} "
          f"added={sum(expected_additions.values())} "
          f"missing={sum(missing.values())} extra={sum(extra.values())}")
    if missing:
        failures.append(f"all-layer diff has {sum(missing.values())} missing primitives")
    if extra:
        failures.append(f"all-layer diff has {sum(extra.values())} extra primitives")

    added_track_count = sum(
        count for signature, count in expected_additions.items() if signature[0] == "track"
    )
    added_mask_count = sum(
        count for signature, count in expected_additions.items()
        if signature[0] == "shape" and signature[1] == pcbnew.F_Mask
    )
    print(f"Declared additions: F.Cu tracks={added_track_count} F.Mask windows={added_mask_count}")

    outlines = [
        item for item in candidate.GetDrawings()
        if (item.IsOnLayer(pcbnew.Edge_Cuts)
            and type(item).__name__ == "PCB_SHAPE"
            and item.GetShape() == pcbnew.SHAPE_T_RECT
            and not item.IsSolidFill()
            and _points(item) == ((MM(0.0), MM(0.0)), (MM(100.0), MM(100.0)))
            and item.GetWidth() == MM(0.05))
    ]
    print(f"100 x 100 mm Edge.Cuts outlines: {len(outlines)}")
    if len(outlines) != 1:
        failures.append(f"expected one 100 x 100 mm Edge.Cuts outline, found {len(outlines)}")

    back_count = _back_copper_count(candidate)
    print(f"B.Cu objects: {back_count}")
    if back_count:
        failures.append(f"B.Cu has {back_count} objects")

    copper = [
        item for item in list(candidate.GetTracks()) + list(candidate.GetDrawings())
        if item.IsOnLayer(pcbnew.F_Cu)
    ]
    labels = _component_labels(copper)
    gnd = [_find_rect(copper, 51.6, top + 0.5, 58.0, top + 5.5)
           for top in (6.0, 29.0, 52.0, 75.0)]
    sig = [_find_rect(copper, 42.0, top + 0.5, 50.4, top + 5.5)
           for top in (6.0, 29.0, 52.0, 75.0)]
    if any(index is None for index in gnd + sig):
        failures.append("could not identify all four GND and SIG tongue rectangles")
        print("Geometric connectivity: tongue lookup failed")
    else:
        gnd_labels = [labels[index] for index in gnd]
        sig_labels = [labels[index] for index in sig]
        common_gnd = len(set(gnd_labels)) == 1
        sig_isolated = (len(set(sig_labels)) == 4
                        and all(label != gnd_labels[0] for label in sig_labels))
        print(f"Geometric connectivity: common_GND={common_gnd} isolated_SIG={sig_isolated}")
        if not common_gnd:
            failures.append("four GND tongues are not in one geometric copper component")
        if not sig_isolated:
            failures.append("SIG tongues are shorted together or to the common GND component")

    return failures


def verify(candidate_path, baseline_path):
    failures = collect_failures(candidate_path, baseline_path)
    if failures:
        print("FAIL")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("PASS: connected candidate matches the frozen baseline and connectivity contract")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("candidate_path")
    parser.add_argument("baseline_path")
    args = parser.parse_args(argv)
    try:
        return verify(args.candidate_path, args.baseline_path)
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
