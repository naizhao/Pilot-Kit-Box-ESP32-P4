#!/usr/bin/env python3
"""把人工精修后的布局导出成 PLACEMENT.py，并顺手做行对齐吸附。

## 为什么要冻结坐标

区域装箱器的任务已经完成了——它把 152 个元件从零摆到一个能用的起点。
用户 2026-08-02 人工精修了 81 个元件（52 个 <1mm 微调、16 个 1-5mm、13 个 5-20mm），
把 CC1312R 去耦重新聚拢、978 直流侧上移贴近 J1，这些是装箱器摆不出来的判断。

**精修之后，坐标就是设计本身，不该再被算法覆盖。**
所以 gen_pcb.py 从"生成布局"退为"按 PLACEMENT.py 摆位 + 校验"，
区域装箱降级为新元件没坐标时的兜底。所有断言保留——它们是防回归的护栏。

## 吸附规则（判据来自实测，不是拍脑袋）

同一簇 = **y 差 ≤0.35mm 且 x 间距 ≤12mm**（真正挨着的才算一行）。
- 0.35 这个阈值：实测手滑量级是 0.003–0.31mm，而有意的行间距是 0.6mm 以上。
- x 间距 12mm：不加这条的话，板子两端 40mm 外的元件会被误判成同一行
  （实测 SW1 和 C19 隔着大半块板被连成一"列"，位移近 1mm）。

吸附目标取**众数**而非中位数：多数件用户已经对齐，只有少数漂了；
取众数是往"已对齐的那条线"靠，取中位数会把对好的也拽歪。

FROZEN 里的件不参与：对扣排针与射频钉死件，几何有硬约束，动了要重新验证。

运行：~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 export_placement.py
"""
import collections
import os

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import sys as _sys
# 可指定源板子：从备份导出丝印时用得上（当前板子的丝印可能已被重排过）
PCB = _sys.argv[1] if len(_sys.argv) > 1 else os.path.join(T, "kicad", "expansion-board-v3.kicad_pcb")
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "PLACEMENT.py")

# 几何有硬约束，不参与吸附：J1 对扣排针、天线、射频口钉死件、USB-C 开口
FROZEN = {"J1", "ANT1", "L8", "D3", "C39", "J5", "J4"}
DY, GAP = 0.35, 12.0

# 用户 2026-08-02 明确指定的额外对齐（差 0.6mm，超出自动阈值，需人工判断）：
# 右侧 L14/C37/C38/R30 那一行下拉，与左侧 Q5/F5/L15/R26 对齐成一整行。
# 用户已在 KiCad 里手拉到 66.500，但 Q5 行是 66.553——**手拉差了 0.053mm**，
# 正说明肉眼对不准，必须靠吸附。这里以已对齐的多数（Q5 行）为准。
_Q5_ROW_Y = 66.553
# ⚠️ 这一行的成员会随布局调整而流失，**摘除记录必须留痕**——否则下次有人看到
# 断言报错，会以为是对齐算法坏了，实际上是元件被有意挪走了。
#   · L14  2026-08-10 人工挪出（(99.97,66.55) → (120.42,63.27)，20.71mm）
#   · C37  2026-08-13 挪去做 U14.4 的去耦（现离 U14 5.31mm，离本行 18.43mm）
#   · C38  2026-08-13 挪去做 U13.7 的去耦（现离 U13 3.89mm，离本行 2.69mm）
#          ——U13.7 原本要 29.8mm 外才有去耦电容，那个量级等于没有
# 旧的行对齐规则会把它们硬拉回 y=66.553，C37 位移 18.429mm 直接撞上下面的保护断言。
# **新的布局调整优先于旧的对齐意图。** 只剩 R30 还在这一行，单个成员没有"对齐"
# 可言，但留着无害（它本来就在 66.60，位移 0.05mm）。
# 2026-08-13 清空：这张表原本有 C37/C38/R30/L14 四个成员，前三个都因布局调整
# 被摘除（见上），只剩 R30。而 R30 实际在 66.600、离本行 66.553 只差 0.047mm——
# 单个成员没有"对齐"可言，却成了 rebuild 复现不一致的**唯一**来源
# （PLACEMENT.py 写 66.553、板上是 66.600）。
# 固化工具的第一要务是忠实，留着这 0.047mm 的"美化"不值。
MANUAL_ROW = {}
# 用户明确要求不动：J8/J2(54.500) 与 U17/C57/C58/C59(55.100) 保持 0.6mm 差——
# 两者本体高 5.09 vs 2.29mm，中心对齐反而两端不齐。

