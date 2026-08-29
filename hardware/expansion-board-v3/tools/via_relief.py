"""过孔让位——诊断「哪个过孔堵死了哪根引脚的逃逸通道，差多少，该往哪挪」。

## 规则从哪来

2026-08-12 手工救活 SUBG_VDDR(U10.45) 和 RP_1V1(U8.23) 之后，要求把做法
总结成规则而不是固化坐标——坐标一改布局就作废，规则不会。评审给的规则原文：

    「先把走线往芯片外侧挪开，腾出一定空间能给到过孔。同时对于相邻过孔，要判断
      中间是否夹着过粗的电源线或者数据线，如果有，那就把过孔也一起往外挪、走 45° 躲开」

## 判据 ✅ 已固化并验证

两种挡法，都要查：

    ① 两孔夹住：孔间净空 ≥ 线宽 + 2×净空      信号 0.45mm ／ 电源 0.55mm
    ② 单孔贴边：孔到逃逸走廊中心线 ≥ 孔半径 + 半线宽 + 净空

②不能省。实测挪开 3V3_DIG 之后 RP_1V1 依然走不通——SWCLK 孔离 pin23 的逃逸路径
只有 0.20mm，而走线要 0.275mm。手工修的时候两个孔都挪了，正是这个道理。

**逃逸通道的方向必须按引脚轴向算，不能用「DRC 缺口两端的连线」。** DRC 报的是
"这两块铜没连上"，那条连线的方向未必是这根线该走的方向：U8.23 的缺口另一端在
In2、位于右侧，而它实际要往左逃。按连线找只会找到右边一对无关的过孔
（RP_XIN/RP_RUN），真正堵死它的完全漏掉。

回归验证（拿 pcb.best 跑，靶子是手工修出来的 pcb.zero）：脚本自动找出全部
4 处挡路点，**包含手工挪的那两个孔**，差值算得准：

    U10.45[SUBG_VDDR] 需要离轴 0.575，实际 0.500（差 0.075）  via[3V3_DIG] 贴着走廊
    U8.23 [RP_1V1]    需要离轴 0.575，实际 0.400（差 0.175）  via[3V3_DIG] 贴着走廊
    U8.23 [RP_1V1]    需要离轴 0.475，实际 0.400（差 0.075）  via[SWCLK]   贴着走廊
    U8.23 [RP_1V1]    需要 0.550，实际 0.323（差 0.227）      两孔夹住

## 自动让位 ❌ 跑不通，别开 apply

apply 能算出合法的新孔位（方向和手工挪的一致），但**挪完之后补不回来**。
瓶颈不在挪孔，在重布：孔一挪，连在它上面的走线就得重走，而这一带正是全板最密的
地方，route_fix 的栅格 A*（GRID=0.15mm）补不回来。四种配置实测（基线未连通 2）：

    余量 0.02、挪 1 个删 2 段         → 2   持平
    余量 0.20、挪 1 个删 2 段          → 3   变差
    加单孔判据、挪 4 个删 8 段         → 8   大幅变差
    一次只挪 1 个 + 每步验收回退       → 4   第一轮就亏，回退

**删线容易重布难，删得越多亏得越狠。** 手工能成，是因为挪孔的同时重新
规划了走线路径；算法这边"删掉交给 route_fix"这一步接不住。

所以本脚本的定位是**诊断器**：布局一改跑一下 plan，立刻知道哪几个孔挡了谁、
差多少、该沿哪个 45° 方向挪多远，然后人在 KiCad 里三下挪好——实测这比改五版算法
有效得多。apply 保留着，但默认一次只挪一个（PK_MAX_MOVES），且**必须**跟一轮
route_fix + 未连通总数验收，不赚立刻回退。

## 用法

  via_relief.py plan  [drc.json]     诊断（推荐）
  via_relief.py apply [drc.json]     试着挪；PK_MAX_MOVES 控制一次挪几个（默认 1）
  PK_WHY=1                           打印每个候选位失败在哪
  PK_BOARD_DIR=<dir>                 在副本上跑

## 怎么挪

- **方向**：远离所属元件的中心（"往芯片外侧"）。过孔归属取最近的元件 courtyard。
- **角度**：只走 45° 的整数倍。斜着让位比正交多腾出 √2 倍的横向空间，
  而且 45° 是本板布线的既有风格（正交路径还会踩 freerouting 的 PolylineTrace 递归 bug）。
- **连带**：过孔挪了，接在它上面的走线端点跟着挪，否则电气就断了。
- **校验**：新位置对**所有 6 层**做连续几何净空检查（通孔穿全层），
  挪完的走线段也要重新检查。任何一项不过就换下一个候选位；全试完还不行就放弃这个孔，
  **绝不留下 DRC 违例**。

## 用法

  via_relief.py plan  [drc.json]     只打印，不写盘
  via_relief.py apply [drc.json]     挪动并保存

PK_BOARD_DIR 可指到副本上跑（回归验证用）。
"""
import json
import math
import os
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(T, "tools"))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
BUILD = os.environ.get("PK_BUILD_DIR") or os.path.join(T, "build")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")
PRO = os.path.join(BDIR, "expansion-board-v3.kicad_pro")

