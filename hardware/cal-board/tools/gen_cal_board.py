#!/usr/bin/env python3
"""生成 VNA 校准/转接板（2 层，华秋免费打样档）。

    KiCad 的 python 才有 pcbnew：
    ~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 \
        tools/gen_cal_board.py

## 这块板解决什么

U.FL 没有现成的 SOL 校准件，导致校准面只能停在 VNA 的 SMA 口，尾线（pigtail）
留在测量链路里。2026-09-04 的实测教训：V4.4 板 J6 读到 11.45pF，而理论只有
1.6pF，差 7 倍，一度以为板子坏了——实际是尾线没被校准掉。
详见 hardware/tools/vna_deembed.py。

有了这块板，就能把尾线**真正校准进 VNA**，而不是事后用软件补偿。

## 设计的唯一硬要求：参考面一致

Short / Open / Load 三个标准件到连接器焊盘的距离**必须相同**，否则三者不在
同一参考面上，校准出来的误差模型是错的。本板的做法是**零长度**：标准件直接
贴在连接器的信号焊盘边上，不走任何引线。

    OPEN   信号焊盘什么都不接（只有焊盘自身的边缘电容）
    SHORT  信号焊盘紧邻打 3 个过孔到底层地平面（多孔并联降电感）
    LOAD   信号焊盘紧邻贴 2×100Ω 0402 并联到地（并联降寄生电感）

## 为什么 2 层 1.6mm 板也够用

2 层 1.6mm 板的 50Ω 微带线宽是 **2.692mm**（由嘉立创 2 层官方数据点
h=1.43mm/W=2.6916mm/50Ω 反标定 Dk=4.423 算得），很宽。但本板 1090MHz 的
波导波长是 150.4mm，**λ/20 = 7.5mm** —— 只要每组的两个连接器紧挨着、走线
控制在 8mm 以内，特征阻抗的偏差就不足以影响结果。所以转接组一律短连，
不追求精确 50Ω 线宽。

## 封装来源

    U.FL   Connector_Coaxial:U.FL_Hirose_U.FL-R-SMT-1_Vertical   ← 与主板 J2/J5-J8 同款
    SMA    Connector_Coaxial:SMA_Amphenol_132134_Vertical        ← 通孔四脚方法兰
    R      Resistor_SMD:R_0402_1005Metric

SMA 封装按**厂家工程图纸**绘制（2026-09-08 拿到），不是照商品图猜的：
    法兰 6.5×6.5 · 地脚 0.9×0.9 方脚，外缘 6.0/内缘 4.2 → 中心距 5.10mm
    中心针 Ø1.0 · 总长 13.5 · 牙长 8 · 1/4-36UNS · DC~12.4GHz · 50Ω

一度想直接套用 KiCad 的 SMA_Amphenol_132134_Vertical——脚位确实几乎一样
（5.08 vs 5.10mm，差 0.02mm），但**孔径差太多**：它是 1.50/1.70，而这款针只有
Ø1.0、方脚对角 1.27。Ø1.0 的针在 Ø1.5 的孔里能晃 0.5mm，同轴度受损。
自绘封装取 1.20/1.40，留 0.1~0.2mm 装配间隙。

法兰从 12.7 缩到 6.5mm 还带来一个好处：两个 SMA 能靠得更近，
SMA-SMA 走线因此从 14.0mm 缩到 8.0mm，逼近 λ/20。
"""

import os
import sys

import pcbnew

# ── 板级参数 ────────────────────────────────────────────────────────
BOARD_W, BOARD_H = 95.0, 92.0        # 华秋免费档 10×10cm 以内
EDGE_W = 0.15
COPPER_CLEAR = 0.5                    # 覆铜到信号的间隙
VIA_D, VIA_DRILL = 0.8, 0.4
# 2层1.6mm板的50Ω微带线宽：由嘉立创2层官方数据点(h=1.43/W=2.6916/50Ω)
# 反标定等效Dk=4.423后正算得到。比多层板宽得多，这是2层板的固有代价。
W50 = 2.692
# SMA 垂直座四地脚(中心距5.08、焊盘半径1.125)之间的净空是 2.83mm，
# 留 0.2mm 间距后线最宽只能到 2.43mm。这是几何硬限，不是设计选择。
W_SMA_MAX = 2.40

