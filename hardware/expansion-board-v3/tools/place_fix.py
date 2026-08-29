"""配套元件归位——只动**真影响性能**的，其余一律不碰。

## ⚠️ 先读这段：这个脚本第一版把板子做坏了

2026-08-12 我用它挪了 41 个元件，把去耦电容按"离电源脚 1.2mm"硬塞到 U8/U10
引脚周围，结果：

    未连通      0  →  8~14      ← 前几轮辛苦解开的引脚出口拥堵，一轮全填回去
    板面        一半空着，另一半挤到位号都印不下（U8 右侧到 U10 那条带糊成一片）
    换来的收益  去耦电容 15mm → 1.2mm，**测不出来**

评审的质疑："你是否真的过于吹毛求疵了"、"距离差 0.1mm 和差 0.3mm 还有差 0.5mm，
在电气性能上真的达到了完全无法接受的程度吗？我们这块板子的精密程度真的超过
手机主板了吗？"

**答案是：没有，是我错了。** 三条教训：

### 一、去耦距离是软指标，布线可行性是硬约束，别搞反

去耦效果由**环路电感**决定，不是横向距离。6 层板上电容到 In1 地平面只隔 0.1mm
介质、过孔极短，回路电感主要来自过孔和平面，横向 1mm 还是 4mm 贡献很小。
"必须 <2mm"是 GHz 级 BGA 的经验值，我们最快的信号是 48MHz 晶振和 QSPI，套那个
标准是错的。而"引脚布不出线"是当场就付的硬代价——用测不出的收益换硬指标，
这个交易永远不划算。

### 二、我们不是手机主板

    手机主板   10~14 层、盲埋孔、0.075mm 线宽、0.35mm pitch BGA
    这块板     6 层、全通孔、0.15mm 线宽、0.4mm pitch QFN

精密度差一个数量级，却用了比手机主板还激进的贴身程度。

### 三、**要考虑人拿热风枪能不能吹**（2026-08-12 评审明确要求）

"请不要以计算机的标准来要求人类"。元件贴太近，返修时热风一吹旁边的件全跑了，
镊子也伸不进去。手工可返修要求**边到边 ≥0.5mm，理想 1.0mm**。
算法眼里 0.2mm 间隙"合法"，人手里那是没法修的板子。

## 现在的取舍

只动**真影响性能**的，判据是"不动会不会有可测量的后果"：

    ✅ 该动  模拟小信号横穿半块板（1090 检波链 60~79mm）——噪声直接进判决门限
    ✅ 该动  高阻抗小信号正对开关电源电感（32kHz 晶振 vs 对面 2R2）
    ✅ 该动  去耦电容离电源脚 >10mm ——这个量级确实等于没有
    ❌ 不动  去耦电容 3mm 想优化到 1.2mm ——测不出来，还堵死引脚出口
    ❌ 不动  任何"看着更整齐"的调整

## 硬约束

- FROZEN 里的件绝不动：对扣排针 J1、天线 ANT1、射频钉死件
- 射频网络上的件不动：位置是阻抗匹配的一部分
- courtyard 不许重叠，且**边到边留够返修间距**（HAND_GAP）
- 不占引脚正前方的逃逸通道（PIN_ESCAPE_MM）
- 不许出板框内缩线

用法：place_fix.py [plan|apply]
      apply 只改 PLACEMENT.py，随后跑 gen_pcb 生效
"""
import collections
import math
import os
import re
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(T, "tools"))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")
MODE = sys.argv[1] if len(sys.argv) > 1 else "plan"

FROZEN = {"J1", "ANT1", "L8", "D3", "C39", "J5", "J4", "H1", "H2", "H3", "H4",
          "J2", "J8", "J6"}
POWER_NETS = {"VCC_5V", "USB_VBUS", "3V3_DIG", "3V3_RF", "3V3_GNSS", "RP_1V1",
              "SUBG_VDDR"}
