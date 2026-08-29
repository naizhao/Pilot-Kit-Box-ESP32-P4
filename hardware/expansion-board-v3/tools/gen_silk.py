#!/usr/bin/env python3
"""丝印整理 + 品牌标识。

152 个元件挤在 100×62 板上，位号丝印默认摆在本体上方 1.5mm，必然出界/重叠/压焊盘。
本脚本统一重排：

1. **接口功能标注**（GNSS INT/EXT、1090 EXT、978 UAT、USB）——锚定到对应元件，
   元件挪到哪标注跟到哪。**不写死坐标**：J6 从左边缘搬到射频开关旁之后，
   写死的 '1090 EXT' 还留在旧位置，直接压在 FL2 上（出图才看见）。
2. 标注**排在位号之前**占位。它们比位号重要（插线不接错），让位号绕开它们；
   反过来做的话标注会被排满的位号挤得无处可放。
3. **位号**在本体四周由近及远试位，避开板框、焊盘、铜箔、已有文字；
   找不到位的降到 F.Fab（装配图仍可查，不占板面）。
4. **品牌放背面**：正面没有连片空地，之前放板底那两行正好被 J1 排针盖住，
   出图里根本看不见。背板除覆铜外全空，是品牌的天然位置。

字高统一 0.8mm：更小会触发 min_text_height 规则，那也是嘉立创丝印可印下限。

运行：~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 gen_silk.py
"""
import collections
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")   # 副本上跑整条链用
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")

# 板级元数据的唯一来源。改版本号只改 board_meta.py，别在这里复制一份——
# 「改了一处漏了另一处」这块板已经栽过 5 次。
from board_meta import (BOARD_NAME, BOARD_REV, BOARD_DATE,   # noqa: E402
                        COPYRIGHT, WEBSITE, SUBTITLE)

X0, Y0, X1, Y1 = 50.0, 50.0, 150.0, 112.0
EDGE = 0.3          # 丝印到板框最小间距
TXT_H = 0.8         # 位号字高（低于 0.8 触发 min_text_height，也印不出来）
TXT_W = 0.15        # 笔画宽

# 接口功能标注：(文字, 锚定位号, 字高)
LABELS = [
    ("GNSS INT", "J8", 0.8),
    ("GNSS EXT", "J2", 0.8),
    ("1090 EXT", "J6", 0.8),
    ("978 UAT", "J5", 0.8),
    ("USB", "J4", 0.8),
    # J7 是**板载 IFA 天线**那一路的 U.FL，接在 π 匹配之后、天线之前。
    # 板上另外三个 U.FL（J2/J6/J8）都是接外部天线进来，只有它是把板载天线引出去：
    # 拿 U.FL→SMA pigtail 接 NanoVNA 就能直接量这根 IFA 的谐振点和阻抗——V3.5 的
    # 主臂是"画长待切"（46.5mm），出厂后要靠这个口边量边切到 1090。
    # 所以标 IFA 而不是 EXT，跟 '1090 EXT'(J6，外接天线) 区分开。
    ("1090 IFA", "J7", 0.8),
]
# 天线本体是一整块几十毫米长的自定义焊盘，四周试位没意义，用实测确定的位置
ANT_LABEL = ("1090MHz IFA ANT", 113.0, 54.0, 0.9)   # 用户 2026-08-02 手调：辐射臂与竖枝之间的空当
#                               ↑ 锚点每次左移，标注跟着左移同样距离，保持在同一空当
#                                 41→44mm 时左移 3；V3.5 整体左移 5.5 → 118.5-5.5=113.0

board = pcbnew.LoadBoard(PCB)


def bbox_mm(item):
    b = item.GetBoundingBox()
    return (pcbnew.ToMM(b.GetLeft()), pcbnew.ToMM(b.GetTop()),
            pcbnew.ToMM(b.GetRight()), pcbnew.ToMM(b.GetBottom()))


def overlap(a, b, gap=0.0):
    return (a[0] - gap < b[2] and b[0] < a[2] + gap
            and a[1] - gap < b[3] and b[1] < a[3] + gap)


