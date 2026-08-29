#!/usr/bin/env python3
"""导出 Specctra DSN 给 freerouting——射频走线转成 keepout，数字段绕着它走。

## 为什么是这个架构

三件事凑到一起逼出了它：

1. **射频必须先布。** 射频线要留在 F.Cu（In1 是它的参考面）、不许打过孔、
   在细间距封装出脚处要 neck-down。这些约束在空板上很好满足，在数字线布完之后
   就无解了——实测射频最后布只有 1/6 通，先布是 26/27 通。

2. **freerouting 的 DSN 导入器吃不下已布好的走线。** 喂它 97 段预布射频线就卡死在
   `Opening '...dsn'`，一行都走不下去（早期纯直角路径版本则是 StackOverflowError，
   -Xss256m 无效——同一个 bug 的两副面孔）。二分实测：
     裸板 ✅ ／ +锁 In1 +板边内缩 ✅ ／ +186 缝合过孔 ✅ ／ **+97 段预布射频 ❌卡死**

3. **freerouting 布数字段比别的工具好。** 同一块板：freerouting 出 2 个
   starved_thermal + 4 个 via_dangling、22 处真缺线；KRT 整板出 95 条 clearance
   违例、52 处真缺线，而且把 In2 荒着不用、还偷偷改松 .kicad_pro 的规则来迁就自己。

结论：射频交给 KRT 先布在空板上，**再把这些走线翻译成 keepout 禁布区**喂给
freerouting——它不用"读懂"这些走线，只要知道"这块地方不许走"。射频网络同时从
网表里摘掉，免得它去布已经布好的东西。布完导入时再把真实射频走线合并回来
（见 import_ses.py）。

## 顺带做的两件事
- **锁 In1 为平面**：不锁的话实测 freerouting 在上面布 154 段/625mm，其中 71 段
  横穿射频带——那是在 50Ω 线的参考面上开槽，回流路径直接断掉。
  In2 不锁，放开当走线层（3V3_DIG 负载 ≈150mA，不需要专门一层供电）。
- **板边内缩 0.5mm**：freerouting 不认"铜到板边"这条规则，会一路贴到板框（实测 7 条违例）。

运行：~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 export_dsn.py
"""
import collections
import json
import os
import re
import shutil
import subprocess
import sys

# --plain：不转 keepout，射频留在网表里让 freerouting 自己布，只用
# (circuit (use_layer F.Cu)) 把它限死在顶层。
#
# 为什么保留这个模式并且**默认用它**：同一块板实测——
#   freerouting 一把梭（射频+数字一起排）  未连接 38
#   keepout 两段式（射频先布再转禁布区）    未连接 49~51
# 399 条射频 keepout 在 F.Cu 上划出的禁行走廊，比两段式省下的还多。
# keepout 模式（--keepout）留着，因为它是唯一能拿到射频 neck-down 的路子，
# 等布局宽裕些也许能翻盘。
PLAIN = "--keepout" not in sys.argv

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")   # 副本上跑整条链用
BUILD = os.environ.get("PK_BUILD_DIR") or os.path.join(T, "build")   # 中间产物落在工程内，可核对、不会被系统清掉
os.makedirs(BUILD, exist_ok=True)
TOOLS = os.path.join(T, "tools")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")
PRO = os.path.join(BDIR, "expansion-board-v3.kicad_pro")
NORF = os.path.join(BUILD, "norf.kicad_pcb")                 # 摘掉射频走线的板子，只用来导 DSN
RF_JSON = os.path.join(BUILD, "rf_tracks.json")              # 射频走线快照，import_ses.py 据此合并回来
DSN = os.path.join(BUILD, "exp.dsn")

sys.path.insert(0, TOOLS)
from gen_sch import RF50_NETS                                  # noqa: E402

# --unlock-in1：把 In1 也放开当走线层。实验用。
# In1 默认锁死是因为它是 F.Cu 上 50Ω 射频线的参考面（不锁的话实测 freerouting
# 在上面布 154 段/625mm，其中 71 段横穿射频带）。但射频全在 y=54~72，
# U8 在 y=78~86，两者不重叠——问题是 Specctra 没有"按区域限层"的语法，
# 放开就是整层放开。先量一下代价和收益再决定。
if "--unlock-in1" in sys.argv:
    PLANE_LAYERS = ("In4.Cu",)               # 实验用：解锁 In1（RF 参考面），仅留 In4 平面