# 去耦电容的目标距离。1.2 是第一版的值，太贪：电容直接压在引脚出口上。
# 3.0 在环路电感上和 1.2 没有可测量的差别（见文件头），却给引脚留出了逃逸通道。
TARGET_MM = 3.0
# 只救**离谱**的。3mm 想优化到 1.2mm 是吹毛求疵，15mm 才是真问题。
RESCUE_OVER_MM = 10.0
# 但"超了 10mm"还不够格动手——还要看**落点在不在拥挤区**。
# 实测各区可放 0603 的落点数（zero 板）：
#     U13/U14 一带  2032 个   占用率 18~34%   宽松
#     U7 一带        798 个                   宽松
#     U8 引脚出口    308 个                   全板最挤 ← 上次就是往这儿塞翻的车
# 这两颗的电源脚去耦虽然也有 13~16mm，但它们的落点全在引脚出口那一小块，
# 而那里正是前几轮辛苦解开、好不容易做到未连通 0 的地方。
# 收益是"更好"（15mm→3.5mm，测不出差别），代价是可能又布不通——不值。
# 留到下一版整体重排布局时一起处理。
CROWDED_HOSTS = {"U8", "U10"}
# 手工返修间距：人拿热风枪吹一个 0402/0603，旁边留不出这个空隙就会把邻居一起
# 吹跑，镊子也伸不进去。算法眼里 0.2mm "合法"，人手里那是没法修的板子。
HAND_GAP = 0.5
# 引脚正前方这么长的一段是逃逸通道，不许放元件——占了它，线就出不来。
PIN_ESCAPE_MM = 1.5
EDGE_KEEP = 1.0          # 离板框留白

board = pcbnew.LoadBoard(PCB)
mm = pcbnew.ToMM
try:
    from gen_sch import RF_NETS
except Exception:
    RF_NETS = set()

bb = board.GetBoardEdgesBoundingBox()
BX0, BY0 = mm(bb.GetLeft()) + EDGE_KEEP, mm(bb.GetTop()) + EDGE_KEEP
BX1, BY1 = mm(bb.GetRight()) - EDGE_KEEP, mm(bb.GetBottom()) - EDGE_KEEP

# ── 快照（f.Pads() 只能遍历一次，一次抽干净）────────────────────────────
FP = {}
for f in board.GetFootprints():
    ref = f.GetReference()
    c = f.GetPosition()
    cy = f.GetCourtyard(pcbnew.F_CrtYd).BBox()
    if cy.GetWidth() <= 0:
        cy = f.GetBoundingBox()
    pads = []
    for p in f.Pads():
        if not p.IsOnCopperLayer():
            continue
        pp = p.GetPosition()
        pads.append((p.GetNumber(), p.GetNetname(), mm(pp.x), mm(pp.y)))
    FP[ref] = dict(x=mm(c.x), y=mm(c.y), val=f.GetValue(),
                   w=mm(cy.GetWidth()), h=mm(cy.GetHeight()),
                   # courtyard 相对元件中心的偏移，挪动时跟着走
                   dx=mm(cy.GetLeft()) - mm(c.x), dy=mm(cy.GetTop()) - mm(c.y),
                   pads=pads, nets={n for _, n, _, _ in pads if n})

MOVED = {}                     # ref -> (x, y)


def box_of(ref, x=None, y=None):
    d = FP[ref]
    x = d["x"] if x is None else x
    y = d["y"] if y is None else y
    return (x + d["dx"], y + d["dy"], x + d["dx"] + d["w"], y + d["dy"] + d["h"])


def overlaps(ref, x, y):
    """courtyard 与别人重叠？—— 判空位必须用 courtyard，不是焊盘范围。"""
    a = box_of(ref, x, y)
    if a[0] < BX0 or a[1] < BY0 or a[2] > BX1 or a[3] > BY1:
        return True
    for other in FP:
        if other == ref:
            continue
        ox, oy = MOVED.get(other, (FP[other]["x"], FP[other]["y"]))
        b = box_of(other, ox, oy)
        # 留出手工返修间距，不是"不重叠就行"
        if not (a[2] + HAND_GAP <= b[0] or a[0] >= b[2] + HAND_GAP
                or a[3] + HAND_GAP <= b[1] or a[1] >= b[3] + HAND_GAP):
            return True
    return False


# ── 对面板子的危险区（投影到我们板上的坐标）──────────────────────────
# 4.3 主板左下角那颗 2R2 是 7×7×3mm / 8A / 2.2uH 的开关电源储能电感。
# 按安装孔换算（两板孔距同为 92×50）+ 对扣手性（面对面，X 镜像 / Y 不变）
# 投影到我们板是 (135.6, 101.2)。对扣净空 7mm、电感高 3mm，垂直只隔 3.2mm。
# 磁耦合按距离立方衰减，这个距离等于贴着——敏感的高阻抗小信号电路必须让开。
# 半径取 8mm：电感本体 7mm 见方（半对角 5mm）再加 3mm 余量。
DANGER = [(135.6, 101.2, 8.0, "4.3 主板 2R2 电感(8A DC-DC)")]