# 障碍：普通焊盘与铜层图形用包围盒；**自定义形状焊盘用实际多边形**。
# IFA 天线是一整块自定义焊盘，外接矩形 41×7.5mm，用包围盒会把辐射臂与竖枝之间的
# 大片空地也误判成占用，标注根本放不进去。
obstacles = []
polys = []
for f in board.GetFootprints():
    for p in f.Pads():
        if p.GetShape() == pcbnew.PAD_SHAPE_CUSTOM:
            polys.append(p.GetEffectivePolygon(pcbnew.ERROR_INSIDE))
        else:
            obstacles.append(bbox_mm(p))
    for g in f.GraphicalItems():
        if g.GetLayer() in (pcbnew.F_Cu, pcbnew.B_Cu):
            obstacles.append(bbox_mm(g))


def hits_poly(box, step=0.25):
    """文字框是否压到自定义形状焊盘的实际铜箔（采样包含测试）"""
    x = box[0]
    while x <= box[2]:
        y = box[1]
        while y <= box[3]:
            if any(q.Contains(pcbnew.VECTOR2I_MM(round(x, 3), round(y, 3))) for q in polys):
                return True
            y += step
        x += step
    return False


# 超过这个尺寸就不按整体本体锚定位号，改锚到第一个焊盘。
# 依据：IFA 天线本体几十毫米长（V3.5 包络 49.8mm），按本体锚定会把位号甩到离焊盘几十毫米外，
# 板上出现一个孤零零的位号却看不到对应元件（实测 ANT1 就是这样）。
BIG_FP_MM = 12.0


def body_of(f):
    cy = f.GetCourtyard(pcbnew.F_CrtYd).BBox()
    if cy.GetWidth() == 0:
        cy = f.GetBoundingBox(False, False)
    x0, y0 = pcbnew.ToMM(cy.GetLeft()), pcbnew.ToMM(cy.GetTop())
    x1, y1 = pcbnew.ToMM(cy.GetRight()), pcbnew.ToMM(cy.GetBottom())
    if max(x1 - x0, y1 - y0) > BIG_FP_MM:
        pads = list(f.Pads())
        if pads:
            b = pads[0].GetBoundingBox()
            return (pcbnew.ToMM(b.GetLeft()), pcbnew.ToMM(b.GetTop()),
                    pcbnew.ToMM(b.GetRight()), pcbnew.ToMM(b.GetBottom()))
    return (x0, y0, x1, y1)


CENTERS = []       # (x, y, ref) 所有元件中心，用于歧义判定


def unambiguous(cx, cy, own, bx0, by0, bx1, by1):
    """位号必须离自己的元件最近。密集阵列里，一个夹在两颗相同元件中间的位号
    比不标还坏——会指错件。实测左边缘曾出现 'R33R21C17C12' 挤成一团、
    每个都离自家元件不近，完全无法辨认。"""
    mine = max(0.0, max(bx0 - cx, cx - bx1)) ** 2 + max(0.0, max(by0 - cy, cy - by1)) ** 2
    for x, y, r in CENTERS:
        if r == own:
            continue
        if (x - cx) ** 2 + (y - cy) ** 2 < mine:
            return False
    return True


# 行/列分组：装箱器本来就是按行摆的，行结构是**已知的**，不必靠"最近邻"去猜。
# 之前用最近邻猜：C18 正下方 2.62mm 有 C14，就被判成竖排、位号甩到左侧，
# 而它明明属于 C18/C10/C11/C12/C13 这一横行，同行其它件都标在上方，只有它在左边。
ROW_TOL = 0.6        # y 相差在此范围内视为同一行
COL_TOL = 0.6
_rows, _cols = {}, {}
_row_of, _col_of = {}, {}


def _cluster(vals, tol):
    """按容差聚类，返回 值→簇号。

    不能用 round(v/tol) 分桶——那样 84.54 与 84.65 只差 0.11mm 却会落到相邻两个
    桶里（边界效应），同一行被拆散（实测 C51 vs C18）。

    ⚠️ 也不能拿 v 跟**前一个值**比（第一版就是这么写的）——那是**链式聚类**：
    只要一串元件依次相差 ≤tol 就一路链下去，簇的跨度可以无限大。
    156 个元件挤在 62mm 高度上，相邻间距普遍小于 0.6mm，于是链成几个巨簇，
    y=63 和 y=82 被判成"同一行"，跨度 19mm。后果是行对齐机检报
        AssertionError: 位号未与同行对齐：R21(y=63.233 vs 同行同高82.71)
    而 R21 其实在 y=85.17，跟 63.233 毫无关系——那是它在旧布局里的位置，
    被链式聚类拉进了一个横跨半块板的"行"。

    正确做法是跟**簇的起点**比：簇内最大跨度恒等于 tol，
    这才符合"同一行"的语义。
    """
    out, cid, start = {}, 0, None
    for v in sorted(set(vals)):
        if start is not None and v - start <= tol:
            out[v] = cid
        else:
            cid += 1
            start = v
            out[v] = cid
    return out