else:
    # In3 也锁死。它铺的是 3V3_DIG 电源平面，之前没锁，freerouting 在上面布了
    # 181 段信号，把平面切成 14 块孤岛——KiCad DRC 直接报
    # "Zone [3V3_DIG] on In3.Cu ↔ Zone [3V3_DIG] on In3.Cu" 未连通，
    # 是 3V3_DIG 六项未连通的根因。同一层不能既当电源平面又当信号层。
    # 代价：走线层 4→3（F/In2/B）。B.Cu 原本只用了 24 段（In3 用了 181 段），
    # 有大量余量吸收被赶出来的走线。
    PLANE_LAYERS = ("In1.Cu", "In3.Cu", "In4.Cu")   # 两个 GND 平面 + 3V3_DIG 电源平面
EDGE_CLR_UM = 500          # 与 .kicad_pro 的 min_copper_edge_clearance 一致

# keepout 每侧额外留的间距。射频线旁边留宽一点对信号有好处（减少串扰耦合），
# 但留太宽会把数字线挤没地方走。0.2mm 是板子 clearance 规则的整数倍，先用这个。
RF_KEEPOUT_MARGIN_MM = 0.20


def paren_block(s, start):
    """从 s[start] 的 '(' 开始，返回匹配的右括号位置（含）。"""
    assert s[start] == "(", f"{start} 处不是左括号"
    d = 0
    for i in range(start, len(s)):
        if s[i] == "(":
            d += 1
        elif s[i] == ")":
            d -= 1
            if d == 0:
                return i + 1
    raise AssertionError("括号不闭合")


# ── ① 把射频走线摘出来存档，得到一块"没有射频"的板子 ─────────────────────
board = pcbnew.LoadBoard(PCB)
lname = {board.GetLayerID(n): n for n in ("F.Cu", "In1.Cu", "In2.Cu", "In3.Cu", "In4.Cu", "B.Cu")}

rf_tracks, rf_vias = [], 0
for t in list(board.GetTracks()):
    if t.GetNetname() not in RF50_NETS:
        continue
    if isinstance(t, pcbnew.PCB_VIA):
        rf_vias += 1
        continue
    rf_tracks.append({
        "net": t.GetNetname(),
        "layer": lname[t.GetLayer()],
        "sx": t.GetStart().x, "sy": t.GetStart().y,
        "ex": t.GetEnd().x, "ey": t.GetEnd().y,
        "w": t.GetWidth(),
    })
assert PLAIN or rf_tracks, "板上一段射频走线都没有——keepout 模式要求射频先由 KRT 布好"
assert rf_vias == 0, f"射频网络上有 {rf_vias} 个过孔，会打穿 In1 参考面"
off = {r["layer"] for r in rf_tracks} - {"F.Cu"}
assert not off, f"射频走线跑出 F.Cu: {off}"

# plain 模式射频由 freerouting 自己布，不需要贴回——写空表告诉 import_ses.py 别贴。
with open(RF_JSON, "w") as f:
    json.dump([] if PLAIN else rf_tracks, f)
if not PLAIN:
    print(f"射频走线快照: {len(rf_tracks)} 段 → {RF_JSON}")

# 删除必须在本进程做完就走人：board.Remove() 之后 SWIG 的封装代理会退化成裸
# SwigPyObject，同一进程里再取 pad/footprint 会 AttributeError，重新 LoadBoard
# 也救不回来。所以导 DSN 放到独立子进程。
# **两种模式都要删**：freerouting 吃不下任何已布好的走线，plain 模式也不例外。
# 区别在于 plain 把射频网络留在网表里让它自己布，keepout 把网络摘掉、只留禁布区。
# ⚠️ 下面的 Remove() 会让**整个 pcbnew 模块的 SWIG 类型注册失效**——之后连
# board.GetFootprints() 都直接段错误（SIGSEGV，不是抛异常），而且重新 LoadBoard
# 也救不回来（返回的一样是裸 SwigPyObject）。原注释早就写明了这点。
# 所以后面 ④'(EP 禁布区) 和 via_ring 要用的封装/焊盘信息，必须**在这里先提成纯数据**。
_FP_PADS = {}
for _f in board.GetFootprints():
    _lst = []
    for _p in _f.Pads():
        _bb = _p.GetBoundingBox()
        _lst.append((_p.GetNetname(),
                     _p.GetLayerSet().Contains(pcbnew.F_Cu),
                     _p.IsOnCopperLayer(),
                     _bb.GetLeft(), _bb.GetTop(), _bb.GetRight(), _bb.GetBottom(),
                     _p.GetNumber()))
    _FP_PADS[_f.GetReference()] = _lst

