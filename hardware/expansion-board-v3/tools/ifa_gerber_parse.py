#!/usr/bin/env python3
"""从 EasyEDA 导出的 RS-274X gerber 里提取 IFA 天线的一手几何。

⚠️ 两个坑（都踩过）：
  · D 码选择用的是**老式** `G54D12*`，正则只匹配 `D12*` 会一条线段都拿不到
  · 坐标是 %FSLAX45Y45%（4 整数 5 小数、前导零省略），要除以 1e5

只信文件，不用任何记忆里的数字。
"""
import re
import sys
import math
from collections import defaultdict

path = sys.argv[1]
src = open(path, encoding="utf-8", errors="replace").read()

# ── aperture 表 ────────────────────────────────────────────────
AP = {}
for m in re.finditer(r"%ADD(\d+)([A-Z][A-Za-z0-9]*),?([^*]*)\*%", src):
    code, shape, params = int(m.group(1)), m.group(2), m.group(3)
    vals = [float(v) for v in params.split("X") if re.match(r"^-?[\d.]+$", v)] if params else []
    AP[code] = (shape, vals)

# ── 坐标格式 ───────────────────────────────────────────────────
fs = re.search(r"%FSLAX(\d)(\d)Y(\d)(\d)\*%", src)
assert fs, "找不到 %FS 坐标格式"
DEC = int(fs.group(2))
SCALE = 10 ** DEC


def num(s):
    return int(s) / SCALE


# ── 逐行走状态机 ───────────────────────────────────────────────
cur_ap = None
x = y = 0.0
segs = defaultdict(list)          # aperture -> [(x1,y1,x2,y2), ...]
regions = []                      # G36/G37 填充区多边形
in_region = False
region_pts = []

for line in src.splitlines():
    line = line.strip()
    if not line:
        continue
    # 老式 + 新式 aperture 选择
    m = re.match(r"^(?:G54)?D(\d+)\*$", line)
    if m and int(m.group(1)) >= 10:
        cur_ap = int(m.group(1))
        continue
    if line.startswith("G36"):
        in_region, region_pts = True, []
        continue
    if line.startswith("G37"):
        if len(region_pts) >= 3:
            regions.append(region_pts[:])
        in_region, region_pts = False, []
        continue
    m = re.match(r"^(?:G0?[123])?(?:X(-?\d+))?(?:Y(-?\d+))?(?:I(-?\d+))?(?:J(-?\d+))?D0?([123])\*$", line)
    if not m:
        continue
    nx = num(m.group(1)) if m.group(1) else x
    ny = num(m.group(2)) if m.group(2) else y
    op = m.group(5)
    if in_region:
        if op in ("1", "2"):
            region_pts.append((nx, ny))
    else:
        if op == "1" and cur_ap is not None:
            segs[cur_ap].append((x, y, nx, ny))
    x, y = nx, ny

print(f"=== {path} ===")
print(f"坐标格式 X{fs.group(1)}{fs.group(2)} → 除以 {SCALE}")
print(f"aperture: {len(AP)} 个   填充区(G36): {len(regions)} 个")
print("\n各 aperture 的线段统计（只列有线段的）：")
for code in sorted(segs):
    shape, vals = AP.get(code, ("?", []))
    L = sum(math.dist((a, b), (c, d)) for a, b, c, d in segs[code])
    xs = [v for s in segs[code] for v in (s[0], s[2])]
    ys = [v for s in segs[code] for v in (s[1], s[3])]
    print(f"  D{code:<3} {shape}{vals}  段数 {len(segs[code]):5d}  总长 {L:9.2f}mm  "
          f"范围 x[{min(xs):8.3f},{max(xs):8.3f}] y[{min(ys):8.3f},{max(ys):8.3f}]")