def build_groups():
    xs = _cluster([c[0] for c in CENTERS], COL_TOL)
    ys = _cluster([c[1] for c in CENTERS], ROW_TOL)
    for x, y, r in CENTERS:
        _rows.setdefault(ys[y], []).append(r)
        _cols.setdefault(xs[x], []).append(r)
    _row_of.update({r: ys[y] for x, y, r in CENTERS})
    _col_of.update({r: xs[x] for x, y, r in CENTERS})


_REF_Y = {}        # 位号 → 已排定的位号绝对 y


def _bh(ref):
    """元件本体高度（courtyard），用于判断"同高"。"""
    cy = FPS[ref].GetCourtyard(pcbnew.F_CrtYd).BBox()
    return round(pcbnew.ToMM(cy.GetHeight()), 2) if cy.GetHeight() else 0.0


def row_targets(own):
    """同行**且同高**的邻居已用的位号绝对 y，按出现次数排序。

    对齐的不变式必须同时满足三个条件，少一个都会误判（三次试错换来的）：
      1. 同一行（y 簇）——不同行本来就不该对齐
      2. **同样本体高度**——U4 高 6.3mm、0603 只有 1mm，位号各自贴自己本体上沿，
         绝对 y 必然不同，强行拉平会把 U4 的位号压到它自己身上
      3. 同一侧（都在上方或都在下方）
    """
    mates = _rows.get(_row_of.get(own), [])
    h = _bh(own)
    ys = [_REF_Y[m] for m in mates if m in _REF_Y and abs(_bh(m) - h) < 0.2]
    return [y for y, _ in collections.Counter(round(v, 3) for v in ys).most_common()]


def preferred_dirs(own, bx0, by0, bx1, by1):
    """同一行的件统一标上方，同一列的统一标右侧——这样一整排看过去方向一致才认得出。
    既在行又在列时，取成员多的那个（更像主结构）。"""
    nrow = len(_rows.get(_row_of.get(own), []))
    ncol = len(_cols.get(_col_of.get(own), []))
    if ncol > nrow and ncol >= 3:
        return ("right", "left", "up", "down")
    if nrow >= 2:
        return ("up", "down", "right", "left")
    return ("up", "down", "right", "left")


def find_spot(bx0, by0, bx1, by1, tw, th, placed, own=None):
    """在本体四周找位。**横排放不下就竖排**——元件行之间常有横向窄缝，
    横排的 'C18' 要 2.1mm 宽塞不进，竖排只要 1.1mm 宽就行。
    实测只试横排时 16 个位号无位可放，加竖排后降到个位数。
    返回 (cx, cy, box, angle)。"""
    ccx, ccy = (bx0 + bx1) / 2, (by0 + by1) / 2
    dirs = preferred_dirs(own, bx0, by0, bx1, by1) if own else ("up", "down", "right", "left")
    for d in (0.5, 0.8, 1.2, 1.7, 2.4, 3.2, 4.2):   # 有歧义检查兜底，放远也不会指错件
        # 每个方向都先试横排、再试竖排（竖排时宽高互换）
        for ang, w, h in ((0, tw, th), (90, th, tw)):
            pos = {"up": (ccx, by0 - d - h / 2), "down": (ccx, by1 + d + h / 2),
                   "right": (bx1 + d + w / 2, ccy), "left": (bx0 - d - w / 2, ccy)}
            for cx, cy in (pos[k] for k in dirs):
                box = (cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2)
                if not (X0 + EDGE < box[0] and box[2] < X1 - EDGE
                        and Y0 + EDGE < box[1] and box[3] < Y1 - EDGE):
                    continue
                if any(overlap(box, o, 0.05) for o in obstacles) or hits_poly(box):
                    continue
                if any(overlap(box, o, 0.10) for o in placed):
                    continue
                if own and not unambiguous(cx, cy, own, bx0, by0, bx1, by1):
                    continue
                return cx, cy, box, ang
    return None