FP_LIB = os.path.expanduser(
    "~/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints")
UFL = ("Connector_Coaxial", "U.FL_Hirose_U.FL-R-SMT-1_Vertical")
SMA = ("__LOCAL__", "SMA_KHD_Vertical_THT")   # 见 kicad/cal-board.pretty/
R04 = ("Resistor_SMD", "R_0402_1005Metric")

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(T, "kicad", "cal-board.kicad_pcb")


def mm(v):
    return pcbnew.FromMM(v)


def vec(x, y):
    return pcbnew.VECTOR2I(mm(x), mm(y))


LOCAL_LIB = os.path.join(T, "kicad", "cal-board.pretty")


def add_fp(board, lib, name, ref, x, y, rot=0):
    path = LOCAL_LIB if lib == "__LOCAL__" else os.path.join(FP_LIB, lib + ".pretty")
    fp = pcbnew.FootprintLoad(path, name)
    assert fp, f"封装加载失败: {lib}:{name}"
    fp.SetReference(ref)
    fp.SetPosition(vec(x, y))
    if rot:
        fp.SetOrientationDegrees(rot)
    fp.Reference().SetVisible(True)
    fp.Value().SetVisible(False)
    board.Add(fp)
    return fp


def pad_of(fp, num):
    for p in fp.Pads():
        if p.GetNumber() == str(num):
            return p
    raise KeyError(f"{fp.GetReference()} 没有 pad {num}")


def net(board, name):
    ni = board.FindNet(name)
    if ni is None:
        ni = pcbnew.NETINFO_ITEM(board, name)
        board.Add(ni)
    return ni


def track(board, a, b, netinfo, width=0.6, layer=pcbnew.F_Cu):
    t = pcbnew.PCB_TRACK(board)
    t.SetStart(vec(*a))
    t.SetEnd(vec(*b))
    t.SetWidth(mm(width))
    t.SetLayer(layer)
    t.SetNet(netinfo)
    board.Add(t)
    return t


def via(board, x, y, netinfo):
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(vec(x, y))
    v.SetWidth(mm(VIA_D))
    v.SetDrill(mm(VIA_DRILL))
    v.SetNet(netinfo)
    v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
    board.Add(v)
    return v


def text(board, s, x, y, size=1.2, layer=pcbnew.F_SilkS, bold=False):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(s)
    t.SetPosition(vec(x, y))
    t.SetLayer(layer)
    t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
    t.SetTextThickness(mm(size * (0.20 if bold else 0.15)))
    t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_CENTER)
    board.Add(t)
    return t


def edge(board, a, b):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(vec(*a))
    s.SetEnd(vec(*b))
    s.SetLayer(pcbnew.Edge_Cuts)
    s.SetWidth(mm(EDGE_W))
    board.Add(s)