def in_danger(x, y):
    for dx, dy, dr, _ in DANGER:
        if math.hypot(x - dx, y - dy) < dr:
            return True
    return False


# 每根引脚正前方的一段是它的逃逸通道，元件压上去线就出不来了。
# 第一版没有这条，find_spot 只管"离目标点近"，专挑最不该放的地方放——
# 这是把未连通从 0 顶到 8~14 的直接原因。
ESCAPE = []          # [(x1,y1,x2,y2)] 通道矩形
for _ref, _d in FP.items():
    if not re.match(r"^U\d", _ref) or len(_d["pads"]) < 4:
        continue
    for _num, _n, _px, _py in _d["pads"]:
        _vx, _vy = _px - _d["x"], _py - _d["y"]
        _L = math.hypot(_vx, _vy) or 1.0
        _vx, _vy = _vx / _L, _vy / _L
        _ex, _ey = _px + _vx * PIN_ESCAPE_MM, _py + _vy * PIN_ESCAPE_MM
        _w = 0.3
        ESCAPE.append((min(_px, _ex) - _w, min(_py, _ey) - _w,
                       max(_px, _ex) + _w, max(_py, _ey) + _w))


# ── 射频通道：绝对不能压 ──────────────────────────────────────────────
# 这是 2026-08-12 第二轮失败的**技术根因**。我把 1090 检波链 11 个挪到 U13/U14
# 旁边，理由是"那片区域 courtyard 占用率只有 28%，很空"——**但那片空地不是空的，
# 是留给射频链的通道**，courtyard 密度统计里根本没有走线这一项。
# 结果 11 个里有 7 个直接压在射频走线上（多个 0.00mm）：
#     R31/R20/C47/C51 压 DET_INLO   R34/C46 压 DET_IN   R33 压 SUBG_RFN
#
# 射频线为什么压不得：它有两条硬约束——**只能走 F.Cu、绝对不能打过孔**
# （In1 是它的 50Ω 参考面，打孔就是在参考面上开洞）。所以射频线**没有换层这个
# 逃生出口**，元件往它头上一放，它只能绕，绕不动就断。
# 那一轮断掉的 SUBG_RXTX / SW1_J3 正是这么来的。
RF_CLEAR = 0.40          # 射频线半宽 0.17 + 净空 0.2 + 余量
RF_SEGS = []
for _t in board.GetTracks():
    if isinstance(_t, pcbnew.PCB_VIA):
        continue
    if _t.GetNetname() in RF_NETS:
        RF_SEGS.append(((mm(_t.GetStart().x), mm(_t.GetStart().y)),
                        (mm(_t.GetEnd().x), mm(_t.GetEnd().y))))


def _seg_box(a, b, r):
    """线段到矩形的最近距离（采样，够用）"""
    best = 1e9
    for i in range(21):
        t = i / 20
        x, y = a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t
        best = min(best, math.hypot(max(r[0] - x, 0, x - r[2]),
                                    max(r[1] - y, 0, y - r[3])))
    return best


def blocks_rf(ref, x, y):
    box = box_of(ref, x, y)
    for a, b in RF_SEGS:
        if _seg_box(a, b, box) < RF_CLEAR:
            return True
    return False


def blocks_escape(ref, x, y):
    a = box_of(ref, x, y)
    for e in ESCAPE:
        if not (a[2] <= e[0] or a[0] >= e[2] or a[3] <= e[1] or a[1] >= e[3]):
            return True
    return False


def find_spot(ref, tx, ty, maxr=6.0, avoid_danger=False):
    """在 (tx,ty) 附近螺旋找一个放得下的位置，越近越好。

    avoid_danger=True 时额外避开 DANGER 区——用于 32kHz 晶振这类
    高阻抗小信号电路，它们正是被对面电感干扰的对象，
    不加这条约束的话算法可能"就近"把它们又放回危险区里。
    """
    def ok(x, y):
        return (not overlaps(ref, x, y)
                and not blocks_escape(ref, x, y)
                and not blocks_rf(ref, x, y)
                and not (avoid_danger and in_danger(x, y)))

    if ok(tx, ty):
        return tx, ty, 0.0
    r = 0.25
    while r <= maxr:
        n = max(8, int(2 * math.pi * r / 0.25))
        for i in range(n):
            a = 2 * math.pi * i / n
            x, y = tx + r * math.cos(a), ty + r * math.sin(a)
            if ok(x, y):
                return round(x, 3), round(y, 3), r
        r += 0.25
    return None


ONLY = os.environ.get("PK_ONLY", "")     # "2" = 只跑规则二，别再动已归位的去耦电容

