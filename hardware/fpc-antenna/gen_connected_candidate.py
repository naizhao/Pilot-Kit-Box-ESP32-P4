#!/usr/bin/env python3
"""Generate the connected FPC tuning candidate with 0804/IPEX hand pads."""

import argparse
from pathlib import Path
import sys

import gen_panel
from antenna_geometry import all_geometries


TOPS = (6.0, 29.0, 52.0, 75.0)
TRACKS = (
    ((97.0, 5.6), (97.0, 92.975), 0.20),
    *((((57.8, top - 0.4), (97.0, top - 0.4), 0.20)) for top in TOPS),
    *((((57.8, top - 0.4), (57.8, top + 0.7), 0.20)) for top in TOPS),
)
RESISTOR_COPPER = tuple(
    (cx - 0.5, top - 1.0, cx + 0.5, top + 0.2)
    for top in TOPS for cx in (90.6, 92.4)
)
IPEX_COPPER = (
    (94.95, 91.0, 96.0, 92.0),
    (95.9, 89.5, 98.1, 90.55),
    (95.9, 92.45, 98.1, 93.5),
)


def _silks(board):
    for index, top in enumerate(TOPS, start=1):
        segment = gen_panel.pcbnew.PCB_SHAPE(board)
        segment.SetShape(gen_panel.pcbnew.SHAPE_T_SEGMENT)
        segment.SetStart(gen_panel.pcbnew.VECTOR2I(
            gen_panel.MM(57.2), gen_panel.MM(top + 0.35)))
        segment.SetEnd(gen_panel.pcbnew.VECTOR2I(
            gen_panel.MM(58.4), gen_panel.MM(top + 0.35)))
        segment.SetWidth(gen_panel.MM(0.15))
        segment.SetLayer(gen_panel.pcbnew.F_SilkS)
        board.Add(segment)
        gen_panel.text(board, 59.0, top + 0.35, "CUT LINK", size=0.6,
                       align="left")
        gen_panel.text(board, 91.5, top + 0.75,
                       f"R{index} 0R 0804", size=0.45)
    gen_panel.shape_rect(board, 94.4, 89.2, 99.6, 93.8,
                         gen_panel.pcbnew.F_SilkS, filled=False, stroke=0.15)
    gen_panel.text(board, 97.0, 88.65, "IPEX GND", size=0.55)