_removed_rf = 0
for t in list(board.GetTracks()):
    if not isinstance(t, pcbnew.PCB_VIA) and t.GetNetname() in RF50_NETS:
        board.Remove(t)
        _removed_rf += 1
board.Save(NORF)

# 这个坑长期潜伏着没爆：只有板上**真的存在 RF50 走线**时上面的 Remove 才会执行。
# route_rf.py 的链路表覆盖不到"天线馈电脚 → π 匹配"这条（天线 pad 是自定义焊盘、
# 锚点在主臂上，pad-to-pad 布不了），所以它一直报"布线 0 段"，Remove 一次都没跑过。
# 2026-08-21 补上 route_ifa_feed.py 之后第一次触发，整条链在第③步 SIGSEGV。
if _removed_rf:
    print(f"射频走线已摘除 {_removed_rf} 段（后续一律走 _FP_PADS，不再碰 board）")
shutil.copy(PRO, NORF.replace(".kicad_pcb", ".kicad_pro"))   # 网络类规则在工程文件里

subprocess.run([sys.executable, "-c",
                f"import pcbnew;b=pcbnew.LoadBoard({NORF!r});"
                f"assert pcbnew.ExportSpecctraDSN(b,{DSN!r})"], check=True,
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

with open(DSN) as f:
    dsn = f.read()

# ── ①.5 规范化排序：让 DSN 逐字节可复现 ──────────────────────────────
# KiCad 的 ExportSpecctraDSN **每次导出的顺序都不一样**——同一块板连导两遍，
# 文件大小一模一样（141259 字节）但 (place …) 条目和 (pins …) 列表的排列不同
# （内部容器遍历顺序不稳定）。顺序变 → freerouting 处理网络的次序变 → 布线结果变：
# 实测同一块板、同样 186 个缝合过孔、同样 212 条待布，跑出 22 unrouted(16m37s)
# 和 30 unrouted(11m28s) 两种结果。
#
# 这会让所有"改了 X 之后变好/变坏"的对比失去意义——差异可能纯粹来自导出顺序。
# 排序后同一输入必然得到同一 DSN，freerouting 对固定 DSN 是确定性的
# （实测同一 DSN 连跑 3 次，32/32/32 一字不差），整条流水线就可复现了。
def _sort_same_level(dsn, pattern, indent):
    """把**连续相邻**的同级 (xxx ...) 块按内容排序后原位放回。

    只对"彼此之间只隔空白"的块分组排序。原因：同一个前缀会出现在不同作用域里
    ——比如 `(via ` 既是 structure 里的过孔**声明**，又是 wiring 里的过孔**实例**，
    一起排会把两个作用域的内容搅在一起。按连续性分组天然把它们隔开。
    """
    spans, i = [], 0
    for m in re.finditer(pattern, dsn, re.M):
        if m.start() < i:
            continue
        end = paren_block(dsn, m.start() + indent)
        spans.append((m.start(), end))
        i = end
    groups, cur = [], []
    for k, (st, en) in enumerate(spans):
        if cur and not dsn[spans[k - 1][1]:st].strip() == "":
            groups.append(cur)
            cur = []
        cur.append((st, en))
    if cur:
        groups.append(cur)

    for g in reversed(groups):          # 从后往前改，前面的下标才不会失效
        if len(g) < 2:
            continue
        seps = {dsn[g[k][1]:g[k + 1][0]] for k in range(len(g) - 1)}
        assert len(seps) == 1, f"块间分隔符不一致: {seps!r}"
        body = seps.pop().join(sorted(dsn[x:y] for x, y in g))
        dsn = dsn[:g[0][0]] + body + dsn[g[-1][1]:]
    return dsn


def _canon(dsn):
    # ① 每个 (component ...) 里的 (place ...) 行排序
    out, i = [], 0
    for m in re.finditer(r"^    \(component ", dsn, re.M):
        if m.start() < i:
            continue
        end = paren_block(dsn, m.start() + 4)
        blk = dsn[m.start():end]
        places = re.findall(r"^      \(place .*\)$", blk, re.M)
        if len(places) > 1:
            first, last = blk.index(places[0]), blk.rindex(places[-1]) + len(places[-1])
            blk = blk[:first] + "\n      ".join(sorted(places)) + blk[last:]
        out.append((dsn[i:m.start()], blk))
        i = end
    if out:
        dsn = "".join(pre + blk for pre, blk in out) + dsn[i:]

    # ② (component ...) 块之间排序
    dsn = _sort_same_level(dsn, r"^    \(component ", 4)

    # ③ 每个 (net ...) 里的 pins 排序
    out, i = [], 0
    for m in re.finditer(r"^    \(net ", dsn, re.M):
        if m.start() < i:
            continue
        end = paren_block(dsn, m.start() + 4)
        blk = dsn[m.start():end]
        # token 里必须排除 ')'——用 \S+ 会把收尾括号吃进最后一个引脚名，
        # 结果重排后写成 "… C27-1) C4-1"，括号错位。
        pm = re.search(r"\(pins((?:\s+[^\s)]+)+)\s*\)", blk)
        if pm:
            lines, line = [], "      (pins"
            for t in sorted(pm.group(1).split()):
                if len(line) + len(t) + 1 > 88:
                    lines.append(line)
                    line = "       "
                line += " " + t
            lines.append(line + ")")
            blk = blk[:pm.start()].rstrip("\n ") + "\n" + "\n".join(lines) + blk[pm.end():]
        out.append((dsn[i:m.start()], blk))
        i = end
    if out:
        dsn = "".join(pre + blk for pre, blk in out) + dsn[i:]

    # ④ 其余同级重复块也排序。
    #    (layer …) **绝不能排**——那是叠层顺序，排了整个板子的层定义就乱了。
    #    (class …) 不排：只有 3 个，且 kicad_default 是兜底类，顺序可能有语义。
    for _pat in (r"^    \(net ", r"^    \(plane ", r"^    \(image ",
                 r"^    \(padstack ", r"^    \(via ", r"^    \(wire "):
        dsn = _sort_same_level(dsn, _pat, 4)
    return dsn


_before_len = len(dsn)
dsn = _canon(dsn)
assert dsn.count("(") == dsn.count(")"), "规范化排序把括号写乱了"
print(f"DSN 规范化排序完成（{_before_len} → {len(dsn)} 字节）")

# ── ② 锁 In1 为平面层 ────────────────────────────────────────────────
for layer in PLANE_LAYERS:
    pat = re.compile(r"(\(layer\s+" + re.escape(layer) + r"\s*\n\s*\(type\s+)signal(\s*\))")
    dsn, n = pat.subn(r"\1power\2", dsn)
    assert n == 1, f"{layer} 的 (type signal) 没找到或匹配了 {n} 处——DSN 格式变了，别静默放过"

# ── ③ 板边内缩 ──────────────────────────────────────────────────────
m = re.search(r"\(boundary\s*\n\s*\(path pcb 0((?:\s+-?\d+)+)\s*\)", dsn)
assert m, "DSN 里找不到 boundary path——格式变了"
c = [int(v) for v in m.group(1).split()]
xs, ys = c[0::2], c[1::2]
cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
new = []
for x, y in zip(xs, ys):                          # 矩形板框：每个顶点朝中心各缩 0.5mm
    new += [int(x - EDGE_CLR_UM if x > cx else x + EDGE_CLR_UM),
            int(y - EDGE_CLR_UM if y > cy else y + EDGE_CLR_UM)]
w = max(xs) - min(xs) - 2 * EDGE_CLR_UM
assert w > 90000, f"内缩后板宽 {w/1000:.1f}mm 异常，boundary 可能不是矩形"
bal = dsn.count("(") - dsn.count(")")
# 只替换 path 本身，不要再补 (boundary 的收尾括号——正则没吃掉它。曾经多写一个 ")"，
# 提前关掉 (structure 块，freerouting 直接报 "0 unrouted nets" 秒退。
dsn = dsn[:m.start()] + "(boundary\n      (path pcb 0  " + \
    " ".join(str(v) for v in new) + ")" + dsn[m.end():]
assert dsn.count("(") - dsn.count(")") == bal, "括号不平衡——DSN 被改坏了"
print(f"板边内缩 {EDGE_CLR_UM/1000}mm：布线区 {w/1000:.1f}×{(max(ys)-min(ys)-2*EDGE_CLR_UM)/1000:.1f}mm")

# DSN 坐标是微米、Y 轴取反（KiCad 的 Y 向下，Specctra 向上）。拿刚解析出来的
# boundary 反推，不猜——板框 X 范围应当等于板宽。
SCALE = 1000.0 / pcbnew.FromMM(1)       # KiCad 内部单位(nm) → DSN 微米


def to_dsn(x, y):
    return f"{x * SCALE:.1f} {-y * SCALE:.1f}"


# ── ④ 射频走线 → keepout 禁布区 ──────────────────────────────────────
ko = []
for r in (() if PLAIN else rf_tracks):
    wid = (r["w"] * SCALE) + 2 * RF_KEEPOUT_MARGIN_MM * 1000
    ko.append(f'    (keepout "" (path {r["layer"]} {wid:.0f}  '
              f'{to_dsn(r["sx"], r["sy"])}  {to_dsn(r["ex"], r["ey"])}))')
if ko:
    print(f"射频 keepout: {len(ko)} 条（线宽 + 每侧 {RF_KEEPOUT_MARGIN_MM}mm）")

# ── ④' 散热焊盘(EP) → 内层禁布区 ─────────────────────────────────────
# EP 要打通孔阵列下到地平面，而通孔穿透**全部 6 层**。之前没留这块地方，
# freerouting 把 In2/B.Cu 的走线直接从 EP 底下横穿过去：实测 U10 的 5.15mm
# 见方 EP 被 5 条 In2 竖线 + 3 条 B.Cu 横线切成 0.35mm 见方的小格，
# **格子本身比过孔还小**，一个孔都打不下。从顶层看那块 EP 空空荡荡，
# 所以我一度以为是打孔算法的问题——不是，是内层被占了。
#
# 只锁 In2/B.Cu 两个信号层，**不锁 F.Cu**：EP 四周的引脚要在顶层扇出，
# 锁了顶层等于把芯片封死。In1/In3/In4 是平面层，本来就不布线。
EP_KEEPOUT_MARGIN_MM = 0.05      # 略外扩，免得走线正好擦着边、把最外圈孔位吃掉
sys.path.insert(0, TOOLS)
from ep_targets import (TARGETS as EP_TARGETS, EP_MIN_SIDE_MM,   # noqa: E402
                        EP_VIAS, PITCH_MM)

eko = []
for _ref, _pads in _FP_PADS.items():      # 纯数据，board 此时已不可用（见上）
    if _ref not in EP_TARGETS:
        continue
    for _pnet, _onf, _oncu, _L, _T, _R, _B, _pnum in _pads:
        if _pnet != "GND" or not _onf:
            continue
        _w, _h = pcbnew.ToMM(_R - _L), pcbnew.ToMM(_B - _T)
        if max(_w, _h) <= EP_MIN_SIDE_MM:
            continue
        # 禁布区**只盖孔阵实际占的中心区域**，不是整个 EP。
        # U10 全铺是 5.25×5.25=27.6mm²，按 4×4@1.2mm 只需 4.2×4.2=17.6mm²，
        # 省下的 0.5mm 环带正好还给内层走线绕行。
        _n = EP_VIAS.get(_ref)
        if _n:
            _k = int(round(_n ** 0.5))
            assert _k * _k == _n, f"{_ref} 的 EP_VIAS={_n} 不是完全平方数"
            _side = (_k - 1) * PITCH_MM + 0.30 + 2 * EP_KEEPOUT_MARGIN_MM  # 阵列跨度+盘径
            assert _side <= min(_w, _h) + 1e-6, \
                f"{_ref} 要 {_k}×{_k}@{PITCH_MM}mm 需 {_side:.2f}mm，EP 只有 {min(_w,_h):.2f}mm"
            cx, cy = (_L + _R) / 2, (_T + _B) / 2
            _hs = pcbnew.FromMM(_side / 2)
            x1, y1, x2, y2 = cx - _hs, cy - _hs, cx + _hs, cy + _hs
        else:
            m2 = pcbnew.FromMM(EP_KEEPOUT_MARGIN_MM)   # 小 EP 整块禁，本来就不占地方
            x1, y1 = _L - m2, _T - m2
            x2, y2 = _R + m2, _B + m2
        for ly in ("In2.Cu", "B.Cu"):
            # DSN 的 Y 轴取反，rect 的上下角会翻个个儿，这里直接排序给 min/max，
            # 不依赖 freerouting 帮忙纠正。
            ax, ay = to_dsn(x1, y1).split()
            bx, by = to_dsn(x2, y2).split()
            lo_y, hi_y = sorted((float(ay), float(by)))
            eko.append(f'    (keepout "" (rect {ly} {ax} {lo_y:.1f} {bx} {hi_y:.1f}))')
        _ks = pcbnew.ToMM(x2 - x1)
        print(f"  EP keepout {_ref}.{_pnum}  EP {_w:.2f}x{_h:.2f} → "
              f"禁布 {_ks:.2f}x{pcbnew.ToMM(y2-y1):.2f}mm ({_ks*pcbnew.ToMM(y2-y1):5.2f}mm²/层)"
              + (f"  {_k}×{_k}孔阵，四周留 {(min(_w,_h)-_ks)/2:.2f}mm 环带给内层" if _n else "  整块禁"))
assert len(eko) >= 8, f"EP keepout 只生成了 {len(eko)} 条，名单或焊盘识别出问题了"
ko += eko

# ── ④'' 细间距 QFN 引脚外沿 → via_keepout（禁过孔、**不禁走线**）─────────
# 评审指出剩下两处未连通是同一个病：扇出过孔就近打，把邻居的逃逸通道堵死了。
# 实测（焊盘尖端到最近过孔的边距）：
#   U8.23  RP_1V1     左边 SWCLK 孔 0.34mm / 3V3_DIG 孔 0.39mm，两孔之间只剩 0.425mm
#   U10.45 SUBG_VDDR  正下方 3V3_DIG 孔 0.45mm
# 而过一条 0.25mm 的 POWER_LO 线要 0.25 + 2×0.15 = **0.55mm**。差 0.1~0.125mm，
# 于是这两脚怎么布都出不去。
#
# 病根不是"某个孔放错了"，是 freerouting 就近打孔时**没有"给邻居留路"这条约束**。
# 与其一个个挪孔，不如把约束交给它：引脚外沿一圈禁止打过孔，但**走线照常通过**。
# 孔被迫外移到更大的半径上，同样的角度间隔换来更长的弧长，交错自然发生，
# 中间那几脚也就有了 F.Cu 直出的走廊。
#
# 只对 0.4/0.5mm pitch 的两颗 QFN 用。间距宽的元件没有这个问题，套上去反而添乱。
# 三种方案的实测对比（同一布局，真缺线数）：
#     不加             2   ← 目前最好
#     实心矩形禁整块   7   ← 连芯片下方可用区和路过走线的换层都封死了
#     口字环带         3   ← SUBG_VDDR/RP_1V1 依旧缺，还多搭进去一条 SW1_J1
# 结论：**光禁止孔打在坏位置是不够的**。评审点出了更根本的一层——顺序问题：
# freerouting 是边布线边打孔，先布的线把后面孔位堵死了；正确顺序是**先把扇出孔
# 按规则放好，再让走线自动避让**。那是 fanout_ring.py 干的活（run_route.sh 步骤②'），
# 用它的时候这个环带必须关掉，否则预放的孔全落在禁区里。
# 留空 = 关闭；要单独试 via_keepout 就填回 ("U8", "U10")。
VIA_RING_REFS = ()
VIA_RING_MM = 0.75          # 环带宽度：约等于 fanout_ring 的 ROW0(0.55) + 盘半径(0.2)

vko = []
for _ref, _pads in _FP_PADS.items():      # 同上，纯数据
    if _ref not in VIA_RING_REFS:
        continue
    xs, ys = [], []
    for _pnet, _onf, _oncu, _L, _T, _R, _B, _pnum in _pads:
        if not _oncu:
            continue
        xs += [_L, _R]
        ys += [_T, _B]
    assert xs, f"{_ref} 一个铜焊盘都没有"
    r = pcbnew.FromMM(VIA_RING_MM)
    px1, py1, px2, py2 = min(xs), min(ys), max(xs), max(ys)      # 焊盘外框
    x1, y1, x2, y2 = px1 - r, py1 - r, px2 + r, py2 + r          # 外扩框
    # ⚠️ 必须是**环**，不能是实心矩形。第一版直接禁整块 9.25×9.25mm，
    # 结果真缺线 2 → 7：芯片本体那 7.75×7.75mm 里，EP 之外、引脚之内的区域
    # 是可用空间，而且路过的走线也要在那儿换层——一并封死，代价远超收益。
    # 只禁引脚**正外方**那圈 0.75mm：上下左右四条矩形拼成"口"字（四角重叠无妨）。
    rings = [(x1, y1, x2, py1), (x1, py2, x2, y2),        # 上、下
             (x1, y1, px1, y2), (px2, y1, x2, y2)]        # 左、右
    for (a, b, c, d) in rings:
        for ly in ("F.Cu", "In2.Cu", "B.Cu"):
            ax, ay = to_dsn(a, b).split()
            bx, by = to_dsn(c, d).split()
            lo_y, hi_y = sorted((float(ay), float(by)))
            vko.append(f'    (via_keepout "" (rect {ly} {ax} {lo_y:.1f} {bx} {hi_y:.1f}))')
    print(f"  via_keepout {_ref} 引脚外沿 {VIA_RING_MM}mm 环带（口字四条）"
          f"  焊盘外框 {pcbnew.ToMM(px2-px1):.2f}mm，中央不禁")
assert len(vko) == len(VIA_RING_REFS) * 12, f"via_keepout 应为 {len(VIA_RING_REFS)} 颗 × 4 条 × 3 层，实得 {len(vko)}"
ko += vko

if ko:
    anchor = dsn.index('    (via "')              # structure 块里 via 声明之前插入
    bal = dsn.count("(") - dsn.count(")")
    dsn = dsn[:anchor] + "\n".join(ko) + "\n" + dsn[anchor:]
    assert dsn.count("(") - dsn.count(")") == bal, "keepout 注入把括号写乱了"
    print(f"keepout 合计 {len(ko)} 条（EP 内层禁布 {len(eko)} + 引脚环禁过孔 {len(vko)}）")

# ── ⑤ 把射频网络从网表和网络类里摘掉 ────────────────────────────────
# 走线已经在了，不能让 freerouting 再去布一遍——它既不该动这些线，
# 也不该在 keepout 之外另找路径把同一个网络重连一次。
if PLAIN:
    # 射频留在网表里，用 (circuit (use_layer F.Cu)) 限死在顶层——In1 是它的参考面，
    # 换层就得打过孔，等于在参考面上开洞。freerouting 支持这个关键字
    # （见 io/specctra/parser/Keyword.class 的常量表）。
    m = re.search(r"(\(class RF50\b.*?)(\n\s*\(rule\b)", dsn, re.S)
    assert m, "DSN 里找不到 RF50 网络类——gen_pcb.py 的网络类规则可能没写进去"
    dsn = dsn[:m.end(1)] + "\n      (circuit\n        (use_layer F.Cu)\n      )" + dsn[m.end(1):]
    print("RF50 类限层 → F.Cu（射频不换层，In1 参考面不被过孔打穿）")
else:
    gone = 0
    for net in sorted(RF50_NETS):
        m = re.search(r"^    \(net " + re.escape(net) + r"\n", dsn, re.M)
        if not m:
            continue
        dsn = dsn[:m.start()] + dsn[paren_block(dsn, m.start() + 4):].lstrip("\n") + ""
        gone += 1
    # 整个 RF50 类连同规则一起删掉（成员全没了，留着是空壳）
    m = re.search(r"^    \(class RF50\b", dsn, re.M)
    if m:
        dsn = dsn[:m.start()] + dsn[paren_block(dsn, m.start() + 4):].lstrip("\n")
    # 其它类的成员列表里可能还残留射频网络名（比如 kicad_default 兜底）。
    # 只在 (class ...) 块内部替换——全 DSN 范围的正则会把 (net XXX) 这类引用也抹掉。
    _names = set(RF50_NETS)
    _out, _i = [], 0
    for m in re.finditer(r"^    \(class ", dsn, re.M):
        if m.start() < _i:
            continue
        end = paren_block(dsn, m.start() + 4)
        blk = dsn[m.start():end]
        head, sep, tail = blk.partition("\n      (rule")   # 成员列表在 (rule 之前
        assert sep, "class 块里找不到 (rule——DSN 格式变了"
        toks = head.split()
        kept = [t for t in toks if t not in _names]
        _out.append(dsn[_i:m.start()] + "    " + " ".join(kept) + sep + tail)
        _i = end
    dsn = "".join(_out) + dsn[_i:]
    print(f"网表里摘掉射频网络: {gone} 个（走线已由 keepout 保护）")

left = [n for n in RF50_NETS if re.search(r"^    \(net " + re.escape(n) + r"\n", dsn, re.M)]
if PLAIN:
    assert len(left) >= 20, f"plain 模式下射频网络应留在网表里，只剩 {len(left)} 个"
else:
    assert not left, f"这些射频网络没摘干净: {left}"
# 网络类不能被误删光——线宽规则全在类里，丢了 POWER 类电源线就掉回 0.2mm 默认宽
cls = re.findall(r"^    \(class (\S+)", dsn, re.M)
# KiCad 9 会把同时命中 netclass_pattern 和 Default 的类导成 "POWER_LO,Default"
# 这种合并名，所以按逗号拆平再比。POWER_LO 是低压轨（0.25mm），必须在——
# 丢了它 3V3 就掉回 Default 的 0.15mm，载流不够。
_flat = set()
for _c in cls:
    _flat.update(_c.split(","))
_must = {"kicad_default", "POWER", "POWER_LO"}
assert _must <= _flat, f"网络类缺失: 有 {_flat}，必须包含 {_must}"
_dbg = {}
for _m in re.finditer(r"^    \(class (\S+)(.*?)^    \)", dsn, re.M | re.S):
    _w = re.search(r"\(width\s+([0-9.]+)\)", _m.group(2))
    _dbg[_m.group(1)] = float(_w.group(1)) / 1000 if _w else None
print("DSN 各类线宽(mm):", {k: v for k, v in sorted(_dbg.items())})

sig = len(re.findall(r"\(type\s+signal\s*\)", dsn))
pwr = len(re.findall(r"\(type\s+power\s*\)", dsn))
_want_sig = 6 - len(PLANE_LAYERS)
assert sig == _want_sig and pwr == len(PLANE_LAYERS), \
    f"层类型不对：signal={sig} power={pwr}，应为 {_want_sig}/{len(PLANE_LAYERS)}"
assert dsn.count("(") == dsn.count(")"), "最终括号不平衡"

# ── 非 45° 预布走线自检 ───────────────────────────────────────────────
# 这是整条链的头号杀手，必须在导出时就报出来，别等 freerouting 卡死 25 分钟。
#
# freerouting 在 autoroute 之后有个「45 度化」后处理，板上**已经存在**的非 45°
# 走线会让它陷进去不出来——日志停在 "after autoroute: N traces not 45 degree"，
# 跑满一个核不退出，SES 一直不生成，前面所有步骤白做。
#
# 2026-08-22 实测：87 条时必卡。来源是我们自己预布的扇出/缝合短接线（过孔放在
# 焊盘斜外侧，直连出来是 30°/60°），已由 track45.py 统一拆成 45°/正交两段。
# 这里只做守门：数字应该是 0，非 0 说明又有谁在画斜线。
_bad = collections.Counter()
_wire = dsn[dsn.find("(wiring"):]
for _m in re.finditer(r'\(wire\s*\(path\s+\S+\s+[\d.]+((?:\s+-?[\d.]+)+)\)\s*'
                      r'\(net\s+"?([^)"]+)"?\)', _wire):
    _c = _m.group(1).split()
    _pts = [(float(_c[k]), float(_c[k + 1])) for k in range(0, len(_c) - 1, 2)]
    for _a, _b in zip(_pts[:-1], _pts[1:]):
        _dx, _dy = abs(_b[0] - _a[0]), abs(_b[1] - _a[1])
        # DSN 单位是 um。容差 1um：低于这个量级是坐标舍入，freerouting 不计较。
        if _dx > 1.0 and _dy > 1.0 and abs(_dx - _dy) > 1.0:
            _bad[_m.group(2)] += 1
            break
if _bad:
    print(f"⚠️ 预布走线里有 {sum(_bad.values())} 条非 45°/正交: {dict(_bad)}")
    print("   freerouting 的 45 度化后处理会卡死在这些线上，SES 出不来。")
    print("   画这些线的地方要改用 tools/track45.py 的 add_track45()。")
else:
    print("预布走线角度自检：全部 45°/正交 ✅")

with open(DSN, "w") as f:
    f.write(dsn)
print(f"DSN: {DSN}  走线层=F.Cu/In2.Cu/B.Cu  平面层={'/'.join(PLANE_LAYERS)}")

# ⚠️ 必须 os._exit 而不是正常返回：上面 board.Remove() 之后那个 board 对象已经废了，
# 解释器退出时析构它会 SIGSEGV——**活儿全干完了、DSN 也写盘了，退出码却是 139**，
# run_route.sh 的 set -e 当场把整条链掐断（2026-08-21 实测）。
# os._exit 跳过解释器的清理阶段，绕开这次析构。stdout 要先手动刷，
# 否则上面这些 print 会连同缓冲区一起丢掉。
sys.stdout.flush()
sys.stderr.flush()
os._exit(0)
