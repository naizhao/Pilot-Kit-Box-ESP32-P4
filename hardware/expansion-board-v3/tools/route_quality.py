#!/usr/bin/env python3
"""纯几何布线质量门禁：不依赖 KiCad，可被单测和板级检查共用。"""

from collections import Counter, defaultdict
from dataclasses import dataclass, field
import math


# ROUTES.json 和 KiCad 都量化到 0.0001mm；浮点往返会把理论上的同一点/45°
# 表示成相差一个最小量化单位。容差略高于 0.1µm，避免把序列化噪声当几何错误。
EPS = 1.1e-4


@dataclass(frozen=True, order=True)
class Segment:
    net: str
    layer: str
    start: tuple[float, float]
    end: tuple[float, float]
    width: float


@dataclass
class QualityReport:
    non45: list = field(default_factory=list)
    duplicates: list = field(default_factory=list)
    interior_joins: list = field(default_factory=list)
    copper_touches: list = field(default_factory=list)
    right_angle_corners: list = field(default_factory=list)
    collinear_splices: list = field(default_factory=list)

    @property
    def problem_count(self):
        return sum(len(getattr(self, name)) for name in (
            "non45", "duplicates", "interior_joins", "copper_touches",
            "right_angle_corners", "collinear_splices",
        ))


def _point(point):
    return (round(float(point[0]), 4), round(float(point[1]), 4))


def normalize_record(net, layer, start, end, width):
    a, b = _point(start), _point(end)
    if b < a:
        a, b = b, a
    return Segment(str(net), str(layer), a, b, round(float(width), 4))


def direction_kind(segment):
    dx = abs(segment.end[0] - segment.start[0])
    dy = abs(segment.end[1] - segment.start[1])
    if dx <= EPS and dy <= EPS:
        return "zero"
    if dx <= EPS or dy <= EPS:
        return "orthogonal"
    if abs(dx - dy) <= EPS:
        return "45deg"
    return "non45"


def _point_segment_distance(point, segment):
    ax, ay = segment.start
    bx, by = segment.end
    dx, dy = bx - ax, by - ay
    den = dx * dx + dy * dy
    if den <= EPS * EPS:
        return math.dist(point, segment.start), 0.0
    u = ((point[0] - ax) * dx + (point[1] - ay) * dy) / den
    clamped = max(0.0, min(1.0, u))
    q = (ax + clamped * dx, ay + clamped * dy)
    return math.dist(point, q), u


def _outward_vector(segment, vertex):
    other = segment.end if segment.start == vertex else segment.start
    return (other[0] - vertex[0], other[1] - vertex[1])


def _angle_between(a, b):
    la, lb = math.hypot(*a), math.hypot(*b)
    if la <= EPS or lb <= EPS:
        return 0.0
    cosine = max(-1.0, min(1.0, (a[0] * b[0] + a[1] * b[1]) / (la * lb)))
    return math.degrees(math.acos(cosine))


def analyze_records(records, corner_exemptions=()):
    records = [r if isinstance(r, Segment) else normalize_record(*r) for r in records]
    report = QualityReport()
    corner_exemptions = set(corner_exemptions)

    report.non45 = [r for r in records if direction_kind(r) in {"zero", "non45"}]
    report.duplicates = [(r, count) for r, count in Counter(records).items() if count > 1]

    unique = sorted(set(records))
    index = {segment: i for i, segment in enumerate(unique)}
    parents = list(range(len(unique)))

    def find(item):
        while parents[item] != item:
            parents[item] = parents[parents[item]]
            item = parents[item]
        return item

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parents[rb] = ra

    endpoint_owners = defaultdict(list)
    for segment in unique:
        endpoint_owners[(segment.net, segment.layer, segment.start)].append(segment)
        endpoint_owners[(segment.net, segment.layer, segment.end)].append(segment)
    for attached in endpoint_owners.values():
        for segment in attached[1:]:
            union(index[attached[0]], index[segment])

    interior = set()
    touches = set()
    for i, first in enumerate(unique):
        for second in unique[i + 1:]:
            if (first.net, first.layer) != (second.net, second.layer):
                continue
            shared = {first.start, first.end} & {second.start, second.end}
            if shared:
                continue
            pair_has_interior = False
            for endpoint, owner, trunk in (
                (first.start, first, second), (first.end, first, second),
                (second.start, second, first), (second.end, second, first),
            ):
                distance, u = _point_segment_distance(endpoint, trunk)
                if distance <= EPS and EPS < u < 1.0 - EPS:
                    interior.add((endpoint, owner, trunk))
                    pair_has_interior = True
            if pair_has_interior:
                continue
            # “只靠铜宽搭接”只描述两个精确几何分量之间的假连接。同一条已经按顶点
            # 连通的折线在小倒角两侧也可能进入彼此半线宽，不能误报为断头搭接。
            if find(index[first]) == find(index[second]):
                continue
            limit = (first.width + second.width) / 2.0 + EPS
            distances = [
                _point_segment_distance(first.start, second)[0],
                _point_segment_distance(first.end, second)[0],
                _point_segment_distance(second.start, first)[0],
                _point_segment_distance(second.end, first)[0],
            ]
            minimum = min(distances)
            if EPS < minimum <= limit:
                touches.add((first, second, round(minimum, 4)))

    report.interior_joins = sorted(interior, key=repr)
    report.copper_touches = sorted(touches, key=repr)

    vertices = defaultdict(list)
    for segment in unique:
        vertices[(segment.net, segment.layer, segment.start)].append(segment)
        vertices[(segment.net, segment.layer, segment.end)].append(segment)
    for (net, layer, vertex), segments in vertices.items():
        if len(segments) != 2 or (net, layer, vertex) in corner_exemptions:
            continue
        first, second = segments
        angle = _angle_between(_outward_vector(first, vertex), _outward_vector(second, vertex))
        item = (net, layer, vertex, first, second)
        if abs(angle - 90.0) <= 0.01:
            report.right_angle_corners.append(item)
        elif abs(angle - 180.0) <= 0.01 and abs(first.width - second.width) <= EPS:
            report.collinear_splices.append(item)

    return report