def main():
    board = pcbnew.BOARD()
    ds = board.GetDesignSettings()
    ds.SetCopperLayerCount(2)

    gnd = net(board, "GND")

    # ── 板框 ───────────────────────────────────────────────────
    for a, b in (((0, 0), (BOARD_W, 0)), ((BOARD_W, 0), (BOARD_W, BOARD_H)),
                 ((BOARD_W, BOARD_H), (0, BOARD_H)), ((0, BOARD_H), (0, 0))):
        edge(board, a, b)

    refs = {"J": 0, "R": 0}

    def nref(p):
        refs[p] += 1
        return f"{p}{refs[p]}"

    # ── ① U.FL 的 SOL 三件 ────────────────────────────────────
    # U.FL 的 pad1（信号）在本地坐标 (-1.525, 0)，即元件左侧。
    # 标准件一律贴在 pad1 左边，走线长度 <1.2mm，三组完全一致。
    def ufl_std(kind, x, y):
        ref = nref("J")
        fp = add_fp(board, *UFL, ref, x, y)
        sig = pad_of(fp, 1)
        sx, sy = pcbnew.ToMM(sig.GetPosition().x), pcbnew.ToMM(sig.GetPosition().y)
        # SHORT 的信号脚**就是** GND —— 给它独立网络的话，过孔到了底层
        # 无处可连，DRC 报 via_dangling，而且语义也是错的。
        n = gnd if kind == "SHORT" else net(board, f"UFL_{kind}")
        sig.SetNet(n)
        for p in fp.Pads():
            if p.GetNumber() == "2":
                p.SetNet(gnd)
        if kind == "SHORT":
            # 三个过孔并联，紧贴 pad1 左沿 —— 多孔是为了压低短路电感
            for dy in (-0.9, 0.0, 0.9):
                via(board, sx - 1.3, sy + dy, n)
                track(board, (sx, sy), (sx - 1.3, sy + dy), n)
        elif kind == "LOAD":
            # 2×100Ω 并联 = 50Ω，寄生电感减半
            for dy in (-1.1, 1.1):
                rr = nref("R")
                # 镜像：下面那颗转 90°、上面那颗转 270°，两颗的信号脚都朝
                # 中间（U.FL 走线侧），接地脚都朝外，过孔才不会互相打架。
                rfp = add_fp(board, *R04, rr, sx - 2.2, sy + dy,
                             90 if dy < 0 else 270)
                rfp.SetValue("100")
                pad_of(rfp, 1).SetNet(n)
                pad_of(rfp, 2).SetNet(gnd)
                p1 = pad_of(rfp, 1).GetPosition()
                track(board, (sx, sy),
                      (pcbnew.ToMM(p1.x), pcbnew.ToMM(p1.y)), n)
                p2 = pad_of(rfp, 2).GetPosition()
                g2 = (pcbnew.ToMM(p2.x), pcbnew.ToMM(p2.y))
                g1 = (pcbnew.ToMM(p1.x), pcbnew.ToMM(p1.y))
                away = 1.0 if g2[1] >= g1[1] else -1.0   # 背离信号脚的方向
                via(board, g2[0], g2[1] + away * 1.0, gnd)
                track(board, g2, (g2[0], g2[1] + away * 1.0), gnd)
        # OPEN 什么都不接
        text(board, f"U.FL {kind}", x, y + 4.6, 1.2, bold=True)
        return fp

    text(board, "1) U.FL SOL  -  calibrate the pigtail", 30, 8.0, 1.6,
         bold=True)
    for i, k in enumerate(("OPEN", "SHORT", "LOAD")):
        ufl_std(k, 16 + i * 18, 20)

    # ── ② SMA 的 SOL 三件 ─────────────────────────────────────
    # SMA 垂直座 pad1 在中心，四个地脚在 ±2.54。标准件贴在 -x 侧。
    def sma_std(kind, x, y):
        ref = nref("J")
        fp = add_fp(board, *SMA, ref, x, y)
        sig = pad_of(fp, 1)
        sx, sy = pcbnew.ToMM(sig.GetPosition().x), pcbnew.ToMM(sig.GetPosition().y)
        n = gnd if kind == "SHORT" else net(board, f"SMA_{kind}")
        sig.SetNet(n)
        for p in fp.Pads():
            if p.GetNumber() == "2":
                p.SetNet(gnd)
        if kind == "SHORT":
            for dy in (-0.9, 0.0, 0.9):
                via(board, sx - 4.4, sy + dy, n)
                track(board, (sx, sy), (sx - 4.4, sy + dy), n)
        elif kind == "LOAD":
            for dy in (-1.1, 1.1):
                rr = nref("R")
                # 镜像：下面那颗转 90°、上面那颗转 270°，两颗的信号脚都朝
                # 中间（U.FL 走线侧），接地脚都朝外，过孔才不会互相打架。
                rfp = add_fp(board, *R04, rr, sx - 5.2, sy + dy,
                             90 if dy < 0 else 270)
                rfp.SetValue("100")
                pad_of(rfp, 1).SetNet(n)
                pad_of(rfp, 2).SetNet(gnd)
                p1 = pad_of(rfp, 1).GetPosition()
                track(board, (sx, sy),
                      (pcbnew.ToMM(p1.x), pcbnew.ToMM(p1.y)), n)
                p2 = pad_of(rfp, 2).GetPosition()
                g2 = (pcbnew.ToMM(p2.x), pcbnew.ToMM(p2.y))
                g1 = (pcbnew.ToMM(p1.x), pcbnew.ToMM(p1.y))
                away = 1.0 if g2[1] >= g1[1] else -1.0
                via(board, g2[0], g2[1] + away * 1.1, gnd)
                track(board, g2, (g2[0], g2[1] + away * 1.1), gnd)
        text(board, f"SMA {kind}", x, y + 8.6, 1.2, bold=True)
        return fp

    text(board, "2) SMA SOL  -  skip if your VNA kit has them",
         33, 36.0, 1.6, bold=True)
    for i, k in enumerate(("OPEN", "SHORT", "LOAD")):
        sma_std(k, 20 + i * 18, 50)

    # ── ③ 转接 / THRU ─────────────────────────────────────────
    # 两端连接器紧挨着，走线 <8mm(λ/20)，故不追求精确 50Ω 线宽。
    def link(tag, a_kind, b_kind, x, y, gap):
        """两端连接器**信号脚朝内**相对放置，把走线压到最短。

        U.FL 的 pad1 在本体中心偏 -1.525mm，所以左件转 180°、右件保持 0°，
        两个 pad1 才会面对面；一开始两边都朝外，走线白白多出 3.05mm。
        SMA 的 pad1 在正中心，朝向不影响长度。
        """
        n = net(board, f"THRU_{tag}")
        sigs = []
        for kind, dx, rot in ((a_kind, -gap / 2, 180), (b_kind, gap / 2, 0)):
            lib = UFL if kind == "UFL" else SMA
            fp = add_fp(board, *lib, nref("J"), x + dx, y, rot)
            sig = pad_of(fp, 1)
            sig.SetNet(n)
            for p in fp.Pads():
                if p.GetNumber() == "2":
                    p.SetNet(gnd)
            sigs.append(sig)
        p1 = (pcbnew.ToMM(sigs[0].GetPosition().x),
              pcbnew.ToMM(sigs[0].GetPosition().y))
        p2 = (pcbnew.ToMM(sigs[1].GetPosition().x),
              pcbnew.ToMM(sigs[1].GetPosition().y))
        d = abs(p2[0] - p1[0])
        # 走线越长，特征阻抗越要紧。λ/20=7.5mm 是分界：
        #   短于它 → 电长度不足 18°，用较窄的线即可，失配可忽略
        #   长于它 → 该上 50Ω 线宽（2层1.6mm算得 2.692mm）
        # ⚠️ 但 SMA 垂直座的四个地脚中心距只有 5.08mm、焊盘半径 1.125mm，
        #    净空 2.83mm，留 0.2mm 间距后**最宽只能走 2.43mm**(=53.0Ω)。
        #    2.692mm 塞不下（实测间距只剩 0.069mm）。取 2.40mm 是这个物理约束
        #    与阻抗之间的折中，不是随手写的数。
        w = min(W50, W_SMA_MAX) if d > 7.5 else 1.2
        # ⚠️ 端点不能落在 pad 中心：走线端帽是圆的，半径=线宽/2，会伸进
        #    U.FL 封装自带的 top_keepout（其右边界离 pad1 中心只有 0.535mm）。
        #    往内缩 0.35mm，既仍与焊盘重叠导通，又让端帽退出禁布区。
        sh = 0.35
        a = (p1[0] + sh, p1[1]) if p2[0] > p1[0] else (p1[0] - sh, p1[1])
        bb = (p2[0] - sh, p2[1]) if p2[0] > p1[0] else (p2[0] + sh, p2[1])
        track(board, a, bb, n, width=w)
        text(board, f"{tag}  {d:.1f}mm", x, y + 9.6, 1.2, bold=True)
        return d, w

    text(board, "3) THRU / adapters", 78, 8.0, 1.6, bold=True)
    d1 = link("UFL-SMA", "UFL", "SMA", 78, 20, 9.0)
    d2 = link("UFL-UFL", "UFL", "UFL", 22, 78, 6.5)
    d3 = link("SMA-SMA", "SMA", "SMA", 62, 78, 8.0)

    # ── 说明丝印 ───────────────────────────────────────────────
    text(board, "Pilot Kit  VNA CAL BOARD  2L/1.6mm", 30, 67.0, 1.5, bold=True)
    text(board, "LOAD = 2x100R parallel    SHORT = 3 vias", 30, 70.5, 1.1)

    # ── 双面地覆铜 ─────────────────────────────────────────────
    for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
        z = pcbnew.ZONE(board)
        z.SetLayer(layer)
        z.SetNet(gnd)
        z.SetLocalClearance(mm(COPPER_CLEAR))
        z.SetMinThickness(mm(0.20))
        z.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL)   # 校准件要低电感，不用热焊盘
        o = z.Outline()
        o.NewOutline()
        for x, y in ((0.2, 0.2), (BOARD_W - 0.2, 0.2),
                     (BOARD_W - 0.2, BOARD_H - 0.2), (0.2, BOARD_H - 0.2)):
            o.Append(mm(x), mm(y))
        board.Add(z)

    # 缝合过孔：把两层地缝起来。**必须避开元件**——第一版直接按网格打，
    # 有一个正好扎进 J12(SMA) 的引脚孔（hole_to_hole 实际间距 0.000mm）。
    occupied = []
    for fp in board.GetFootprints():
        bb = fp.GetBoundingBox()
        occupied.append((pcbnew.ToMM(bb.GetLeft()) - 1.5,
                         pcbnew.ToMM(bb.GetTop()) - 1.5,
                         pcbnew.ToMM(bb.GetRight()) + 1.5,
                         pcbnew.ToMM(bb.GetBottom()) + 1.5))

    def free(x, y):
        return not any(x0 <= x <= x1 and y0 <= y <= y1
                       for x0, y0, x1, y1 in occupied)

    n_st = 0
    for x in range(6, int(BOARD_W) - 4, 10):
        for y in (3.0, BOARD_H - 3.0):
            if free(x, y):
                via(board, x, y, gnd)
                n_st += 1

    # ⚠️ ZONE_FILLER 在**从零 BOARD() 建**的板子上会 segfault（退出码 139）——
    # 这种板缺少 LoadBoard 才会做的那部分初始化。项目里其他脚本没踩到，是因为
    # 它们一律 LoadBoard 已有板子。解法：先存盘，重新加载，再填充。
    board.Save(OUT)
    board = pcbnew.LoadBoard(OUT)
    pcbnew.ZONE_FILLER(board).Fill(board.Zones())
    board.Save(OUT)

    n_ufl = sum(1 for f in board.GetFootprints()
                if "U.FL" in str(f.GetFPID().GetLibItemName()))
    n_sma = sum(1 for f in board.GetFootprints()
                if "SMA" in str(f.GetFPID().GetLibItemName()))
    n_r = sum(1 for f in board.GetFootprints()
              if "R_0402" in str(f.GetFPID().GetLibItemName()))
    unfilled = [z for z in board.Zones() if z.CalculateFilledArea() == 0]
    assert not unfilled, f"{len(unfilled)} 个覆铜没填上"
    print(f"→ {OUT}")
    print(f"   板框 {BOARD_W}x{BOARD_H}mm (2层/1.6mm)")
    print(f"   U.FL 座 {n_ufl}  SMA 座 {n_sma}  0402 电阻 {n_r}(=100R x{n_r})"
          f"  缝合过孔 {n_st}")
    print("   THRU 组（λ/20=7.5mm 是「要不要管阻抗」的分界）:")
    for tag, (d, w) in (("UFL-SMA", d1), ("UFL-UFL", d2), ("SMA-SMA", d3)):
        ok = "短线,可忽略" if d <= 7.5 else "已用50Ω线宽"
        print(f"      {tag:8s} 走线 {d:5.2f}mm  线宽 {w:.3f}mm   {ok}")
    assert n_ufl and n_sma, "连接器没放进去"


if __name__ == "__main__":
    main()