MODE = sys.argv[1] if len(sys.argv) > 1 else "plan"
DRC = sys.argv[2] if len(sys.argv) > 2 else os.path.join(BUILD, "drc-final.json")

STEP = 0.05             # 外推步长
MAX_PUSH = 2.0          # 最多推这么远，再远就不是"让位"而是重新布局了
# 让位后额外留的余量。⚠️ 不能取"刚好够"：实测让到 0.59mm（需求 0.55）之后，
# route_fix 依然报走不通——它是栅格 A*（GRID=0.15mm），**量化误差就能吃掉那 0.04mm**。
# 手工修的时候把通道拉到了 1.25mm，余量充足。这里给一个整栅格 + 一点。
MARGIN = 0.20

board = pcbnew.LoadBoard(PCB)
mm = pcbnew.ToMM
CU = [pcbnew.F_Cu, pcbnew.In1_Cu, pcbnew.In2_Cu, pcbnew.In3_Cu, pcbnew.In4_Cu, pcbnew.B_Cu]

# ── 网络类 → 线宽/净空（读工程文件，不写死）─────────────────────────────
import fnmatch                                                    # noqa: E402
_ns = json.load(open(PRO))["net_settings"]
CLS = {c["name"]: c for c in _ns["classes"]}
_PAT = _ns.get("netclass_patterns", [])


def net_class(name):
    for p in _PAT:
        if fnmatch.fnmatch(name, p["pattern"]):
            return p["netclass"]
    return "Default"


def clr_of(name):
    return CLS.get(net_class(name), CLS["Default"]).get("clearance", 0.15)


def width_of(name):
    return CLS.get(net_class(name), CLS["Default"]).get("track_width", 0.15)


# ── 几何 ──────────────────────────────────────────────────────────────
def pt_seg(p, a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    L = dx * dx + dy * dy
    if L < 1e-12:
        return math.hypot(p[0] - a[0], p[1] - a[1])
    t = max(0.0, min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / L))
    return math.hypot(p[0] - (a[0] + dx * t), p[1] - (a[1] + dy * t))


def seg_seg(a, b, c, d):
    """⚠️ 必须先判相交。只取四端点到对方线段的最小值，会把**交叉**的两条线判成
    有余量——route_fix 就是这么布出过一条真短路的。"""
    def cr(o, p, q):
        return (p[0] - o[0]) * (q[1] - o[1]) - (p[1] - o[1]) * (q[0] - o[0])
    d1, d2, d3, d4 = cr(c, d, a), cr(c, d, b), cr(a, b, c), cr(a, b, d)
    if ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0)):
        return 0.0
    return min(pt_seg(a, c, d), pt_seg(b, c, d), pt_seg(c, a, b), pt_seg(d, a, b))


def pt_rect(p, r):
    return math.hypot(max(r[0] - p[0], 0, p[0] - r[2]), max(r[1] - p[1], 0, p[1] - r[3]))


