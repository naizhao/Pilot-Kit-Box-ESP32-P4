#!/usr/bin/env python3
"""Generate ADS-B FPC antenna tuning panel REV-A3.1 via KiCad pcbnew API.

RESTORED after REV-B was withdrawn (REV-B changed topology without simulation
and had tightly-coupled anti-phase parallel sections).

REV-A3.1 = symmetric 3-run meandered dipole, center-fed — the exact topology
that was simulated with openEMS (resonances 1038-1068 MHz, S11@1090
-3.4..-5.1 dB for cut variants). Solder tongue for the RF137 pigtail sits at
each piece TOP edge: tip -> SIG window, braid -> GND window, cable folds back
over the edge and is epoxied (standard FPC antenna mounting).

Run with KiCad's bundled python:
  ~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 gen_panel.py out.kicad_pcb
"""
import sys
from pathlib import Path

import wx

_WX_APP = wx.App(False)
wx.Log.SetLogLevel(wx.LOG_Error)

import pcbnew  # noqa: E402  (wx.App must exist before pcbnew is imported)

from antenna_geometry import PANEL_WIDTH, PIECE_HEIGHT, all_geometries

W = PANEL_WIDTH
PIECE_H = PIECE_HEIGHT

MM = pcbnew.FromMM


def track(board, x1, y1, x2, y2, w, layer=pcbnew.F_Cu):
    t = pcbnew.PCB_TRACK(board)
    t.SetStart(pcbnew.VECTOR2I(MM(x1), MM(y1)))
    t.SetEnd(pcbnew.VECTOR2I(MM(x2), MM(y2)))
    t.SetWidth(MM(w))
    t.SetLayer(layer)
    t.SetNetCode(0)
    board.Add(t)


def shape_rect(board, x1, y1, x2, y2, layer, filled=True, stroke=0.0):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_RECT)
    s.SetStart(pcbnew.VECTOR2I(MM(x1), MM(y1)))
    s.SetEnd(pcbnew.VECTOR2I(MM(x2), MM(y2)))
    if filled:
        try:
            s.SetFilled(True)
        except Exception:
            s.SetFillMode(pcbnew.FILLED_SHAPE)
        s.SetWidth(0)
    else:
        try:
            s.SetFilled(False)
        except Exception:
            s.SetFillMode(pcbnew.NOT_FILLED_SHAPE)
        s.SetWidth(MM(stroke))
    s.SetLayer(layer)
    board.Add(s)


def text(board, x, y, s, layer=pcbnew.F_SilkS, size=0.8, align="center"):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(s)
    t.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
    t.SetLayer(layer)
    t.SetTextSize(pcbnew.VECTOR2I(MM(size), MM(size)))
    t.SetTextThickness(MM(max(0.12, size / 5.0)))
    just = {"left": pcbnew.GR_TEXT_H_ALIGN_LEFT,
            "right": pcbnew.GR_TEXT_H_ALIGN_RIGHT,
            "center": pcbnew.GR_TEXT_H_ALIGN_CENTER}.get(align)
    if just is not None:
        t.SetHorizJustify(just)
    board.Add(t)