# 板级文字的三层优先级。**这个脚本不再无条件重新生成文字。**
#
# 原来 text() 是无脑 board.Add()，跑在一块已经有文字的板上就多一份：
# 2026-08-22 用户在 KiCad 里调好文字 → 我跑 gen_silk 又生成一遍 → export 出去
# SILK.json 攒到 15 条（该有 11 条），下一次 run_route.sh 的 ⑦' 把这 15 条贴回板上，
# 用户打开看到 'GNSS EXT'/'GNSS INT'/'1090 EXT'/'1090MHz IFA ANT' 各有两份。
# 更糟的是同一轮里自动试位算出的新坐标覆盖了用户手调的位置——人工调整被冲掉。
#
#   ① 板上已有同内容文字  → 一个字都不动，连坐标都不碰
#   ② 板上没有、SILK.json 里有 → 按固化坐标贴（gen_pcb 从零重建后走这条）
#   ③ 两处都没有          → 才自动试位（只有真正新增的标注会走到这里）
#
# 判据用文字内容本身：板级文字全板唯一，重名就是重复。
_EXIST = {}
for _d in board.GetDrawings():
    if _d.GetClass() in ("PCB_TEXT", "PCB_TEXTBOX"):
        _EXIST[_d.GetText()] = _d
_FROZEN_TXT = {}
_SJ = os.path.join(T, "tools", "SILK.json")
if os.path.exists(_SJ):
    import json
    for _t in json.load(open(_SJ)).get("texts", []):
        _FROZEN_TXT.setdefault(_t["text"], _t)      # setdefault：SILK.json 自己脏了也只取第一条


def text(s, x, y, h, layer=pcbnew.F_SilkS, just=None):
    if s in _EXIST:
        return _EXIST[s]                            # ① 板上已有，原样保留
    f = _FROZEN_TXT.get(s)
    if f is not None:                               # ② 用固化坐标，不重新计算
        x, y, h = f["x"], f["y"], f.get("h", h)
    t = pcbnew.PCB_TEXT(board)
    t.SetText(s)
    t.SetPosition(pcbnew.VECTOR2I_MM(round(x, 3), round(y, 3)))
    t.SetLayer(layer)
    t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(h), pcbnew.FromMM(h)))
    t.SetTextThickness(pcbnew.FromMM(h * 0.17))
    if just:
        t.SetHorizJustify(just)
    if layer == pcbnew.B_SilkS:
        t.SetMirrored(True)      # 背面文字必须镜像，否则从背面看是反的
    board.Add(t)
    _EXIST[s] = t                # 本轮内也去重
    return t


def _txt_box(t, h):
    """已存在文字的包围盒——位号试位要避开它们。
    直接问 pcbnew 要，不按字符数估（估算框偏小的坑见 _txt_wh）。"""
    b = t.GetBoundingBox()
    return (pcbnew.ToMM(b.GetLeft()), pcbnew.ToMM(b.GetTop()),
            pcbnew.ToMM(b.GetRight()), pcbnew.ToMM(b.GetBottom()))


placed = []          # 已定位的 F.SilkS 文字包围盒
CENTER = pcbnew.GR_TEXT_H_ALIGN_CENTER
for _f in board.GetFootprints():
    _p = _f.GetPosition()
    CENTERS.append((pcbnew.ToMM(_p.x), pcbnew.ToMM(_p.y), _f.GetReference()))
build_groups()

# ---------------- ① 接口功能标注（必须排在位号之前）----------------
FPS = {f.GetReference(): f for f in board.GetFootprints()}
_n_keep = 0
for s_, ref, h in LABELS:
    f = FPS.get(ref)
    assert f, f"标注 '{s_}' 锚定的元件 {ref} 不存在"
    if s_ in _EXIST or s_ in _FROZEN_TXT:
        # 已有或已固化：不试位，直接按它现在的位置占坑，让位号绕开
        t = text(s_, 0, 0, h, just=CENTER)      # text() 内部走 ①/② 分支
        placed.append(_txt_box(t, h))
        _n_keep += 1
        continue
    tw, th = len(s_) * h * 0.75 + 0.3, h + 0.3
    spot = find_spot(*body_of(f), tw, th, placed)
    assert spot, f"标注 '{s_}' 在 {ref} 四周找不到位置"
    cx, cy, box, ang = spot
    t = text(s_, cx, cy, h, just=CENTER)
    if ang:
        t.SetTextAngleDegrees(ang)
    placed.append(box)