def seg_rect(a, b, r):
    best = min(pt_rect(a, r), pt_rect(b, r))
    n = max(2, int(math.hypot(b[0] - a[0], b[1] - a[1]) / 0.05))
    for i in range(n + 1):
        t = i / n
        best = min(best, pt_rect((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t), r))
    return best


# ── 板上的铜 ──────────────────────────────────────────────────────────
PADS, TRKS, VIAS = [], [], []
for f in board.GetFootprints():
    for p in f.Pads():
        if not p.IsOnCopperLayer():
            continue                       # F.Paste 锡膏格不是铜，别当障碍
        bb = p.GetBoundingBox()
        PADS.append(dict(net=p.GetNetname(),
                         layers={l for l in CU if p.IsOnLayer(l)},
                         box=(mm(bb.GetLeft()), mm(bb.GetTop()),
                              mm(bb.GetRight()), mm(bb.GetBottom()))))
for t in board.GetTracks():
    if isinstance(t, pcbnew.PCB_VIA):
        p = t.GetPosition()
        VIAS.append(dict(obj=t, net=t.GetNetname(), x=mm(p.x), y=mm(p.y),
                         r=mm(t.GetWidth()) / 2, drill=mm(t.GetDrill()) / 2))
    else:
        TRKS.append(dict(obj=t, net=t.GetNetname(), layer=t.GetLayer(),
                         a=(mm(t.GetStart().x), mm(t.GetStart().y)),
                         b=(mm(t.GetEnd().x), mm(t.GetEnd().y)),
                         half=mm(t.GetWidth()) / 2))

# 元件 courtyard 中心：判断"芯片外侧"是哪一侧
FPC = []
for f in board.GetFootprints():
    bb = f.GetCourtyard(pcbnew.F_CrtYd).BBox()
    if bb.GetWidth() <= 0:
        bb = f.GetBoundingBox()
    c = f.GetPosition()
    FPC.append((f.GetReference(), mm(c.x), mm(c.y),
                (mm(bb.GetLeft()), mm(bb.GetTop()), mm(bb.GetRight()), mm(bb.GetBottom()))))


def owner_of(x, y):
    """这个过孔属于哪个元件——取 courtyard 距离最近的那个。"""
    best, bd = None, None
    for ref, cx, cy, box in FPC:
        d = pt_rect((x, y), box)
        if bd is None or d < bd:
            bd, best = d, (ref, cx, cy)
    return best


# ── 找"被夹住"的通道 ──────────────────────────────────────────────────
def gap_between(v1, v2):
    """两个过孔之间的净空（铜边到铜边）"""
    return math.hypot(v1["x"] - v2["x"], v1["y"] - v2["y"]) - v1["r"] - v2["r"]


def need_for(net):
    """让 net 的一条线从中间过去，需要多宽的净空"""
    return width_of(net) + 2 * clr_of(net)


# ⚠️ 判据不能用「DRC 缺口两端的连线」。DRC 报的是"这两块铜没连上"，那条连线
# 指向的方向未必是这根线**该走**的方向：U8.23 的缺口另一端在 In2、位于右侧，
# 而它实际要往**左**逃（手工就是往左拉通的）。按连线找挡路者，只会找到右边一对
# 无关的过孔，真正堵死它的 3V3_DIG/SWCLK 在左边、完全漏掉。
#
# 正确判据是**引脚的逃逸通道**：从焊盘尖端沿引脚轴向朝芯片外看过去，
# 前方分居轴线两侧的两个过孔，如果间隙容不下这根线，它们就是挡路者。
FPMAP = {}
for f in board.GetFootprints():
    c = f.GetPosition()
    FPMAP[f.GetReference()] = (mm(c.x), mm(c.y))

PADPOS = {}
for f in board.GetFootprints():
    fx, fy = FPMAP[f.GetReference()]
    for p in f.Pads():
        if not p.IsOnCopperLayer():
            continue
        bb = p.GetBoundingBox()
        px, py = mm(p.GetPosition().x), mm(p.GetPosition().y)
        w, h = mm(bb.GetWidth()), mm(bb.GetHeight())
        # 逃逸方向 = 焊盘长边指向元件外侧
        if w >= h:
            d = (1.0, 0.0) if px > fx else (-1.0, 0.0)
            tip = (mm(bb.GetRight()) if d[0] > 0 else mm(bb.GetLeft()), py)
        else:
            d = (0.0, 1.0) if py > fy else (0.0, -1.0)
            tip = (px, mm(bb.GetBottom()) if d[1] > 0 else mm(bb.GetTop()))
        PADPOS[(f.GetReference(), p.GetNumber())] = (tip, d, p.GetNetname())