SILK_SNAP = os.environ.get("PK_SILK_SNAP", "") == "1"
# ⚠️ **默认忠实导出，不做任何"美化"**。
# 这个脚本的用途是"固化人工精修的结果"，那就一个字节都不该改。
# 下面几条规则（F.Fab/压本体的位号不冻结、元件行吸附、网格吸附）都是为
# **自动排版结果**设计的——不把算法的失败固化下来，有它的道理。
# 但 2026-08-13 手工重排 82 个位号之后，它们就成了误伤：
#     U13  位号落在自己 courtyard 内 → 被"压本体不冻结"跳过，手排位置直接丢
#     C43  元件 y 78.35 → 78.00，行吸附改了 0.35mm
#     R30  位号 y 65.40 → 65.353
# 结果 rebuild 复现出来对不上原板。**固化工具改数据 = 固化失效。**
# 要用那几条规则时显式打开（一般只在自动排版之后、人工介入之前）。
FILTER_SILK = os.environ.get("PK_FILTER_SILK", "") == "1"
SNAP_POS = os.environ.get("PK_SNAP_POS", "") == "1"

board = pcbnew.LoadBoard(PCB)
FPS = {f.GetReference(): f for f in board.GetFootprints()}
mm = pcbnew.ToMM

pos = {r: (mm(f.GetPosition().x), mm(f.GetPosition().y))
       for r, f in FPS.items() if not r.startswith("H")}

# ---------------- 行吸附 ----------------
cand = {r: v for r, v in pos.items() if r not in FROZEN}
par = {r: r for r in cand}


def find(a):
    while par[a] != a:
        par[a] = par[par[a]]
        a = par[a]
    return a


order = sorted(cand, key=lambda r: (cand[r][1], cand[r][0]))
for i, a in enumerate(order):
    for c in order[i + 1:]:
        if cand[c][1] - cand[a][1] > DY:
            break
        if abs(cand[a][0] - cand[c][0]) <= GAP:
            par[find(a)] = find(c)

grp = collections.defaultdict(list)
for r in cand:
    grp[find(r)].append(r)

snap = {}
for members in (grp.values() if SNAP_POS else ()):
    ys = [cand[r][1] for r in members]
    if len(members) < 2 or max(ys) - min(ys) < 0.002:
        continue
    tgt = collections.Counter(round(y, 3) for y in ys).most_common(1)[0][0]
    for r in members:
        if abs(cand[r][1] - tgt) > 0.002:
            snap[r] = tgt
snap.update(MANUAL_ROW)

assert not (set(snap) & FROZEN), f"钉死件被吸附了：{sorted(set(snap) & FROZEN)}"
if snap:      # MANUAL_ROW 清空 + 行吸附默认关闭之后，这里正常就是空的
    worst = max((abs(pos[r][1] - y), r) for r, y in snap.items())
    assert worst[0] < 1.0, f"{worst[1]} 位移 {worst[0]:.3f}mm 过大，规则有问题"
    print(f"行吸附 {len(snap)} 个元件，最大位移 {worst[0]:.3f}mm（{worst[1]}）")

# ---------------- 位号丝印行吸附 ----------------
# 元件对齐了位号也要对齐，否则一行位号高低差零点几毫米，出图一眼看得出参差。
# 判据必须同时满足三条，缺一即误判（三次试错换来的）：
#   ① 同一行（y 簇）   ② **同样本体高度**   ③ 同一侧（都在上方或都在下方）
# 高度不同的元件位号各自贴自己本体上沿，绝对 y 本就不同，强行拉平会压到本体上
# （U4 高 6.3mm，与旁边 0603 的位号不可能同一条线）。


