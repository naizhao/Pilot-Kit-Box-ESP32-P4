#!/usr/bin/env python3
"""顶部全层禁铜带补齐（纯文本手术，v4 同构三段式顶部的左右两段）。

v4 的做法：整条顶部 y50.077-58.512 无铜，由三段拼成——
  ANT1090_short_end_keepout（板左缘→封装禁铜区左沿）+ ANT1 封装自带禁铜区
  + ANT1090_open_end_keepout（封装右沿→板右缘）。H2 螺丝孔整个罩在无铜带里。

v3 移植时只带了封装（gen_ifa_footprint 生成），左右两段必须本脚本补。
gen_pcb.py 是 NewBoard() 从零重建，每次 rebuild 后都要重跑本脚本。

## 为什么是文本手术而不是 pcbnew API

ZONE(board) + 多层 LSET + Save 实测 Bus error/Segfault（KiCad 8.0 SWIG，
单层 F.Cu zone 可存，多层必崩）。zone 的 S 表达式块结构稳定，直接从
v4 冻结文件提取模板、改坐标插入，100% 可靠且幂等（先删同名块再插）。

运行：python3 top_keepout_v3.py [板路径]
"""
import os
import sys

V3 = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__)),
                                  ), "kicad", "expansion-board-v3.kicad_pcb")

# v4 冻结模板（dd22ebc/ed1873c 的 expansion-board-v4.kicad_pcb）
TEMPLATE = '''	(zone
		(layers "F.Cu" "In1.Cu" "In2.Cu" "In3.Cu" "In4.Cu" "B.Cu")
		(uuid "{uuid}")
		(name "{name}")
		(hatch edge 0.5)
		(connect_pads
			(clearance 0)
		)
		(min_thickness 0.25)
		(keepout
			(tracks not_allowed)
			(vias not_allowed)
			(pads allowed)
			(copperpour not_allowed)
			(footprints allowed)
		)
		(placement
			(enabled no)
			(sheetname "")
		)
		(fill
			(thermal_gap 0.5)
			(thermal_bridge_width 0.5)
			(island_removal_mode 0)
		)
		(polygon
			(pts
				(xy {x0} {y0}) (xy {x1} {y0}) (xy {x1} {y1}) (xy {x0} {y1})
			)
		)
	)
'''

# 边界：ANT1 锚点 85.460 + 封装禁铜区 KEEPOUT_LOCAL = x[77.722, 135.222]
# y 带 = 封装禁铜区上下沿 [50.077, 58.512]（上沿 = 板边 50 + 0.077，同 v4）
ZONES = [
    ("ANT1090_short_end_keepout", "6f100b2a-0000-4000-8000-00000000a001",
     50.000, 77.722, 50.077, 58.512),
    ("ANT1090_open_end_keepout", "6f100b2a-0000-4000-8000-00000000a002",
     135.222, 150.000, 50.077, 58.512),
]


def find_zone_end(t, i):
    """从 name 行回溯 (zone 起，找配对括号止。"""
    start = t.rfind('(zone', 0, i)
    depth = 0
    j = start
    while j < len(t):
        c = t[j]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                break
        j += 1
    return start, j + 1


def main(path):
    txt = open(path).read()
    for name, *_ in ZONES:
        if f'(name "{name}")' in txt:
            i = txt.find(f'(name "{name}")')
            s, e = find_zone_end(txt, i)
            txt = txt[:s] + txt[e:]
    last = txt.rfind('\t(zone')
    if last < 0:
        print("板上没有任何 zone——异常，中止")
        sys.exit(1)
    depth = 0
    j = last
    while j < len(txt):
        c = txt[j]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                break
        j += 1
    blocks = []
    for name, uuid, x0, x1, y0, y1 in ZONES:
        blocks.append(TEMPLATE.format(uuid=uuid, name=name, x0=x0, x1=x1, y0=y0, y1=y1))
    txt = txt[:j + 1] + '\n' + '\n'.join(blocks) + txt[j + 1:]
    open(path, 'w').write(txt)
    for name, _, x0, x1, y0, y1 in ZONES:
        print(f"  {name}: x[{x0},{x1}] y[{y0},{y1}] 全6铜层")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else
         os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                      "kicad", "expansion-board-v3.kicad_pcb"))