jobs = []
seen = set()
import re                                                          # noqa: E402
try:
    unc = json.load(open(DRC)).get("unconnected_items", [])
except Exception:
    unc = []

REACH = 3.0            # 逃逸通道往外看多远
for it in unc:
    for i in it["items"]:
        m = re.match(r"Pad (\S+) \[([^\]]*)\] of (\S+)", i["description"])
        if not m:
            continue
        num, net, ref = m.group(1), m.group(2), m.group(3)
        key = (ref, num)
        if key not in PADPOS:
            continue
        tip, d, _ = PADPOS[key]
        need = need_for(net)
        # 前方的过孔（投影为正、且在 REACH 内）
        cand = []
        for v in VIAS:
            if v["net"] == net:
                continue
            rel = (v["x"] - tip[0], v["y"] - tip[1])
            proj = rel[0] * d[0] + rel[1] * d[1]
            if not (0 < proj < REACH):
                continue
            side = d[0] * rel[1] - d[1] * rel[0]      # 叉积：在轴线哪一侧
            if abs(side) > REACH:
                continue
            cand.append((v, proj, side))
        # ⚠️ 挡路的不一定是"两孔夹住"，**单个孔贴着走廊**一样能堵死。
        # 实测：挪开 3V3_DIG 之后 RP_1V1 依然走不通，因为 SWCLK 孔离 pin23 的
        # 逃逸路径只有 0.20mm，而走线要 0.125(半线宽)+0.15(净空)=0.275mm。
        # 手工修的时候两个孔都挪了，正是这个道理。
        half_need = width_of(net) / 2 + clr_of(net)
        for v, proj, side in cand:
            d_axis = abs(side)                       # 到逃逸走廊中心线的距离
            if d_axis >= v["r"] + half_need:
                continue
            k = ("single", round(v["x"], 3), round(v["y"], 3), ref, num)
            if k in seen:
                continue
            seen.add(k)
            jobs.append(dict(net=net, kind="single", pair=(v, None),
                             need=v["r"] + half_need, gap=d_axis, who=f"{ref}.{num}",
                             tip=tip, dir=d))
        for a in range(len(cand)):
            for b in range(a + 1, len(cand)):
                v1, p1, s1 = cand[a]
                v2, p2, s2 = cand[b]
                if (s1 > 0) == (s2 > 0):
                    continue                          # 必须分居两侧才叫"夹住"
                g = gap_between(v1, v2)
                if g >= need:
                    continue
                k = tuple(sorted([(v1["x"], v1["y"]), (v2["x"], v2["y"])]))
                if k in seen:
                    continue
                seen.add(k)
                jobs.append(dict(net=net, kind="pair", need=need, gap=g, pair=(v1, v2),
                                 who=f"{ref}.{num}"))

print(f"扫描 {len(unc)} 处未连通 → 找到 {len(jobs)} 处过孔挡住逃逸通道")
for j in jobs:
    v1, v2 = j["pair"]
    if j["kind"] == "pair":
        print(f"  {j['who']}[{j['net']}] 需要 {j['need']:.3f}mm，实际 {j['gap']:.3f}mm"
              f"（差 {j['need']-j['gap']:.3f}）  夹在 via[{v1['net']}]({v1['x']:.2f},{v1['y']:.2f})"
              f" 与 via[{v2['net']}]({v2['x']:.2f},{v2['y']:.2f}) 之间")
    else:
        print(f"  {j['who']}[{j['net']}] 需要离轴 {j['need']:.3f}mm，实际 {j['gap']:.3f}mm"
              f"（差 {j['need']-j['gap']:.3f}）  via[{v1['net']}]({v1['x']:.2f},{v1['y']:.2f}) 贴着走廊")


