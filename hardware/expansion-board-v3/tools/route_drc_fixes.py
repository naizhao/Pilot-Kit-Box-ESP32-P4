#!/usr/bin/env python3
"""按 KiCad DRC 报告从 ROUTES.json 删除明确悬空的走线/过孔。"""

from copy import deepcopy
import json
import math
import re
import sys

from drc_classify import extract_layer_name, extract_net_name


POSITION_TOLERANCE = 1.1e-4
LENGTH_TOLERANCE = 1.5e-4
U16_GND_TRACK = ["GND", "F.Cu", 112.5, 62.1, 113.7625, 62.1, 0.25]
U16_GND_VIA = ["GND", 112.5, 62.1, 0.45, 0.3]
POWER_ISLANDS = ((58.0, 60.0, 146.0, 84.0), (83.0, 84.3, 94.0, 89.0))
FROZEN_IFA_FEED = (
    ["ANT1090_IFA", "F.Cu", 85.472, 60.012, 85.472, 62.865, 0.15],
    ["ANT1090_IFA", "F.Cu", 85.472, 62.865, 87.1845, 62.865, 0.15],
)


def _item(violation):
    items = violation.get("items", ())
    assert len(items) == 1, f"悬空违例应只指向一个对象: {items}"
    return items[0]


def _position(item):
    pos = item["pos"]
    return float(pos["x"]), float(pos["y"])


def _track_length(description):
    match = re.search(r"(?:长度|length)\s*:\s*([0-9.]+)\s*mm", description, re.I)
    assert match, f"DRC 走线描述没有长度: {description}"
    return float(match.group(1))


def prune_dangling(data, report):
    cleaned = deepcopy(data)
    counts = {"vias": 0, "tracks": 0}
    for violation in report.get("violations", ()):
        violation_type = violation.get("type")
        if violation_type not in {"via_dangling", "track_dangling"}:
            continue
        item = _item(violation)
        description = item.get("description", "")
        net = extract_net_name(violation)
        x, y = _position(item)

        if violation_type == "via_dangling":
            matches = [via for via in cleaned["vias"]
                       if math.hypot(via[1] - x, via[2] - y) <= POSITION_TOLERANCE]
            assert len(matches) == 1, f"悬空过孔匹配数 {len(matches)} != 1: {net}@{x},{y}"
            cleaned["vias"].remove(matches[0])
            counts["vias"] += 1
            continue

        layer = extract_layer_name(description)
        expected_length = _track_length(description)
        matches = []
        for track in cleaned["tracks"]:
            track_net, track_layer, x1, y1, x2, y2, _width = track
            if track_net != net or track_layer != layer:
                continue
            endpoint_match = min(math.hypot(x1 - x, y1 - y),
                                 math.hypot(x2 - x, y2 - y)) <= POSITION_TOLERANCE
            length_match = abs(math.hypot(x2 - x1, y2 - y1) - expected_length) \
                <= LENGTH_TOLERANCE
            if endpoint_match and length_match:
                matches.append(track)
        assert len(matches) == 1, f"悬空走线匹配数 {len(matches)} != 1: {description}"
        cleaned["tracks"].remove(matches[0])
        counts["tracks"] += 1
    return cleaned, counts


def ensure_u16_gnd_tie(data):
    """把 U16.2 所在的 F.Cu GND 小岛用受控走线+非 free via 接入地平面。"""
    cleaned = deepcopy(data)
    if U16_GND_TRACK not in cleaned["tracks"]:
        cleaned["tracks"].append(U16_GND_TRACK.copy())
    if U16_GND_VIA not in cleaned["vias"]:
        cleaned["vias"].append(U16_GND_VIA.copy())
    return cleaned


def prune_free_gnd_vias_in_power_islands(data):
    """删掉 B.Cu 电源岛内没有 GND 走线端点锁网的 free via。"""
    cleaned = deepcopy(data)
    anchors = {
        (round(x, 4), round(y, 4))
        for net, _layer, x1, y1, x2, y2, _width in cleaned["tracks"]
        if net == "GND"
        for x, y in ((x1, y1), (x2, y2))
    }

    def inside_power_island(via):
        net, x, y, _diameter, _drill = via
        return (net == "GND"
                and (round(x, 4), round(y, 4)) not in anchors
                and any(x0 <= x <= x1 and y0 <= y <= y1
                        for x0, y0, x1, y1 in POWER_ISLANDS))

    before = len(cleaned["vias"])
    cleaned["vias"] = [via for via in cleaned["vias"] if not inside_power_island(via)]
    return cleaned, before - len(cleaned["vias"])


def ensure_frozen_ifa_feed(data):
    """还原 HFSS 冻结的 taper 末端→ZP1→ZS1 馈电中心线。"""
    cleaned = deepcopy(data)
    cleaned["tracks"] = [track for track in cleaned["tracks"]
                         if track[0] != "ANT1090_IFA"]
    cleaned["tracks"].extend(deepcopy(FROZEN_IFA_FEED))
    cleaned["tracks"].sort(key=lambda item: tuple(item))
    return cleaned


def _main(argv):
    if len(argv) != 4:
        raise SystemExit("用法: route_drc_fixes.py 输入ROUTES.json DRC.json 输出ROUTES.json")
    with open(argv[1], encoding="utf-8") as handle:
        data = json.load(handle)
    with open(argv[2], encoding="utf-8") as handle:
        report = json.load(handle)
    cleaned, counts = prune_dangling(data, report)
    cleaned = ensure_u16_gnd_tie(cleaned)
    cleaned, free_vias = prune_free_gnd_vias_in_power_islands(cleaned)
    cleaned = ensure_frozen_ifa_feed(cleaned)
    with open(argv[3], "w", encoding="utf-8") as handle:
        json.dump(cleaned, handle, ensure_ascii=False, indent=0)
        handle.write("\n")
    print(f"删除 DRC 已确认悬空对象: {counts['tracks']} 段走线 / {counts['vias']} 个过孔")
    print(f"删除 B.Cu 电源岛内未锁网的 GND free-via: {free_vias} 个")


if __name__ == "__main__":
    raise SystemExit(_main(sys.argv))
