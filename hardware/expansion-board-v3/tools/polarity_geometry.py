"""极性标记的纯几何规则，供生成脚本和单元测试共用。"""

import math


def is_diode_target(reference, pad_numbers):
    """所有具有 1/2 两个焊盘的 D* 器件都必须接受极性审计。"""
    pads = {str(number) for number in pad_numbers}
    return reference.upper().startswith("D") and {"1", "2"}.issubset(pads)


def marker_segment(center, cathode, anode, offset, length):
    """返回阴极外侧、垂直于 K-A 轴的一条标记线。"""
    vx, vy = cathode[0] - anode[0], cathode[1] - anode[1]
    norm = math.hypot(vx, vy)
    if norm == 0:
        raise ValueError("阴极和阳极焊盘重合，无法确定极性方向")
    vx, vy = vx / norm, vy / norm
    mx = center[0] + vx * offset
    my = center[1] + vy * offset
    px, py = -vy, vx
    half = length / 2
    return (
        (round(mx - px * half, 3), round(my - py * half, 3)),
        (round(mx + px * half, 3), round(my + py * half, 3)),
    )


def has_clear_cathode_bar(center, cathode, anode, segments):
    """判断封装丝印中是否已有位于阴极外侧的清晰横杠。

    segments 元素为 ``(start, end, width_mm)``。标记至少 0.5 mm 长、
    0.1 mm 宽，基本垂直于 K-A 轴，并位于阴极焊盘中心之外。
    """
    vx, vy = cathode[0] - anode[0], cathode[1] - anode[1]
    norm = math.hypot(vx, vy)
    if norm == 0:
        return False
    vx, vy = vx / norm, vy / norm
    cathode_distance = ((cathode[0] - center[0]) * vx
                        + (cathode[1] - center[1]) * vy)
    for start, end, width in segments:
        dx, dy = end[0] - start[0], end[1] - start[1]
        length = math.hypot(dx, dy)
        if length < 0.5 or width < 0.1:
            continue
        parallel = abs((dx * vx + dy * vy) / length)
        if parallel > 0.2:
            continue
        midpoint = ((start[0] + end[0]) / 2, (start[1] + end[1]) / 2)
        projection = ((midpoint[0] - center[0]) * vx
                      + (midpoint[1] - center[1]) * vy)
        if projection >= cathode_distance + 0.2:
            return True
    return False