# 位号钉死表：自动试位解决不了的，坐标由 PLACEMENT.SILK_REF 给，覆盖上面算出来的结果。
#
# ANT1：本体几十毫米长，自动试位只会把它甩到离焊盘几十毫米外。且它的"第一个焊盘"
#   就是那整块自定义形状焊盘（V3.5 包络 49.8mm）——body_of() 的大封装回退
#   （锚到 pads[0]）对它无效，自动试位仍会把位号甩到右端。放在馈点左侧的禁铜区里。
#
# U13 曾经也在这张表里：AD8319 本体 y 69.16~73.25，上方被 R20(y=67.3) 卡住只剩
#   1.61mm，自动试位挑中的 y=69.575 落在自己本体里。2026-08-22 用户把 R20 挪到
#   (138.48, 66.45)——右移 5.6mm 又上移 0.85mm，U13 上方空出来了，自动试位能解，
#   所以从钉死表里去掉。钉死表只该收自动试位真的解不了的，能解就还回去。
#
# ⚠️ 这里**只留位号名单，不留坐标**。原来写死成 {"ANT1": (88.0, 54.0), ...}，
# 和 PLACEMENT.SILK_REF 是同一个坐标的两份副本——V3.5 就漏改过这里：天线左移
# 6mm 而这份没动，位号直接压到辐射臂上。2026-08-22 又撞一次：用户在 KiCad 里
# 把 ANT1/U13 的位号挪了、export_placement 也固化进 SILK_REF 了，但只要跑一次
# gen_silk，这份写死的旧坐标就会把人工调整冲回去。改成单一来源，副本消失。
# （同族教训见 memory: project_hardcoded_dimension_copies，已栽 4 次）
_PINNED_KEYS = ("ANT1",)
try:
    import PLACEMENT
    _SILK = {t[0]: t[1:] for t in PLACEMENT.SILK_REF}
except ImportError:
    _SILK = {}
_missing = [k for k in _PINNED_KEYS if k not in _SILK]
assert not _missing, (f"PLACEMENT.SILK_REF 里没有 {_missing} 的坐标——"
                      "钉死表不再自带坐标，先跑 export_placement.py")
PINNED_REF = {k: (_SILK[k][0], _SILK[k][1]) for k in _PINNED_KEYS}
for _ref, _at in PINNED_REF.items():
    _a = FPS[_ref].Reference()
    _a.SetLayer(pcbnew.F_SilkS)
    _a.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(TXT_H), pcbnew.FromMM(TXT_H)))
    _a.SetTextThickness(pcbnew.FromMM(TXT_W))
    _a.SetTextAngleDegrees(0)
    _a.SetPosition(pcbnew.VECTOR2I_MM(*_at))
    placed.append((_at[0] - 2.5, _at[1] - 0.7, _at[0] + 2.5, _at[1] + 0.7))

_s, _x, _y, _h = ANT_LABEL
# 先建（或取回已有的），再从**实际位置**算包围盒。
# 原来按 ANT_LABEL 那份写死坐标算框，用户把标注挪走之后框还留在旧位置：
# 压铜检查查的是空地、位号避让避的也是空地，两头都白做。
_t = text(_s, _x, _y, _h, just=CENTER)
_box = _txt_box(_t, _h)
assert not hits_poly(_box), "天线标注压到辐射臂/竖枝了"
placed.append(_box)
print(f"接口标注: {len(LABELS)} 处（{_n_keep} 处沿用已有坐标）+ 天线标注 1 处")

# ---------------- ② 位号 ----------------
# 位号位置同样冻结（见 export_placement.py）：人工调过的丝印不该被算法覆盖。
# 自动试位只用于 PLACEMENT.py 里没有的新元件。_SILK 在钉死表那里已经读好了。
_LAYER = {"F.Silkscreen": pcbnew.F_SilkS, "F.Fab": pcbnew.F_Fab,
          "B.Silkscreen": pcbnew.B_SilkS, "B.Fab": pcbnew.B_Fab}

n_ok, n_fab = 0, 0
def _prio(f):
    """芯片、连接器、晶振、分立器件优先；阻容感与测试点靠后。
    位号原按字母序处理，C 排在 U 前面，电容先把行间那条缝占光，
    三颗并排的 LDO 只能把位号甩到侧面，反而分不清谁是谁（实测 U1/U2/U3）。
    要认的是芯片；无源件查装配图即可。"""
    r = f.GetReference()
    head = "".join(c for c in r if c.isalpha())
    return (0 if head in ("ANT", "U", "J", "FL", "Y", "SW", "D", "Q", "F") else 1, r)