def main(out_path):
    board = pcbnew.CreateEmptyBoard()
    board.SetCopperLayerCount(2)
    board.GetDesignSettings().SetBoardThickness(MM(0.11))
    shape_rect(board, 0, 0, W, W, pcbnew.Edge_Cuts, filled=False, stroke=0.05)

    for geometry in all_geometries():
        spec = geometry.spec
        name, top, tw = spec.name, spec.top, spec.trace_width
        for (x1, y1), (x2, y2) in geometry.left_arm + geometry.right_arm:
            track(board, x1, y1, x2, y2, tw)

        # ---- solder tongue at piece top edge (in-line with cable axis) ----
        for rectangle, layer in (
                (geometry.sig_tongue, pcbnew.F_Cu),
                (geometry.gnd_tongue, pcbnew.F_Cu),
                (geometry.sig_mask, pcbnew.F_Mask),
                (geometry.gnd_mask, pcbnew.F_Mask),
                (geometry.isolation_slot, pcbnew.Edge_Cuts)):
            (x1, y1), (x2, y2) = rectangle
            shape_rect(board, x1, y1, x2, y2, layer)

        # ---- silk ----
        shape_rect(board, 5.0, top, 95.0, top + PIECE_H, pcbnew.F_SilkS, filled=False, stroke=0.15)
        shape_rect(board, 42.3, top + 0.3, 50.2, top + 3.6, pcbnew.F_SilkS, filled=False, stroke=0.12)
        shape_rect(board, 51.8, top + 0.3, 57.7, top + 3.6, pcbnew.F_SilkS, filled=False, stroke=0.12)
        text(board, 46.0, top + 4.5, "SIG", size=0.8)
        text(board, 54.5, top + 4.5, "GND", size=0.8)
        if spec.has_trim_tabs:
            for (x1, y1), (x2, y2) in geometry.cut_lines:
                sh = pcbnew.PCB_SHAPE(board)
                sh.SetShape(pcbnew.SHAPE_T_SEGMENT)
                sh.SetStart(pcbnew.VECTOR2I(MM(x1), MM(y1)))
                sh.SetEnd(pcbnew.VECTOR2I(MM(x2), MM(y2)))
                sh.SetWidth(MM(0.15))
                sh.SetLayer(pcbnew.F_SilkS)
                board.Add(sh)
            text(board, 60.0, top + 13.0, "CUT = FREQ UP", size=0.7)
        label = f"{name}  w{tw} ARM={spec.arm_length}"
        if spec.has_trim_tabs:
            label += f"  3CUTS->-{spec.trim_length}mm"
        text(board, 20.0, top + 1.15, label, size=0.7, align="left")

    text(board, 5.0, 94.6, "ADSB-FPC REV-A3.1F  JLC 2L 0.11mm PI / 1/3oz ED / YEL COVERLAY 25um x2 / ENIG 1uin / NO SHIELD",
         pcbnew.F_SilkS, size=1.0, align="left")
    text(board, 5.0, 96.6, "SYMMETRIC MEANDERED DIPOLE (SIM-VALIDATED). PIGTAIL RF137 ALONG TOP EDGE: TIP->SIG, BRAID->GND, EPOXY 10MM.",
         pcbnew.F_SilkS, size=0.8, align="left")
    text(board, 5.0, 98.2, "CUT PIECES APART WITH SCISSORS ALONG SILK OUTLINES. KEEP 10MM CLEAR OF METAL/BATTERY.",
         pcbnew.F_SilkS, size=0.8, align="left")

    if not pcbnew.SaveBoard(out_path, board):
        raise RuntimeError(f"KiCad could not save {out_path}")
    # KiCad 10 SWIG does not expose a writable BOARD_STACKUP API. Insert only
    # into this freshly saved board, using KiCad's documented stackup syntax:
    # https://dev-docs.kicad.org/en/file-formats/sexpr-pcb/#stack-up-settings
    # The 25 um coverlay on each face follows the round-5 handoff ruling based
    # on the pre-existing panel footer, not a measured supplier stackup.
    # 0.036 mm is the unpartitioned PI/flex dielectric system balance:
    # 0.11 total - 2 * 0.025 coverlay - 2 * 0.012 copper. It does not specify
    # an internal adhesive/core split. KiCad requires all enabled technical
    # layers to keep the stackup synchronized and export thicknesses.
    # No dielectric electrical data is invented.
    path = Path(out_path)
    source = path.read_text()
    anchor = "\n\t(setup\n"
    if source.count(anchor) != 1 or "(stackup" in source:
        raise RuntimeError("Expected one fresh KiCad setup without an existing stackup")
    if source.count("(thickness 0.11)") != 1:
        raise RuntimeError("Fresh KiCad board must declare exactly 0.11 mm thickness")
    stackup = '''		(stackup
			(layer "F.SilkS" (type "Top Silk Screen"))
			(layer "F.Paste" (type "Top Solder Paste"))
			(layer "F.Mask" (type "Top Solder Mask") (thickness 0.025))
			(layer "F.Cu" (type "copper") (thickness 0.012))
			(layer "dielectric 1" (type "Unpartitioned flex dielectric system (metadata balance)") (thickness 0.036) (material "Polyimide"))
			(layer "B.Cu" (type "copper") (thickness 0.012))
			(layer "B.Mask" (type "Bottom Solder Mask") (thickness 0.025))
			(layer "B.Paste" (type "Bottom Solder Paste"))
			(layer "B.SilkS" (type "Bottom Silk Screen"))
			(copper_finish "ENIG")
		)
'''
    path.write_text(source.replace(anchor, anchor + stackup, 1))

    # Reload through KiCad, then prove the manufacturing metadata survives a
    # real kicad-cli Gerber job export before reporting successful generation.
    from verify_panel import fabrication_failures
    reloaded = pcbnew.LoadBoard(str(path))
    failures = fabrication_failures(path, reloaded)
    if failures:
        raise RuntimeError("Generated FPC manufacturing metadata failed: " + "; ".join(failures))
    print("wrote", out_path)


if __name__ == "__main__":
    main(sys.argv[1])
