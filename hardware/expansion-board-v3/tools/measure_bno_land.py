#!/usr/bin/env python3
"""从 BNO08X datasheet Fig 7-2（600dpi 渲染图）程序化测量 28 个焊盘的坐标。

方法：焊盘在图中为红色描边矩形 → 提取红色像素 → 连通域聚类 → 外接矩形。
标定：图内标注 图幅宽 3.8mm / 高 5.2mm 的黑色外框（最大黑色矩形框）。
断言：必须恰好聚出 28 个焊盘；焊盘尺寸应 ≈0.675×0.25 或 0.25×0.675（±20%）。
输出：以图幅中心为原点的毫米坐标表（KiCad footprint 坐标系，Y 向下为正）。
"""
from PIL import Image
import sys
from collections import deque

img = Image.open("/tmp/bno_land-54.png").convert("RGB")
W, H = img.size
px = img.load()

# 1) 红色像素掩码（红明显高于绿蓝）
def is_red(p):
    r, g, b = p
    return r > 130 and g < 110 and b < 110 and (r - max(g, b)) > 50

red = [[False] * W for _ in range(H)]
red_pts = []
for y in range(H):
    for x in range(W):
        if is_red(px[x, y]):
            red[y][x] = True
            red_pts.append((x, y))

assert red_pts, "没有红色像素——渲染或阈值有问题"

# 2) 连通域（8邻域，BFS），允许 2px 间隙桥接（描边可能断续）→ 用膨胀近似：直接按距离聚类太慢，先普通连通域
seen = [[False] * W for _ in range(H)]
boxes = []
for sx, sy in red_pts:
    if seen[sy][sx]:
        continue
    q = deque([(sx, sy)])
    seen[sy][sx] = True
    minx = maxx = sx
    miny = maxy = sy
    while q:
        x, y = q.popleft()
        minx, maxx = min(minx, x), max(maxx, x)
        miny, maxy = min(miny, y), max(maxy, y)
        for dy in (-2, -1, 0, 1, 2):
            for dx in (-2, -1, 0, 1, 2):
                nx, ny = x + dx, y + dy
                if 0 <= nx < W and 0 <= ny < H and red[ny][nx] and not seen[ny][nx]:
                    seen[ny][nx] = True
                    q.append((nx, ny))
    boxes.append((minx, miny, maxx, maxy))

# 3) 合并被引脚编号文字截断的矩形（同一焊盘的碎片：y 区间重叠且 x 间距 < 70px）
merged = True
while merged:
    merged = False
    for i in range(len(boxes)):
        for j in range(i + 1, len(boxes)):
            a, b = boxes[i], boxes[j]
            y_overlap = min(a[3], b[3]) - max(a[1], b[1])
            x_gap = max(a[0], b[0]) - min(a[2], b[2])
            if y_overlap > 100 and x_gap < 70:
                boxes[i] = (min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3]))
                boxes.pop(j)
                merged = True
                break
        if merged:
            break

# 过滤：实测焊盘描边约 300×115 或 114×258 px，用最短边 ≥ 100px 过滤图注噪声
pads = []
for (x0, y0, x1, y1) in boxes:
    w, h = x1 - x0 + 1, y1 - y0 + 1
    if min(w, h) >= 100 and max(w, h) <= 350:
        pads.append((x0, y0, x1, y1))

print(f"候选焊盘数: {len(pads)} (全部红色连通域 {len(boxes)})", file=sys.stderr)

# 4) 标定：右列 10 个焊盘 pitch=0.50mm（数据表），用相邻焊盘中心距的中位数求 px/mm。
centers = [((x0 + x1) / 2, (y0 + y1) / 2, x1 - x0 + 1, y1 - y0 + 1) for (x0, y0, x1, y1) in pads]
xs = sorted(c[0] for c in centers)
# 右列 = x 最大的一簇
right_col = sorted([c for c in centers if c[0] > xs[-1] - 30], key=lambda c: c[1])
gaps = [right_col[i + 1][1] - right_col[i][1] for i in range(len(right_col) - 1)]
gaps = [g for g in gaps if g > 5]
gaps.sort()
med_gap = gaps[len(gaps) // 2]
PX_PER_MM = med_gap / 0.50
print(f"右列焊盘 {len(right_col)} 个, 相邻中心距中位 {med_gap:.1f}px → {PX_PER_MM:.2f} px/mm", file=sys.stderr)

assert len(pads) == 28, f"焊盘聚类数 {len(pads)} ≠ 28，需人工核图"

# 5) 原点 = 所有焊盘外包络中心；输出 mm 坐标（KiCad: +Y 向下）
allx0 = min(p[0] for p in pads); allx1 = max(p[2] for p in pads)
ally0 = min(p[1] for p in pads); ally1 = max(p[3] for p in pads)
cx, cy = (allx0 + allx1) / 2, (ally0 + ally1) / 2
span_w = (allx1 - allx0) / PX_PER_MM
span_h = (ally1 - ally0) / PX_PER_MM
print(f"焊盘包络: {span_w:.3f} × {span_h:.3f} mm (期望 ≈3.8 × 5.2)", file=sys.stderr)
assert abs(span_w - 3.8) < 0.15 and abs(span_h - 5.2) < 0.15, "包络与 3.8×5.2 不符，标定失败"

def snap(v, step=0.025):
    return round(round(v / step) * step, 4)

out = []
for (x0, y0, x1, y1) in pads:
    mx = snap(((x0 + x1) / 2 - cx) / PX_PER_MM)
    my = snap(((y0 + y1) / 2 - cy) / PX_PER_MM)
    mw = snap((x1 - x0 + 1) / PX_PER_MM)
    mh = snap((y1 - y0 + 1) / PX_PER_MM)
    out.append((mx, my, mw, mh))

# 6) 按 Fig 7-2 编号规则赋 pin 号：
#    右列 top→bottom: 1,28,27,26,25,24,23,22,21,20
#    左列 top→bottom: 6,7,8,9,10,11,12,13,14,15
#    顶行中部 left→right: 5,4,3,2 ；底行中部 left→right: 16,17,18,19
LEFT = sorted([p for p in out if p[0] < -1.2], key=lambda p: p[1])
RIGHT = sorted([p for p in out if p[0] > 1.2], key=lambda p: p[1])
TOP = sorted([p for p in out if -1.2 <= p[0] <= 1.2 and p[1] < 0], key=lambda p: p[0])
BOT = sorted([p for p in out if -1.2 <= p[0] <= 1.2 and p[1] > 0], key=lambda p: p[0])
assert len(LEFT) == 10 and len(RIGHT) == 10 and len(TOP) == 4 and len(BOT) == 4, \
    f"分组错误 L{len(LEFT)} R{len(RIGHT)} T{len(TOP)} B{len(BOT)}"

names_right = ["1", "28", "27", "26", "25", "24", "23", "22", "21", "20"]
names_left = ["6", "7", "8", "9", "10", "11", "12", "13", "14", "15"]
names_top = ["5", "4", "3", "2"]
names_bot = ["16", "17", "18", "19"]

result = {}
for n, p in zip(names_right, RIGHT):
    result[n] = p
for n, p in zip(names_left, LEFT):
    result[n] = p
for n, p in zip(names_top, TOP):
    result[n] = p
for n, p in zip(names_bot, BOT):
    result[n] = p

for n in [str(i) for i in range(1, 29)]:
    mx, my, mw, mh = result[n]
    print(f"{n}\t{mx:.4f}\t{my:.4f}\t{mw:.3f}\t{mh:.3f}")