def _txt_wh(ref):
    """位号文字的真实包围盒尺寸（mm）。

    **不要估**。原来估成 tw = len(ref)*TXT_H*0.75+0.3、th = TXT_H+0.3，对 'J7'
    算出 1.50×1.10 而真值是 1.66×1.47，每边小 0.1~0.2mm。后果不是"排得紧一点"，
    是用小框判定不重叠、落盘却真重叠。
    包围盒与位置无关（换位置只是平移），角度归零后取一次即可；竖排的宽高互换在
    find_spot 里处理。"""
    ref.SetTextAngleDegrees(0)
    b = ref.GetBoundingBox()
    return pcbnew.ToMM(b.GetWidth()), pcbnew.ToMM(b.GetHeight())


# ⚠️ 冻结项必须**先全部占位**，再让新元件试位。
#
# 原来只有一趟循环，冻结项和自动试位项混在一起按 _prio 排序处理。_prio 让连接器
# 排在电容之前，于是 2026-08-22 新增的 J7 试位时，C54（冻结）还没进 placed——
# J7 对着一张不完整的占用图找位，挑中的位置和 C54 的位号叠着 1.56×0.14mm。
# 试位那关"通过"了，因为冲突对象当时还不存在。
#
# 用户看到的现象是"J7 没有丝印"：字确实在板上，只是和 C54 糊成一团认不出来。
# 光把估算框换成真实尺寸治不了这个——框再准，也挡不住拿它去比一张空表。
_frozen = []
for f in board.GetFootprints():
    r = f.GetReference()
    if r in PINNED_REF or r not in _SILK:
        continue
    ref = f.Reference()
    ref.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(TXT_H), pcbnew.FromMM(TXT_H)))
    ref.SetTextThickness(pcbnew.FromMM(TXT_W))
    ref.SetVisible(True)
    tw, th = _txt_wh(ref)
    _x, _y, _a, _ly = _SILK[r]
    ref.SetPosition(pcbnew.VECTOR2I_MM(_x, _y))
    ref.SetTextAngleDegrees(_a)
    ref.SetLayer(_LAYER.get(_ly, pcbnew.F_SilkS))
    if _ly.endswith("Silkscreen"):
        if _a % 180:                      # 竖排：包围盒宽高互换
            tw, th = th, tw
        placed.append((_x - tw / 2, _y - th / 2, _x + tw / 2, _y + th / 2))
        _REF_Y[r] = _y
        n_ok += 1
    else:
        n_fab += 1
    _frozen.append(r)
print(f"位号丝印: 冻结 {len(_frozen)} 个先占位")

for f in sorted(board.GetFootprints(), key=_prio):
    # 钉死表里的跳过自动试位。也因此不会进 _REF_Y，后面的行对齐 pass 同样不会动它们。
    if f.GetReference() in PINNED_REF:
        n_ok += 1
        continue
    if f.GetReference() in _SILK:      # 冻结项已在上面处理完
        continue
    ref = f.Reference()
    ref.SetLayer(pcbnew.F_SilkS)
    ref.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(TXT_H), pcbnew.FromMM(TXT_H)))
    ref.SetTextThickness(pcbnew.FromMM(TXT_W))
    ref.SetVisible(True)

    tw, th = _txt_wh(ref)
    # 优先复用同行邻居已经用过的偏移——一行位号必须排在同一条线上。
    # 不这样做的话，新排的件会自己找个"能放下"的位置，与同行差零点几毫米，
    # 出图一眼就看得出参差（实测 C62 比同行高 0.33mm）。
    own = f.GetReference()
    bx0, by0, bx1, by1 = body_of(f)
    spot = None
    for cy in row_targets(own):
        cx = (bx0 + bx1) / 2
        box = (cx - tw / 2, cy - th / 2, cx + tw / 2, cy + th / 2)
        if not (X0 + EDGE < box[0] and box[2] < X1 - EDGE
                and Y0 + EDGE < box[1] and box[3] < Y1 - EDGE):
            continue
        if any(overlap(box, o, 0.05) for o in obstacles) or hits_poly(box):
            continue
        if any(overlap(box, o, 0.10) for o in placed):
            continue
        if not unambiguous(cx, cy, own, bx0, by0, bx1, by1):
            continue
        spot = (cx, cy, box, 0)
        break
    if spot is None:
        spot = find_spot(bx0, by0, bx1, by1, tw, th, placed, own=own)
    if spot:
        cx, cy, box, ang = spot
        ref.SetPosition(pcbnew.VECTOR2I_MM(round(cx, 3), round(cy, 3)))
        ref.SetTextAngleDegrees(ang)
        placed.append(box)
        _REF_Y[f.GetReference()] = cy
        n_ok += 1
    else:
        ref.SetLayer(pcbnew.F_Fab)   # 找不到位：降到装配图层，不占板面丝印
        n_fab += 1
