#!/usr/bin/env python3
"""不依赖 pcbnew 的路由几何 helper。"""

import math


def _closest_point_on_segment(point, segment):
    px, py = point
    x1, y1, x2, y2 = segment
    dx, dy = x2 - x1, y2 - y1
    length_squared = dx * dx + dy * dy
    if length_squared < 1e-18:
        return x1, y1
    ratio = max(0.0, min(1.0, ((px - x1) * dx + (py - y1) * dy) / length_squared))
    return x1 + ratio * dx, y1 + ratio * dy


def _closest_point_on_element(kind, geometry, point):
    px, py = point
    if kind == "pad":
        x0, y0, x1, y1 = geometry
        return max(x0, min(x1, px)), max(y0, min(y1, py))
    if kind == "via":
        return geometry
    return _closest_point_on_segment(point, geometry)


def closest_point_on_block(block, point, layer):
    candidates = []
    for kind, geometry, layers in block:
        if layer not in layers:
            continue
        anchor = _closest_point_on_element(kind, geometry, point)
        candidates.append((math.hypot(anchor[0] - point[0], anchor[1] - point[1]), anchor))
    if not candidates:
        return None
    return min(candidates, key=lambda candidate: candidate[0])[1]