# ── 规则一：去耦电容跟随电源脚 ────────────────────────────────────────
# 谁是去耦电容：两脚，一端接地，另一端接某条电源轨
caps = collections.defaultdict(list)
for ref, d in FP.items():
    if not ref.startswith("C") or ref in FROZEN:
        continue
    if "GND" not in d["nets"]:
        continue
    for n in d["nets"] - {"GND", ""}:
        if n in POWER_NETS:
            caps[n].append(ref)

# 谁需要：IC（≥4 脚）的每个电源焊盘
needs = []
for ref, d in sorted(FP.items()):
    if not re.match(r"^U\d", ref) or len(d["pads"]) < 4:
        continue
    for num, n, px, py in d["pads"]:
        if n not in POWER_NETS:
            continue
        # 当前离它最近的同网络电容有多远
        best = min((math.hypot(FP[c]["x"] - px, FP[c]["y"] - py), c)
                   for c in caps[n]) if caps[n] else (999.0, None)
        # 电源脚朝芯片外的方向
        vx, vy = px - d["x"], py - d["y"]
        L = math.hypot(vx, vy) or 1.0
        needs.append(dict(ic=ref, pin=num, net=n, px=px, py=py,
                          cur=best[0], curcap=best[1], dx=vx / L, dy=vy / L))

# 先救最差的：按"当前最近电容距离"从大到小分配
needs.sort(key=lambda q: -q["cur"])
plan1, short1, used = [], [], set()
skipped_crowded = []
for q in (() if ONLY == "2" else needs):
    # 只救离谱的。3mm 想优化到 1.2mm 是吹毛求疵，测不出来还堵引脚出口。
    if q["cur"] < RESCUE_OVER_MM:
        continue
    if q["ic"] in CROWDED_HOSTS:
        skipped_crowded.append(f"{q['ic']}.{q['pin']}[{q['net']}] {q['cur']:.1f}mm")
        continue
    pool = [c for c in caps[q["net"]] if c not in used]
    if not pool:
        short1.append(q)
        continue
    # 选当前离这个脚最近的可用电容——挪动距离最短，也最不容易打乱别处
    cap = min(pool, key=lambda c: math.hypot(FP[c]["x"] - q["px"], FP[c]["y"] - q["py"]))
    tx = q["px"] + q["dx"] * TARGET_MM
    ty = q["py"] + q["dy"] * TARGET_MM
    spot = find_spot(cap, tx, ty)
    if not spot:
        short1.append(q)
        continue
    x, y, off = spot
    used.add(cap)
    MOVED[cap] = (x, y)
    d = math.hypot(x - q["px"], y - q["py"])
    plan1.append((q["ic"], q["pin"], q["net"], cap, q["cur"], d,
                  math.hypot(x - FP[cap]["x"], y - FP[cap]["y"])))

print("═══ 规则一：去耦电容归位 ═══")
print(f"  {len(needs)} 个电源脚 / 可用去耦电容 {sum(len(v) for v in caps.values())} 个")
imp = [p for p in plan1 if p[5] < p[4] - 0.1]
print(f"  安排 {len(plan1)} 对，其中 {len(imp)} 对拉近了；电容不够/放不下 {len(short1)} 个脚\n")
for ic, pin, net, cap, cur, new, moved in sorted(plan1, key=lambda z: -(z[4] - z[5]))[:18]:
    if new < cur - 0.1:
        print(f"    {ic}.{pin:<3s}[{net:9s}] ← {cap:4s}  {cur:5.1f}mm → {new:4.2f}mm"
              f"   (电容挪了 {moved:.1f}mm)")
if skipped_crowded:
    print(f"\n    避开拥挤区、故意不动的 {len(skipped_crowded)} 个脚"
          f"（{'/'.join(sorted(CROWDED_HOSTS))} 的引脚出口是全板最挤的地方）:")
    for x in skipped_crowded:
        print(f"      {x}")
if short1:
    bynet = collections.Counter(q["net"] for q in short1)
    print(f"\n    没配上的 {len(short1)} 个脚，按电源域: {dict(bynet)}")