def generate_candidate(output_path):
    output = Path(output_path)
    baseline = Path(__file__).with_name("adsb-fpc-antenna.kicad_pcb")
    if output.resolve() == baseline.resolve():
        raise ValueError("candidate output must not overwrite the frozen baseline")
    board = gen_panel.pcbnew.CreateEmptyBoard()
    board.SetCopperLayerCount(2)
    board.GetDesignSettings().SetBoardThickness(gen_panel.MM(0.11))
    gen_panel.shape_rect(board, 0, 0, gen_panel.W, gen_panel.W,
                         gen_panel.pcbnew.Edge_Cuts, filled=False, stroke=0.05)

    for geometry in all_geometries():
        spec = geometry.spec
        width = spec.trace_width
        for (x1, y1), (x2, y2) in geometry.left_arm + geometry.right_arm:
            gen_panel.track(board, x1, y1, x2, y2, width)
        for rectangle, layer in (
                (geometry.sig_tongue, gen_panel.pcbnew.F_Cu),
                (geometry.gnd_tongue, gen_panel.pcbnew.F_Cu),
                (geometry.sig_mask, gen_panel.pcbnew.F_Mask),
                (geometry.gnd_mask, gen_panel.pcbnew.F_Mask),
                (geometry.isolation_slot, gen_panel.pcbnew.Edge_Cuts)):
            (x1, y1), (x2, y2) = rectangle
            gen_panel.shape_rect(board, x1, y1, x2, y2, layer)
        top = spec.top
        gen_panel.shape_rect(board, 5.0, top, 95.0, top + gen_panel.PIECE_H,
                             gen_panel.pcbnew.F_SilkS, filled=False, stroke=0.15)
        gen_panel.shape_rect(board, 42.3, top + 0.3, 50.2, top + 3.6,
                             gen_panel.pcbnew.F_SilkS, filled=False, stroke=0.12)
        gen_panel.shape_rect(board, 51.8, top + 0.3, 57.7, top + 3.6,
                             gen_panel.pcbnew.F_SilkS, filled=False, stroke=0.12)
        gen_panel.text(board, 46.0, top + 4.5, "SIG", size=0.8)
        gen_panel.text(board, 54.5, top + 4.5, "GND", size=0.8)
        if spec.has_trim_tabs:
            for (x1, y1), (x2, y2) in geometry.cut_lines:
                cut = gen_panel.pcbnew.PCB_SHAPE(board)
                cut.SetShape(gen_panel.pcbnew.SHAPE_T_SEGMENT)
                cut.SetStart(gen_panel.pcbnew.VECTOR2I(
                    gen_panel.MM(x1), gen_panel.MM(y1)))
                cut.SetEnd(gen_panel.pcbnew.VECTOR2I(
                    gen_panel.MM(x2), gen_panel.MM(y2)))
                cut.SetWidth(gen_panel.MM(0.15))
                cut.SetLayer(gen_panel.pcbnew.F_SilkS)
                board.Add(cut)
            gen_panel.text(board, 60.0, top + 13.0,
                           "CUT = FREQ UP", size=0.7)
        label = f"{spec.name}  w{width} ARM={spec.arm_length}"
        if spec.has_trim_tabs:
            label += f"  3CUTS->-{spec.trim_length}mm"
        gen_panel.text(board, 20.0, top + 1.15, label, size=0.7, align="left")

    gen_panel.text(board, 5.0, 94.6,
                   "ADSB-FPC REV-A3.1F  JLC 2L 0.11mm PI / 1/3oz ED / YEL COVERLAY 25um x2 / ENIG 1uin / NO SHIELD",
                   gen_panel.pcbnew.F_SilkS, size=1.0, align="left")
    gen_panel.text(board, 5.0, 96.6,
                   "SYMMETRIC MEANDERED DIPOLE (SIM-VALIDATED). PIGTAIL RF137 ALONG TOP EDGE: TIP->SIG, BRAID->GND, EPOXY 10MM.",
                   gen_panel.pcbnew.F_SilkS, size=0.8, align="left")
    gen_panel.text(board, 5.0, 98.2,
                   "CUT PIECES APART WITH SCISSORS ALONG SILK OUTLINES. KEEP 10MM CLEAR OF METAL/BATTERY.",
                   gen_panel.pcbnew.F_SilkS, size=0.8, align="left")

    for (x1, y1), (x2, y2), width in TRACKS:
        gen_panel.track(board, x1, y1, x2, y2, width)
    for x1, y1, x2, y2 in RESISTOR_COPPER + IPEX_COPPER:
        gen_panel.shape_rect(board, x1, y1, x2, y2,
                             gen_panel.pcbnew.F_Cu, filled=True)
        gen_panel.shape_rect(board, x1 - 0.05, y1 - 0.05, x2 + 0.05, y2 + 0.05,
                             gen_panel.pcbnew.F_Mask, filled=True)
    for top in TOPS:
        gen_panel.shape_rect(board, 57.4, top + 0.1, 58.05, top + 0.8,
                             gen_panel.pcbnew.F_Mask, filled=True)
    _silks(board)

    if not gen_panel.pcbnew.SaveBoard(str(output), board):
        raise RuntimeError(f"KiCad failed to save {output}")
    source = output.read_text()
    anchor = "\n\t(setup\n"
    if source.count(anchor) != 1 or "(stackup" in source:
        raise RuntimeError("Expected one fresh KiCad setup without an existing stackup")
    stackup = '''\t\t(stackup
\t\t\t(layer "F.SilkS" (type "Top Silk Screen"))
\t\t\t(layer "F.Paste" (type "Top Solder Paste"))
\t\t\t(layer "F.Mask" (type "Top Solder Mask") (thickness 0.025))
\t\t\t(layer "F.Cu" (type "copper") (thickness 0.012))
\t\t\t(layer "dielectric 1" (type "Unpartitioned flex dielectric system (metadata balance)") (thickness 0.036) (material "Polyimide"))
\t\t\t(layer "B.Cu" (type "copper") (thickness 0.012))
\t\t\t(layer "B.Mask" (type "Bottom Solder Mask") (thickness 0.025))
\t\t\t(layer "B.Paste" (type "Bottom Solder Paste"))
\t\t\t(layer "B.SilkS" (type "Bottom Silk Screen"))
\t\t\t(copper_finish "ENIG")
\t\t)
'''
    output.write_text(source.replace(anchor, anchor + stackup, 1))
    return output


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_path")
    args = parser.parse_args(argv)
    try:
        output = generate_candidate(args.output_path)
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(f"wrote connected candidate {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