def _bh(f):
    c = f.GetCourtyard(pcbnew.F_CrtYd).BBox()
    return round(mm(c.GetHeight()), 2) if c.GetHeight() else 0.0


silk_snap = {}
# ⚠️ 位号吸附**默认关闭**（PK_SILK_SNAP=1 才启用）。
#
# 这是 export_placement 自己的第二套位号排版规则（把同行同侧同高的位号拉到众数 y），
# 但位号排版现在由 gen_silk.py 接管——它有完整的"压本体/压焊盘/压文字"三项检查
# 和自己的行对齐 pass。两套规则同时生效就是打架。
#
# 更要命的是它会**覆盖手工调整**：2026-08-13 手工重排了 82 个位号，
# 这套规则一跑就要吸附 18 个、最大位移 2.700mm(C1)——等于把人的活儿改掉。
# 同一个教训在 MANUAL_ROW 那里已经栽过一次（C37 位移 18.429mm）：
# **新的手工调整优先于旧的自动对齐意图**。


for members in (grp.values() if SILK_SNAP else ()):
    if len(members) < 2:
        continue
    buckets = collections.defaultdict(list)
    for r in members:
        f = FPS[r]
        if board.GetLayerName(f.Reference().GetLayer()) != "F.Silkscreen":
            continue
        ry = mm(f.Reference().GetPosition().y)
        by = snap.get(r, mm(f.GetPosition().y))
        buckets[(_bh(f), 1 if ry > by else -1)].append((r, ry))
    for items in buckets.values():
        if len(items) < 2:
            continue
        # 候选目标按出现次数排序，逐个试；**位号压在自己本体上的候选一律弃用**。
        # 不加这条会传染坏位置：Q4 的位号本来就压在自己身上（早前某轮的遗留），
        # 被当成众数后把同行的 Q3 也拉了过去（实测位移 2.756mm）。
        for tgt, _ in collections.Counter(round(y, 3) for _, y in items).most_common():
            bad = False
            for r, _y in items:
                c = FPS[r].GetCourtyard(pcbnew.F_CrtYd).BBox()
                if mm(c.GetTop()) - 0.3 < tgt < mm(c.GetBottom()) + 0.3:
                    bad = True
                    break
            if not bad:
                for r, y in items:
                    if abs(y - tgt) > 0.002:
                        silk_snap[r] = tgt
                break
if silk_snap:
    _w = max((abs(mm(FPS[r].Reference().GetPosition().y) - y), r)
             for r, y in silk_snap.items())
    print(f"位号吸附 {len(silk_snap)} 个，最大位移 {_w[0]:.3f}mm（{_w[1]}）")

# ---------------- 网格吸附 ----------------
# 人工在 KiCad 里拖出来的是"大概合理的位置"，精度谈不上——实测 152 个元件里只有
# 23 个落在 0.05mm 网格上，其余全是 87.156 / 78.336 这种装箱器算出的任意小数。
# 吸到网格纯粹是为了整齐好看，位移上限只有 GRID/2，不影响任何电气或几何约束。
#
# 顺序很重要：**先行对齐、后网格吸附**。行对齐把同簇的 y 统一成一个值，那个值再
# 一起吸到网格，行内仍然齐；反过来做的话，各自吸各自的网格会把刚对齐的行又打散。
#
# FROZEN 不参与：J1 对扣排针的孔位要跟主板逐脚镜像对齐，射频钉死件的几何是调过的，
# 挪 0.05mm 也得重新验证——这些位置是设计本身，不是"摆得好不好看"的问题。
GRID_SNAP = float(os.environ.get("PK_GRID_SNAP", "0.1"))


def _grid(v):
    return round(round(v / GRID_SNAP) * GRID_SNAP, 3)