# ── 挪一个过孔：沿 45° 往芯片外侧推 ────────────────────────────────────
# 评审给的规则是**两步**，缺一不可：
#     ①「先把**走线**往芯片外侧挪开，腾出一定空间能给到过孔」
#     ②「过孔也一起往外挪、走 45° 躲开」
# 第一版只做了 ②、把周围走线当成不可动的障碍，结果 8 个方向全部"via 位置撞铜"——
# 引脚出口那片地方本来就密，不先让开走线就根本没有落点。
#
# 所以障碍分两档：
#   **硬障碍**（焊盘、别的过孔、孔到孔间距）——绝对绕开，它们代表元件和别人的换层点
#   **软障碍**（走线）——可以让开：把它删掉，挪完孔交给 route_fix 重布
# 这正是手工修的时候做的事：删旧线 → 挪孔 → 重拉线。
DIRS8 = [(math.cos(math.radians(a)), math.sin(math.radians(a)))
         for a in range(0, 360, 45)]
HOLE_GAP = 0.25                 # 板规 min_hole_to_hole


def hard_blocked(x, y, r, drill, net, self_via):
    """硬障碍：焊盘、其它过孔、孔到孔。碰上就换位置，不能靠删东西解决。"""
    myc = clr_of(net)
    for pd in PADS:
        if pd["net"] == net:
            continue
        if pt_rect((x, y), pd["box"]) < r + max(myc, clr_of(pd["net"])):
            return True
    for v in VIAS:
        if v is self_via:
            continue
        d = math.hypot(x - v["x"], y - v["y"])
        if v["net"] != net and d < r + v["r"] + max(myc, clr_of(v["net"])):
            return True
        if d < drill + v["drill"] + HOLE_GAP:      # 物理钻孔，同网络也不豁免
            return True
    return False


def soft_blockers(x, y, r, net, self_via):
    """软障碍：挡在新位置上的走线。返回它们（准备删掉重布）。"""
    myc = clr_of(net)
    out = []
    for t in TRKS:
        if t["net"] == net:
            continue
        if pt_seg((x, y), t["a"], t["b"]) < r + t["half"] + max(myc, clr_of(t["net"])):
            out.append(t)
    return out