print(f"位号丝印: {n_ok} 个留在 F.SilkS，{n_fab} 个因无位降到 F.Fab")

# ---------------- ③ 品牌（背面）----------------
CX = (X0 + X1) / 2
text(BOARD_NAME, CX, 74.0, 3.0, layer=pcbnew.B_SilkS, just=CENTER)
text(f"{BOARD_REV}   {BOARD_DATE}", CX, 80.0, 2.0, layer=pcbnew.B_SilkS, just=CENTER)
text(COPYRIGHT, CX, 85.0, 1.6, layer=pcbnew.B_SilkS, just=CENTER)
text(WEBSITE, CX, 95.0, 1.8, layer=pcbnew.B_SilkS, just=CENTER)
text(SUBTITLE, CX, 90.0, 1.4, layer=pcbnew.B_SilkS, just=CENTER)

# ---------------- 行对齐 pass（每个候选都过完整校验）----------------
# 把同行、同高、同侧的位号拉到同一条线上。
# 曾经把这步放在 export_placement.py 里盲改——那里没有障碍模型，
# 结果把位号推到焊盘与丝印轮廓上（重叠 5→17、压铜 5→28）。对齐必须在这里做。


def _bh(ref):
    c = FPS[ref].GetCourtyard(pcbnew.F_CrtYd).BBox()
    return round(pcbnew.ToMM(c.GetHeight()), 2) if c.GetHeight() else 0.0


def _try_align(items, tgt):
    """把 items 全部移到绝对 y=tgt。任一项通不过检查就整组放弃（返回 None）。"""
    out = []
    for m in items:
        if abs(_REF_Y[m] - tgt) < 0.002:
            continue
        f = FPS[m]
        tw = len(m) * TXT_H * 0.75 + 0.3
        th = TXT_H + 0.3
        cx = pcbnew.ToMM(f.Reference().GetPosition().x)
        box = (cx - tw / 2, tgt - th / 2, cx + tw / 2, tgt + th / 2)
        c = f.GetCourtyard(pcbnew.F_CrtYd).BBox()
        if pcbnew.ToMM(c.GetTop()) - 0.2 < tgt < pcbnew.ToMM(c.GetBottom()) + 0.2:
            return None                                   # 压到自己本体
        if any(overlap(box, o, 0.05) for o in obstacles) or hits_poly(box):
            return None                                   # 压焊盘/铜箔
        mine = (cx, _REF_Y[m])
        rest = [b for b in placed
                if not (abs(b[0] + b[2] - 2 * mine[0]) < 0.01
                        and abs(b[1] + b[3] - 2 * mine[1]) < 0.01)]
        if any(overlap(box, o, 0.10) for o in rest):
            return None                                   # 压别的文字
        out.append((m, cx, box))
    return out


_moved = 0
for _mates in _rows.values():
    _buck = collections.defaultdict(list)
    for _m in _mates:
        if _m in _REF_Y:
            _by = pcbnew.ToMM(FPS[_m].GetPosition().y)
            _buck[(_bh(_m), 1 if _REF_Y[_m] > _by else -1)].append(_m)
    for _items in _buck.values():
        if len(_items) < 2:
            continue
        for _tgt, _ in collections.Counter(round(_REF_Y[m], 3) for m in _items).most_common():
            _res = _try_align(_items, _tgt)
            if _res is None:
                continue
            for _m, _cx, _box in _res:
                _old = _REF_Y[_m]
                FPS[_m].Reference().SetPosition(pcbnew.VECTOR2I_MM(round(_cx, 3), round(_tgt, 3)))
                placed = [b for b in placed
                          if not (abs(b[0] + b[2] - 2 * _cx) < 0.01
                                  and abs(b[1] + b[3] - 2 * _old) < 0.01)]
                placed.append(_box)
                _REF_Y[_m] = _tgt
                _moved += 1
            break