# ---------------- 写 PLACEMENT.py ----------------
rows, silk, n_skip = [], [], [0]
_gs_n, _gs_max = 0, 0.0
for r in sorted(FPS):
    f = FPS[r]
    x, y = mm(f.GetPosition().x), mm(f.GetPosition().y)
    if r in snap:
        y = snap[r]
    if SNAP_POS and r not in FROZEN and not r.startswith("H"):
        gx, gy = _grid(x), _grid(y)
        d = max(abs(gx - x), abs(gy - y))
        if d > 1e-6:
            _gs_n += 1
            _gs_max = max(_gs_max, d)
        x, y = gx, gy
    rows.append((r, round(x, 3), round(y, 3), round(f.GetOrientationDegrees()) % 360))
    ref = f.Reference()
    # **只冻结成功的位号**。F.Fab 表示"当时排不下"——那是算法状态，不是设计意图。
    # 冻结它等于把失败也固化：版面后来改了、周围腾出空间了，也永远不再重试。
    # 实测 C62/C16/C17 就是这样——它们的 F.Fab 来自版面精修**之前**那一轮，
    # 精修后其实放得下（放开重试立刻 156/156 全部有位号）。
    if board.GetLayerName(ref.GetLayer()) not in ("F.Silkscreen", "B.Silkscreen"):
        n_skip[0] += 1
        continue
    # 位号压在自己本体上的也不冻结——那同样是缺陷不是设计意图（同 F.Fab 的道理）。
    # 实测 Q3/Q4 就是这样，冻结下来会永远排不对。大件除外（排针位号压本体是正常的）。
    _c = f.GetCourtyard(pcbnew.F_CrtYd).BBox()
    if FILTER_SILK and _c.GetHeight() and mm(_c.GetWidth()) <= 8 and mm(_c.GetHeight()) <= 8 \
            and mm(_c.GetTop()) < mm(ref.GetPosition().y) < mm(_c.GetBottom()) \
            and mm(_c.GetLeft()) < mm(ref.GetPosition().x) < mm(_c.GetRight()):
        n_skip[0] += 1
        continue
    rx, ry = mm(ref.GetPosition().x), mm(ref.GetPosition().y)
    if r in snap:                      # 位号跟着元件一起挪，保持相对关系
        ry += snap[r] - mm(f.GetPosition().y)
    if r in silk_snap:                 # 行内位号对齐
        ry = silk_snap[r]
    silk.append((r, round(rx, 3), round(ry, 3), round(ref.GetTextAngleDegrees()) % 360,
                 board.GetLayerName(ref.GetLayer())))

print(f"网格吸附 {_gs_n} 个元件到 {GRID_SNAP}mm 网格，最大位移 {_gs_max:.3f}mm")
assert _gs_max <= GRID_SNAP / 2 + 1e-6, "网格吸附位移超过 GRID/2，逻辑有误"

with open(OUT, "w") as fh:
    fh.write('''"""元件布局与位号丝印坐标——**本文件是设计数据，不是生成物**。

由 export_placement.py 从人工精修后的 PCB 导出（含行对齐吸附），
gen_pcb.py / gen_silk.py 读它摆位。手工调整版面后重新跑一次 export_placement.py 即可固化。

⚠️ 直接改这里的数字也可以，但改完要跑 gen_pcb.py 让断言校验（对扣位置、区域互斥、板边余量…）。
"""

# (位号, x, y, 旋转角)  单位 mm，KiCad 坐标系
PLACEMENT = [
''')
    for t in rows:
        fh.write(f"    ({t[0]!r:8s}, {t[1]:7.3f}, {t[2]:7.3f}, {t[3]:3d}),\n")
    fh.write(''']

# (位号, 位号丝印 x, y, 旋转角, 所在层)
SILK_REF = [
''')
    for t in silk:
        fh.write(f"    ({t[0]!r:8s}, {t[1]:7.3f}, {t[2]:7.3f}, {t[3]:3d}, {t[4]!r}),\n")
    fh.write("]\n")

print(f"OK: {OUT}  ({len(rows)} 元件 / {len(silk)} 位号"
      f"{f'，{n_skip[0]} 个未冻结留待重排' if n_skip[0] else ''})")
print("注意：本脚本只读 PCB，未修改它。")
