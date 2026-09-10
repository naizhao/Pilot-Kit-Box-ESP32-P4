#!/usr/bin/env python3
"""Frozen REV-A3.1-fixed antenna geometry shared by PCB and EM models.

All values are millimetres in the 100 x 100 mm panel coordinate system from
HANDOFF_PROMPT.md section 2.  This module intentionally has no KiCad or solver
dependencies so the geometry contract remains directly testable.
"""

from dataclasses import dataclass
from functools import lru_cache
from typing import Tuple


Point = Tuple[float, float]
Segment = Tuple[Point, Point]
Rectangle = Tuple[Point, Point]

PANEL_WIDTH = 100.0
PANEL_HEIGHT = 100.0
PIECE_LEFT = 5.0
PIECE_RIGHT = 95.0
PIECE_HEIGHT = 16.0
MIRROR_X = 50.0
TAB_LENGTH = 1.5
TAB_COUNT = 3


@dataclass(frozen=True)
class PieceSpec:
    name: str
    top: float
    trace_width: float
    a: float
    b: float
    c: float
    has_trim_tabs: bool
    arm_length: float

    @property
    def trim_length(self):
        return TAB_LENGTH * TAB_COUNT if self.has_trim_tabs else 0.0


@dataclass(frozen=True)
class AntennaGeometry:
    spec: PieceSpec
    left_arm: Tuple[Segment, ...]
    right_arm: Tuple[Segment, ...]
    sig_tongue: Rectangle
    gnd_tongue: Rectangle
    sig_mask: Rectangle
    gnd_mask: Rectangle
    isolation_slot: Rectangle
    feed_gap: Rectangle
    cut_lines: Tuple[Segment, ...]

    @property
    def name(self):
        return self.spec.name

    @property
    def top(self):
        return self.spec.top

    @property
    def trace_width(self):
        return self.spec.trace_width


PIECE_SPECS = (
    PieceSpec("S0", 6.0, 2.0, 30.0, 13.0, 12.5, True, 70.5),
    PieceSpec("S-4", 29.0, 2.0, 28.0, 12.5, 10.0, False, 65.5),
    PieceSpec("D0", 52.0, 2.5, 30.0, 15.0, 15.5, True, 75.5),
    PieceSpec("D-5", 75.0, 2.5, 29.0, 13.0, 13.0, False, 70.0),
)
PIECE_NAMES = tuple(spec.name for spec in PIECE_SPECS)
_SPECS_BY_NAME = {spec.name: spec for spec in PIECE_SPECS}


def mirror_segment(segment):
    """Mirror a panel-coordinate segment about x=50 without reversing it."""
    return tuple((2.0 * MIRROR_X - x, y) for x, y in segment)


def _left_arm(spec):
    top = spec.top
    yt, ym, yb = top + 3.0, top + 8.0, top + 13.0
    first_fold_x = 47.0 - spec.a
    second_fold_x = first_fold_x + spec.b
    solid_end_x = second_fold_x - (spec.c - spec.trim_length)
    final_end_x = second_fold_x - spec.c
    segments = [
        ((47.0, ym), (47.0, yt)),
        ((47.0, yt), (first_fold_x, yt)),
        ((first_fold_x, yt), (first_fold_x, ym)),
        ((first_fold_x, ym), (second_fold_x, ym)),
        ((second_fold_x, ym), (second_fold_x, yb)),
        ((second_fold_x, yb), (solid_end_x, yb)),
    ]
    if spec.has_trim_tabs:
        segments.append(((solid_end_x, yb), (final_end_x, yb)))
    return tuple(segments)


def _cut_lines(spec):
    if not spec.has_trim_tabs:
        return ()
    yb = spec.top + 13.0
    solid_end_x = 47.0 - spec.a + spec.b - (spec.c - spec.trim_length)
    lines = []
    for index in range(TAB_COUNT):
        left_x = solid_end_x - TAB_LENGTH * (index + 1)
        for x in (left_x, 2.0 * MIRROR_X - left_x):
            lines.append(((x, yb - 1.8), (x, yb + 1.8)))
    return tuple(lines)


@lru_cache(maxsize=None)
def geometry_for(name):
    """Return one immutable piece geometry in panel coordinates."""
    try:
        spec = _SPECS_BY_NAME[name]
    except KeyError:
        raise ValueError("Unknown antenna variant: %s" % name)
    top = spec.top
    left_arm = _left_arm(spec)
    return AntennaGeometry(
        spec=spec,
        left_arm=left_arm,
        right_arm=tuple(mirror_segment(segment) for segment in left_arm),
        sig_tongue=((42.0, top + 0.5), (50.4, top + 5.5)),
        gnd_tongue=((51.6, top + 0.5), (58.0, top + 5.5)),
        sig_mask=((42.5, top + 0.7), (50.0, top + 3.4)),
        gnd_mask=((52.0, top + 0.7), (57.5, top + 3.4)),
        isolation_slot=((50.4, top), (51.6, top + 3.4)),
        # Common vertical extent of the two tongues; its horizontal width is
        # the physical 1.2 mm feed gap bridged by each solver's lumped port.
        feed_gap=((50.4, top + 0.5), (51.6, top + 5.5)),
        cut_lines=_cut_lines(spec),
    )


def all_geometries():
    """Return the four pieces in their frozen panel order."""
    return tuple(geometry_for(name) for name in PIECE_NAMES)