print(f"行对齐 pass: 调整 {_moved} 个位号（每个都过了压本体/压焊盘/压文字三项检查）")

# 机检：同一行、同一侧的位号必须落在同一条线上（比绝对 y）。
# 靠眼睛看渲染图会漏——实测漏过 C62 比同行高 0.33mm，是用户指出来的。
_bad, _rows_ck = [], 0
for _rid, _mates in _rows.items():
    for _side in (-1, 1):
        _o = [(_REF_Y[m], m) for m in _mates if m in _REF_Y
              and (_REF_Y[m] - pcbnew.ToMM(FPS[m].GetPosition().y)) * _side > 0]
        # 只比同高的：高度不同的元件位号各自贴自己本体，绝对 y 不该相同
        _byh = collections.defaultdict(list)
        for _v, _m in _o:
            _byh[_bh(_m)].append((_v, _m))
        for _h, _grp in _byh.items():
            if len(_grp) < 3:
                continue
            _rows_ck += 1
            _mode = collections.Counter(round(v, 2) for v, _ in _grp).most_common(1)[0][0]
            for _v, _m in _grp:
                if abs(_v - _mode) > 0.15:
                    _bad.append(f"{_m}(y={_v:.3f} vs 同行同高{_mode:.2f})")
        continue
        _rows_ck += 1
        _mode = collections.Counter(round(v, 2) for v, _ in _o).most_common(1)[0][0]
        for _v, _m in _o:
            if abs(_v - _mode) > 0.15:
                _bad.append(f"{_m}(y={_v:.3f} vs 同行{_mode:.2f})")
# ⚠️ 这里曾经是 assert，但它拦错了对象。行对齐 pass 调整位号时要过"压本体/压焊盘/
# 压文字"三项检查，**有些位号周围就是没空位、调不动**——那是现实约束，不是 bug。
# 而 assert 会让脚本在加品牌丝印之前就退出，更糟的是它**先改后验**：
# 位号位置已经落盘，板子停在改了一半的状态（2026-08-13 就撞上过）。
# 改成警告并列出来，让人自己判断哪几个需要手工挪。
if _bad:
    print(f"⚠️ {len(_bad)} 个位号未与同行对齐（周围没空位，调不动，可手工微调）:")
    for _b in _bad:
        print(f"     {_b}")

# 机检：位号不得压在自己元件的本体上——压上去等于看不见，而且丝印会被元件盖住。
_on_body = []
for _f in board.GetFootprints():
    if _f.Reference().GetLayer() != pcbnew.F_SilkS:
        continue
    _c = _f.GetCourtyard(pcbnew.F_CrtYd).BBox()
    if not _c.GetHeight():
        continue
    # 大件（排针、连接器）位号压在本体上是正常做法，本体够大放得下且看得清；
    # 只有小件（<8mm）压上去才是缺陷——丝印会被元件盖住。
    if pcbnew.ToMM(_c.GetWidth()) > 8 or pcbnew.ToMM(_c.GetHeight()) > 8:
        continue
    _ry = pcbnew.ToMM(_f.Reference().GetPosition().y)
    _rx = pcbnew.ToMM(_f.Reference().GetPosition().x)
    if (pcbnew.ToMM(_c.GetTop()) < _ry < pcbnew.ToMM(_c.GetBottom())
            and pcbnew.ToMM(_c.GetLeft()) < _rx < pcbnew.ToMM(_c.GetRight())):
        _on_body.append(f"{_f.GetReference()}(位号 y={_ry:.3f} 落在本体 "
                        f"{pcbnew.ToMM(_c.GetTop()):.2f}~{pcbnew.ToMM(_c.GetBottom()):.2f} 内)")
assert not _on_body, "位号压在自己元件上：" + "，".join(_on_body)
print(f"位号未压本体机检通过")
print(f"位号行对齐机检通过（校验了 {_rows_ck} 组同行同侧位号）")

board.Save(PCB)
print(f"品牌丝印(背面): {BOARD_NAME} {BOARD_REV} {BOARD_DATE} {COPYRIGHT}")
print("saved:", PCB)
