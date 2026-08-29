#!/usr/bin/env python3
"""把 ROUTES.json 规范化为精确顶点连接和 0/45/90° 线段。"""

from collections import defaultdict
import json
import math
import sys

from route_quality import EPS, Segment, analyze_records, direction_kind, normalize_record


def _distance_and_parameter(point, segment):
    ax, ay = segment.start
    bx, by = segment.end
    dx, dy = bx - ax, by - ay
    length_squared = dx * dx + dy * dy
    if length_squared <= EPS * EPS:
        return math.dist(point, segment.start), 0.0, segment.start
    u = ((point[0] - ax) * dx + (point[1] - ay) * dy) / length_squared
    clamped = max(0.0, min(1.0, u))
    projected = (round(ax + clamped * dx, 4), round(ay + clamped * dy, 4))
    return math.dist(point, projected), u, projected


def _dedupe(records):
    records = list(records)
    while True:
        short = next((item for item in records
                      if math.dist(item.start, item.end) <= EPS * 1.01), None)
        if short is None:
            return sorted(set(records))
        records.remove(short)
        if short.start == short.end:
            continue
        collapsed = []
        for item in records:
            if (item.net, item.layer) != (short.net, short.layer):
                collapsed.append(item)
                continue
            start = short.start if item.start == short.end else item.start
            end = short.start if item.end == short.end else item.end
            collapsed.append(normalize_record(item.net, item.layer, start, end, item.width))
        records = collapsed


def _replace_vertex(records, net, layer, old, new):
    replaced = []
    for item in records:
        if (item.net, item.layer) != (net, layer):
            replaced.append(item)
            continue
        start = new if item.start == old else item.start
        end = new if item.end == old else item.end
        replaced.append(normalize_record(item.net, item.layer, start, end, item.width))
    return _dedupe(replaced)


def _snap_near_axis(records):
    near = EPS * 1.01
    snapped = []
    for item in records:
        start, end = item.start, item.end
        if 0 < abs(end[0] - start[0]) <= near:
            end = (start[0], end[1])
        if 0 < abs(end[1] - start[1]) <= near:
            end = (end[0], start[1])
        snapped.append(normalize_record(item.net, item.layer, start, end, item.width))
    return _dedupe(snapped)


def _split_interior_joins(records):
    grouped_vertices = defaultdict(set)
    for item in records:
        grouped_vertices[(item.net, item.layer)].update((item.start, item.end))

    split = []
    for item in records:
        points = [(0.0, item.start), (1.0, item.end)]
        for point in grouped_vertices[(item.net, item.layer)]:
            distance, u, projected = _distance_and_parameter(point, item)
            if distance <= EPS and EPS < u < 1.0 - EPS:
                points.append((u, projected))
        points = [point for _, point in sorted(set(points))]
        for start, end in zip(points[:-1], points[1:]):
            split.append(normalize_record(item.net, item.layer, start, end, item.width))
    return _dedupe(split)


def _merge_collinear(records, anchors):
    records = _dedupe(records)
    while True:
        vertices = defaultdict(list)
        for item in records:
            vertices[(item.net, item.layer, item.start)].append(item)
            vertices[(item.net, item.layer, item.end)].append(item)
        merged = False
        for (net, layer, vertex), attached in vertices.items():
            if len(attached) != 2 or (net, layer, vertex) in anchors:
                continue
            first, second = attached
            if abs(first.width - second.width) > EPS:
                continue
            a = first.end if first.start == vertex else first.start
            b = second.end if second.start == vertex else second.start
            va = (a[0] - vertex[0], a[1] - vertex[1])
            vb = (b[0] - vertex[0], b[1] - vertex[1])
            cross = va[0] * vb[1] - va[1] * vb[0]
            dot = va[0] * vb[0] + va[1] * vb[1]
            lengths = math.hypot(*va) * math.hypot(*vb)
            if lengths <= EPS * EPS or abs(cross) / lengths > 1e-6 or dot >= 0:
                continue
            records = [item for item in records if item not in (first, second)]
            records.append(normalize_record(net, layer, a, b, first.width))
            records = _dedupe(records)
            merged = True
            break
        if not merged:
            return records


def _snap_copper_touches(records, anchors):
    for _ in range(1000):
        report = analyze_records(records)
        if not report.copper_touches:
            return records
        first, second, _ = report.copper_touches[0]
        candidates = []
        for owner, other in ((first, second), (second, first)):
            for endpoint in (owner.start, owner.end):
                distance, u, projected = _distance_and_parameter(endpoint, other)
                candidates.append((distance, endpoint, owner, other, u, projected))
        distance, endpoint, owner, other, u, projected = min(candidates, key=lambda item: item[0])
        assert distance <= (owner.width + other.width) / 2.0 + EPS

        owner_key = (owner.net, owner.layer, endpoint)
        if EPS < u < 1.0 - EPS:
            if owner_key not in anchors:
                records = _replace_vertex(records, owner.net, owner.layer, endpoint, projected)
            else:
                records.append(normalize_record(
                    owner.net, owner.layer, endpoint, projected,
                    min(owner.width, other.width),
                ))
        else:
            other_endpoint = other.start if u <= 0.5 else other.end
            other_key = (other.net, other.layer, other_endpoint)
            if owner_key in anchors:
                target = endpoint
            elif other_key in anchors:
                target = other_endpoint
            else:
                target = (
                    round((endpoint[0] + other_endpoint[0]) / 2.0, 4),
                    round((endpoint[1] + other_endpoint[1]) / 2.0, 4),
                )
            records = _replace_vertex(records, owner.net, owner.layer, endpoint, target)
            records = _replace_vertex(records, other.net, other.layer, other_endpoint, target)
        records = _split_interior_joins(_dedupe(records))
    raise AssertionError("铜宽搭接规范化超过 1000 次，几何没有收敛")