# ── 规则二：1090 检波链跟随 U13/U14 ──────────────────────────────────
# 这 11 个是 AD8319/AD8313 的门限/分压/滤波网络，现在全在板子最左边 x=55.6，
# 而它们伺候的芯片在最右边。模拟小信号横穿 60~79mm，噪声全picked up。
CHAIN = {
    "R20": "U13", "R21": "U14", "R31": "U14", "C46": "U14", "R32": "U14",
    "C47": "U14", "R33": "U14", "R34": "U14", "C49": "U14", "R35": "U14",
    "C51": "U14",
    # ── 32.768kHz 晶振组：躲开 4.3 主板的 DC-DC 电感 ──────────────────
    # 2026-08-12 评审指出 4.3 板左下角有颗 2R2 电感。按安装孔换算（两板孔距同为
    # 92×50，是唯一可靠的对齐锚点）+ 对扣手性（面对面，X 镜像 / Y 不变）：
    #     4.3 的左下孔 ↔ 我们的 H4，2R2 相对左下孔右 10.4mm、上 4.8mm
    #     → 投影到我们板 (135.6, 101.2)，**正下方 0.9mm 就是 C69**
    # 那颗电感是 7×7×3mm / 8A / 2.2uH 的开关电源储能电感，漏磁最强、di/dt 最高；
    # 而 C68/C69/Y3 是 32.768kHz 晶振回路——皮法级、兆欧阻抗、几百毫伏摆幅，
    # 全板抗干扰最差的电路。对扣净空 7mm、电感高 3mm，垂直距离只剩 3.2mm，
    # 磁耦合按距离立方衰减，这个距离基本等于贴着。
    # 后果是频率牵引 / 相噪恶化 / 严重时不起振，而且 DRC、ERC、单板上电全测不出来，
    # 只有整机装配点亮屏幕满载跑才发作。
    # 挪到 U10 旁边一举两得：躲开电感，而且晶振本来就该贴着芯片放。
    "Y3": "U10", "C68": "Y3", "C69": "Y3",
}
plan2, short2 = [], []
for ref, host in CHAIN.items():
    if ref not in FP or ref in FROZEN:
        continue
    if FP[ref]["nets"] & set(RF_NETS):
        short2.append((ref, "挂着射频网络，位置是匹配的一部分，不动"))
        continue
    hx, hy = MOVED.get(host, (FP[host]["x"], FP[host]["y"]))   # host 可能刚被挪过
    old = math.hypot(FP[ref]["x"] - hx, FP[ref]["y"] - hy)
    # 负载电容要紧贴晶振（回路面积越小越好），别人可以远一点。
    # 4.0 不是 3.0：给 3.0 时 C69 在 Y3 周围找不到落点被跳过了，
    # 而 C69 恰恰是被电感正对的那一个，跳过它整件事就白做了。
    sens = ref in ("Y3", "C68", "C69")      # 32kHz 组，必须躲开对面的电感
    spot = find_spot(ref, hx, hy,
                     maxr=4.0 if host.startswith("Y") else 9.0,
                     avoid_danger=sens)
    if not spot:
        short2.append((ref, f"{host} 周围 9mm 内没有空位"))
        continue
    x, y, _ = spot
    MOVED[ref] = (x, y)
    plan2.append((ref, host, old, math.hypot(x - hx, y - hy)))

print("\n═══ 规则二：1090 检波链归位 ═══")
for ref, host, old, new in sorted(plan2, key=lambda z: -(z[2] - z[3])):
    print(f"    {ref:4s} → {host} 旁   离主芯片 {old:5.1f}mm → {new:4.1f}mm")
for ref, why in short2:
    print(f"    {ref:4s} 跳过：{why}")

print(f"\n合计挪动 {len(MOVED)} 个元件")

# ── 落盘：改 PLACEMENT.py ────────────────────────────────────────────
if MODE == "apply" and MOVED:
    P = os.path.join(T, "tools", "PLACEMENT.py")
    src = open(P).read()
    n = 0
    for ref, (x, y) in MOVED.items():
        pat = re.compile(r"(\('" + re.escape(ref) + r"'\s*,\s*)[-\d.]+(\s*,\s*)[-\d.]+(\s*,\s*\d+\s*\))")
        src, k = pat.subn(lambda m: f"{m.group(1)}{x:7.3f}{m.group(2)}{y:6.3f}{m.group(3)}", src, count=1)
        assert k == 1, f"{ref} 在 PLACEMENT.py 里没匹配到（格式变了？）"
        n += 1
    open(P, "w").write(src)
    print(f"已改写 PLACEMENT.py 中 {n} 个元件 → 跑 gen_pcb.py 生效")
    print("⚠️ 布局变了，ROUTES.json 会失配——必须重新布线（run_route.sh），"
          "用飞线总长/交叉数 + ERC 验收，不赚就回退")
else:
    print("（plan 模式，未写盘）")