moved, gaveup = [], []
FAIL = {}
for j in jobs:
    v1, v2 = j["pair"]
    best = None
    for v in ((v1,) if j["kind"] == "single" else (v1, v2)):
        other = v2 if v is v1 else v1
        ref, cx, cy = owner_of(v["x"], v["y"])
        away = (v["x"] - cx, v["y"] - cy)
        L = math.hypot(*away) or 1.0
        away = (away[0] / L, away[1] / L)
        for d in sorted(DIRS8, key=lambda q: -(q[0] * away[0] + q[1] * away[1])):
            if d[0] * away[0] + d[1] * away[1] <= 0:
                break                       # 只往外侧挪，不往芯片里钻
            n = 1
            while n * STEP <= MAX_PUSH:
                nx, ny = v["x"] + d[0] * n * STEP, v["y"] + d[1] * n * STEP
                n += 1
                # 让够了没？pair 看两孔间隙，single 看离逃逸走廊中心线的距离
                if j["kind"] == "pair":
                    if math.hypot(nx - other["x"], ny - other["y"]) - v["r"] - other["r"] \
                            < j["need"] + MARGIN:
                        continue
                else:
                    rel = (nx - j["tip"][0], ny - j["tip"][1])
                    if abs(j["dir"][0] * rel[1] - j["dir"][1] * rel[0]) < j["need"] + MARGIN:
                        continue
                if hard_blocked(nx, ny, v["r"], v["drill"], v["net"], v):
                    if os.environ.get("PK_WHY"):
                        FAIL.setdefault((v["net"], round(math.degrees(math.atan2(d[1], d[0])))),
                                        []).append("硬障碍")
                    continue
                kill = soft_blockers(nx, ny, v["r"], v["net"], v)
                dist = math.hypot(nx - v["x"], ny - v["y"])
                # 打分：优先少删线，其次少挪动。删一条线约等于多挪 0.5mm。
                score = len(kill) * 0.5 + dist
                if best is None or score < best["score"]:
                    best = dict(via=v, nx=nx, ny=ny, dist=dist, score=score,
                                short=j["need"] - j["gap"],
                                ox=v["x"], oy=v["y"], kill=kill, ref=ref,
                                ang=round(math.degrees(math.atan2(d[1], d[0]))))
                break                       # 该方向上最近的可行点，够了
    if best:
        v = best["via"]
        # 连在这个孔上的走线也一并删掉——端点硬拖过去往往就撞了，
        # 而且形状也不该由"拖"决定，交给 route_fix 重新布更干净。
        att = [t for t in TRKS
               if t["net"] == v["net"]
               and (math.hypot(t["a"][0] - best["ox"], t["a"][1] - best["oy"]) < 0.01
                    or math.hypot(t["b"][0] - best["ox"], t["b"][1] - best["oy"]) < 0.01)]
        best["kill"] = list({id(t): t for t in best["kill"] + att}.values())
        nets = sorted({t["net"] for t in best["kill"]})
        print(f"  → via[{v['net']}] ({best['ox']:.3f},{best['oy']:.3f}) 沿 {best['ang']}° "
              f"外推 {best['dist']:.3f}mm 到 ({best['nx']:.3f},{best['ny']:.3f})"
              f"  [{best['ref']} 外侧]")
        print(f"       让开 {len(best['kill'])} 段走线交给 route_fix 重布: {nets}")
        moved.append(best)
        v["x"], v["y"] = best["nx"], best["ny"]
    else:
        gaveup.append(j)
        print(f"  → [{j['net']}] 两个孔都挪不动（{MAX_PUSH}mm 内没有安全落点）")
        if os.environ.get("PK_WHY"):
            for k, lst in sorted(FAIL.items()):
                print(f"       via[{k[0]}] 朝 {k[1]:>4}°: 硬障碍×{len(lst)}")
            FAIL.clear()

# ⚠️ 一次挪太多是净亏。实测（基线未连通 2）：
#     挪 1 个删 2 段 → 2（持平）   挪 1 个删 2 段(推更远) → 3   挪 4 个删 8 段 → 8
# **瓶颈不在挪孔，在重布**：删线容易，route_fix 未必补得回来，删得越多亏得越狠。
# 所以默认一次只挪最急的那一个（差得最多的），由调用方跑 route_fix 验收后再决定
# 要不要挪下一个。PK_MAX_MOVES 可调。
MAXM = int(os.environ.get("PK_MAX_MOVES", "1"))
if len(moved) > MAXM:
    moved.sort(key=lambda b: -b["short"])
    dropped = moved[MAXM:]
    moved = moved[:MAXM]
    print(f"\n本轮只挪最急的 {MAXM} 个，其余 {len(dropped)} 个留到下一轮"
          f"（重布跟不上，一次挪太多是净亏）")

print(f"\n可让位 {len(moved)} 个 / 挪不动 {len(gaveup)} 个")

if MODE == "apply" and moved:
    # ⚠️ 顺序要紧：先改过孔位置，最后统一删走线。board.Remove() 之后本进程里所有
    # SWIG 代理都会退化成裸 SwigPyObject（route_fix.py:26 记的是同一个坑），
    # 删完就不能再碰 board 上的任何对象了，Fill/Save 除外。
    for b in moved:
        b["via"]["obj"].SetPosition(pcbnew.VECTOR2I_MM(round(b["nx"], 4), round(b["ny"], 4)))
    ndel = 0
    for b in moved:
        for t in b["kill"]:
            try:
                board.Remove(t["obj"])
                ndel += 1
            except Exception:
                pass                        # 同一段被两个 job 都点名时，第二次会失败
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(PCB)
    print(f"已挪 {len(moved)} 个过孔、删 {ndel} 段走线 → {PCB}")
    print("⚠️ 接着必须跑 route_fix 把删掉的线重布回来，再用**未连通总数**验收；不赚就回退")
else:
    print("（plan 模式，未写盘）")
