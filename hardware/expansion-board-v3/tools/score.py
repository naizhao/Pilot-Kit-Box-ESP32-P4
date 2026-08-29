#!/usr/bin/env python3
"""布线完成度打分——比 DRC 的 unconnected_items 可靠。

为什么不用 DRC 那个数：
  · 它把「孤立碎铜」和「真缺线」混在一起，删碎铜反而让数字变大；
  · 它的三分类不稳定，同一份文件跑两次会在 12/13/3 和 21/2/5 之间漂——
    同一个未连通对，KiCad 描述成 Pad↔X 还是 Track↔X 取决于内部遍历顺序。

这里直接数电气连通块：一个网络理想状态是 1 块，多出来的块数就是还缺的连接数。
纯几何推导，完全确定性。
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("PK_QUIET", "1")
import route_fix as R                                            # noqa: E402

nets = {}
for n in R.board.GetNetsByNetcode().values():
    nm = n.GetNetname()
    if nm:
        nets[n.GetNetCode()] = nm

extra = 0
detail = []
for nc, nm in sorted(nets.items(), key=lambda x: x[1]):
    els = R.net_elements(nc)
    if len(els) < 2:
        continue
    b = R.blocks_of(nc)
    # 压在同网络覆铜填充实体上的块，彼此经平面互连，算一块。
    # 不做这步的话 GND 的 183 个缝合过孔会被数成 183 处"缺连接"——它们本来就
    # 是靠 GND 平面连起来的，不是缺线。
    zl = R.zone_layers(nc)
    if zl:
        onp = [blk for blk in b if R.block_on_plane(zl, blk)]
        b = [blk for blk in b if not R.block_on_plane(zl, blk)]
        if onp:
            b = b + [[e for blk in onp for e in blk]]
    if len(b) > 1:
        extra += len(b) - 1
        detail.append((len(b) - 1, nm))

detail.sort(reverse=True)
print(f"缺连接 {extra} 处 / 涉及 {len(detail)} 个网络")
for k, nm in detail:
    print(f"   {'RF ' if nm in R.RF else '   '}{nm:20s} 还差 {k} 段")
