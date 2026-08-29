#!/usr/bin/env python3
"""焊盘 → 过孔的短接线，拆成「正交 + 45°」两段。

## 为什么必须是 45°

freerouting 的自动布线本身是好的——2026-08-22 实测 213 个未布网络布到只剩 18 个，
只用了 4 分 20 秒。问题出在它 autoroute 之后的「45 度化」后处理：板上**已经存在**
的非 45° 走线会让它陷进去不出来，日志停在

    INFO  Auto-router session completed: ... final score: 981.50 (18 unrouted).
    WARN  after autoroute: 87 traces not 45 degree

然后跑满一个核不退出，SES 一直不生成，整条链前功尽弃。
（同族问题见 memory: project_freerouting_stackoverflow_orthogonal_paths——
PolylineTrace.combine 无限递归，加 -Xss 没用。）

那 87 条不是 freerouting 布的，是我们自己预布的扇出/缝合短接线：

    GND 60 + 3V3_DIG 21 + 3V3_RF 4 + RP_1V1 2 = 87

数字精确对上。它们是 30°/60° 而不是随机角度——过孔按规则放在焊盘的斜外侧
（典型偏移 0.65 × 0.375mm，atan(0.375/0.65) = 30°），两点之间直接连一条线就成了 30°。

## 拆法

长边先走正交，剩下的等量差交给斜段：

    |dx| > |dy|:  (px,py) → (px + sign(dx)·(|dx|-|dy|), py) → (vx,vy)
    否则:         (px,py) → (px, py + sign(dy)·(|dy|-|dx|)) → (vx,vy)

两段折线**严格落在起终点构成的那个矩形内**，而原来那条直线也在同一个矩形内——
所以占地范围一点没变，不会碰到原本避开的元件或过孔。这是可以放心替换的前提。

dx 或 dy 为零、或本来就是 45° 时退化成单段，不产生零长线段
（零长线段 KiCad 会当成 dangling，DRC 报一堆假违例）。

放在这里而不是各自复制一份：这块板已经因为「同一份数据存了两份、改一处漏一处」
栽过 5 次（见 board_meta.py 开头那句）。
"""
import pcbnew

EPS = 1e-9


# 正交段短于这个长度就不要了，直接从中间点起步。
#
# 为什么需要：pad 到 via 的位移常常是「差一点点就是 45°」，比如 (0.374, 0.375)——
# 拆出来的正交段只有 **1um**。这么短的段导出 DSN 时会被合并掉，剩下的斜段是
# 44.92°，freerouting 照样判它 not 45 degree（实测第一次修完仍残留 20 条，
# 全是 44.92°/45.08°）。
#
# 丢掉那一小段，等于把走线起点从焊盘中心挪开 1um——焊盘最小也有 0.2mm，
# 偏移量比制造公差还小两个数量级，电气上就是同一个点，KiCad 的连接判定也认。
# 阈值取 10um：比这更长的正交段是真实几何，不能丢。
MIN_SEG = 0.010


def path45(px, py, vx, vy):
    """(px,py) → (vx,vy) 的折线顶点，每段要么正交要么严格 45°。"""
    dx, dy = vx - px, vy - py
    adx, ady = abs(dx), abs(dy)
    if adx < EPS or ady < EPS or abs(adx - ady) < EPS:
        return [(px, py), (vx, vy)]          # 已经是正交或 45°，不用拆
    sx = 1.0 if dx > 0 else -1.0
    sy = 1.0 if dy > 0 else -1.0
    if adx > ady:
        mid = (px + sx * (adx - ady), py)
    else:
        mid = (px, py + sy * (ady - adx))
    if abs(adx - ady) < MIN_SEG:
        return [mid, (vx, vy)]               # 正交段短到没意义，起点直接用 mid
    return [(px, py), mid, (vx, vy)]


def add_track45(board, px, py, vx, vy, width_mm, layer, net):
    """按 45° 规则铺一条短接线，返回实际生成的段数。"""
    pts = path45(px, py, vx, vy)
    for (x1, y1), (x2, y2) in zip(pts[:-1], pts[1:]):
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(pcbnew.VECTOR2I_MM(round(x1, 4), round(y1, 4)))
        t.SetEnd(pcbnew.VECTOR2I_MM(round(x2, 4), round(y2, 4)))
        t.SetWidth(pcbnew.FromMM(width_mm))
        t.SetLayer(layer)
        t.SetNet(net)
        board.Add(t)
    return len(pts) - 1
