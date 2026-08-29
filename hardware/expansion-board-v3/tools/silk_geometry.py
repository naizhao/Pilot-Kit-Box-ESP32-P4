#!/usr/bin/env python3
"""不依赖 KiCad 的丝印矩形碰撞 helper。"""


def overlaps(first, second, clearance=0.0):
    return (
        first[0] - clearance < second[2]
        and second[0] < first[2] + clearance
        and first[1] - clearance < second[3]
        and second[1] < first[3] + clearance
    )


def first_clear_box(candidates, obstacles, bounds, clearance):
    for value, box in candidates:
        if not (bounds[0] <= box[0] and box[2] <= bounds[2]
                and bounds[1] <= box[1] and box[3] <= bounds[3]):
            continue
        if any(overlaps(box, obstacle, clearance) for obstacle in obstacles):
            continue
        return value
    return None