def _split_non45(records):
    result = []
    for item in records:
        if direction_kind(item) != "non45":
            result.append(item)
            continue
        x1, y1 = item.start
        x2, y2 = item.end
        dx, dy = x2 - x1, y2 - y1
        adx, ady = abs(dx), abs(dy)
        if adx > ady:
            middle = (round(x1 + math.copysign(adx - ady, dx), 4), y1)
        else:
            middle = (x1, round(y1 + math.copysign(ady - adx, dy), 4))
        result.extend((
            normalize_record(item.net, item.layer, item.start, middle, item.width),
            normalize_record(item.net, item.layer, middle, item.end, item.width),
        ))
    return _dedupe(result)


def _chamfer_right_angles(records, anchors):
    while True:
        report = analyze_records(records)
        candidate = next((item for item in report.right_angle_corners
                          if (item[0], item[1], item[2]) not in anchors), None)
        if candidate is None:
            return records
        net, layer, vertex, first, second = candidate
        first_other = first.end if first.start == vertex else first.start
        second_other = second.end if second.start == vertex else second.start
        first_len = math.dist(vertex, first_other)
        second_len = math.dist(vertex, second_other)
        trim = min(0.2, first_len / 3.0, second_len / 3.0)

        def toward(point, distance):
            length = math.dist(vertex, point)
            return (
                round(vertex[0] + (point[0] - vertex[0]) * distance / length, 4),
                round(vertex[1] + (point[1] - vertex[1]) * distance / length, 4),
            )

        p1, p2 = toward(first_other, trim), toward(second_other, trim)
        records = [item for item in records if item not in (first, second)]
        records.extend((
            normalize_record(net, layer, first_other, p1, first.width),
            normalize_record(net, layer, p1, p2, min(first.width, second.width)),
            normalize_record(net, layer, p2, second_other, second.width),
        ))
        records = _dedupe(records)


def cleanup_records(records, anchors=()):
    records = [item if isinstance(item, Segment) else normalize_record(*item) for item in records]
    anchors = set(anchors)
    records = _dedupe(records)
    for _ in range(6):
        records = _split_interior_joins(_snap_near_axis(records))
        records = _snap_copper_touches(records, anchors)
        records = _split_non45(records)
        records = _split_interior_joins(_snap_near_axis(records))
        records = _merge_collinear(records, anchors)
        records = _chamfer_right_angles(records, anchors)
        report = analyze_records(records, corner_exemptions=anchors)
        if report.problem_count == 0:
            break
    # 收敛末尾只做角度量化；不再移动顶点，避免最后一次吸附把 45° 改回任意角。
    records = _split_non45(_snap_near_axis(records))
    records = _split_interior_joins(records)
    return _merge_collinear(records, anchors)


def _pad_anchors(board_path):
    """读取焊盘中心作为拐角豁免；只有 CLI 需要 KiCad，纯几何测试不依赖它。"""
    import pcbnew

    board = pcbnew.LoadBoard(board_path)
    layer_names = {board.GetLayerID(name): name for name in
                   ("F.Cu", "In1.Cu", "In2.Cu", "In3.Cu", "In4.Cu", "B.Cu")}
    anchors = set()
    for footprint in board.GetFootprints():
        for pad in footprint.Pads():
            if not pad.GetNetname():
                continue
            position = (round(pcbnew.ToMM(pad.GetPosition().x), 4),
                        round(pcbnew.ToMM(pad.GetPosition().y), 4))
            for layer_id in pad.GetLayerSet().Seq():
                if layer_id in layer_names:
                    anchors.add((pad.GetNetname(), layer_names[layer_id], position))
    return anchors


def _main(argv):
    if len(argv) not in (3, 4):
        raise SystemExit("用法: route_cleanup.py 输入ROUTES.json 输出ROUTES.json [焊盘来源PCB]")
    source, destination = argv[1:3]
    with open(source, encoding="utf-8") as handle:
        data = json.load(handle)
    records = [normalize_record(net, layer, (x1, y1), (x2, y2), width)
               for net, layer, x1, y1, x2, y2, width in data["tracks"]]
    anchors = {(net, layer, (round(x, 4), round(y, 4)))
               for net, x, y, _diameter, _drill in data["vias"]
               for layer in ("F.Cu", "In1.Cu", "In2.Cu", "In3.Cu", "In4.Cu", "B.Cu")}
    if len(argv) == 4:
        anchors.update(_pad_anchors(argv[3]))
    cleaned = cleanup_records(records, anchors)
    data["tracks"] = [[item.net, item.layer, *item.start, *item.end, item.width]
                      for item in cleaned]
    data["source"] = "route_cleanup.py"
    with open(destination, "w", encoding="utf-8") as handle:
        json.dump(data, handle, ensure_ascii=False, indent=0)
        handle.write("\n")
    report = analyze_records(cleaned, corner_exemptions=anchors)
    print(f"走线 {len(records)} → {len(cleaned)}；剩余几何问题 {report.problem_count}")
    return 0 if report.problem_count == 0 else 1


if __name__ == "__main__":
    raise SystemExit(_main(sys.argv))
