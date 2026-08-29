#!/usr/bin/env python3
"""生成板载 1090MHz IFA 天线封装 ANT_IFA_1090MHz.kicad_mod。

⚠️ **当前实现状态（2026-08-24）**：本生成器已采用完整六层HFSS + V1双点校准的冻结输入：
50.2mm切短目标、52.0mm PCB画长、53.5mm铜箔外包络，并把1.5mm长的
1.5→0.15mm taper做进pad1的自定义铜箔。运行后会同步封装库和`ifa_geom.py`；
PCB内嵌ANT1由`integrate_ifa_v4.py`可重复同步。集成必须按
`../../expansion-board-v3/IFA_HFSS_2026-08-24.md` §8执行。

几何源自 `docs/jlc` 工程里的 `1090_MHz_IFA_ANT`（tools/extract_ifa.py 从 LCEDA 解析）。

🔴 **原先写在这里的前提"原样复用你已调好的天线"是错的，2026-08-21 被实测推翻。**
   那根天线**从来没有在 1090MHz 上调准过**：

       2.4 / 4.3 两块 2 层板（几何逐值比对完全一致，41mm）  实测谐振 1.43GHz
       v3 6 层板（44mm）                                    实测谐振 1.20GHz

   两块都离 1090 差 10~30%。"已调好"这句话进了 extract_ifa.py 和本文件的注释，
   又从注释变成"不许改"的禁令，挡了三个版本。**照抄一个没人验证过的东西，
   抄得再精确也没用。**

历史几何（馈点为原点，KiCad 口径 y 向下；下方数值为 V3.5，仅用于解释来源）：
  辐射臂     (-43.300, -5.988) → (  4.988, -5.988)   1.5mm 宽，顶层
  馈电竖枝   ( -0.012,  0.012) → ( -0.012, -5.988)
  短路竖枝   (  4.988,  0.012) → (  4.988, -5.988)
  短路引线   (  4.988, -0.023) → (6.454,-0.023) → (7.747, 1.27)  0.508mm 宽
  pad1 馈点 (0, 0)，pad2 短路点 (4.988, -0.023)，均 1.524×1.524 顶层 SMD
  铜箔包络 49.800 × 7.500 mm

═══ V3.5 改了三处，依据如下 ═══════════════════════════════════════════════

① **辐射臂加长 5.8mm**（开路端 -37.500 → -43.300，中心线跨度 42.488 → 48.288）
   依据：v3 装盒实测 1.220GHz，电长 48.48mm → λ/4=61.5mm 反推 εeff≈1.61。
   要落到 1090MHz 需电长 68.8mm，即物理 54.2mm，缺 5.8mm。
   3mm 全部加在辐射臂上——IFA 的谐振长度就是这条臂。
   ⚠️ 这是**下限**：开路端净空同时放大后频率会回升，可能还要再加。

② **短路脚从"实心泡在铺铜里"改成"引线 + 隔离槽"**（本次唯一有硬实测背书的改动）
   V3.4 实测：pad2 落在 F.Cu GND 灌铜多边形内 = 电流就地入地，有效 D 只有 4.988mm。
   用户在实板上做的对照实验：

       短路脚实心接铺铜（原状）  R = 25.79Ω
       6mm 铜线引地              R = 38.70Ω
       彻底切断（无短路支路）    R = 57.56Ω

   单调上升，因果闭合：**短路支路电感决定输入阻抗**。4.3 靠 0.508mm 引线走 3.295mm
   再打过孔，有效 D = 7.85mm（算到过孔，不是算到 pad）；v3 那条 0.15mm×0.75mm 引线
   被同网络铺铜整个旁路，等于没画。
   ⚠️ 引线长度照抄 4.3 的 3.295mm，但**这个值对 6 层板大概率偏短**：按传输线算它只给
   +j7.4Ω，而 6mm 悬空线给约 33Ω 才把 R 抬到 38.7。两块板的"引线长度→阻抗"斜率对不上
   （4.3 用 7.4Ω 拿到 58Ω，v3 用 33Ω 只拿到 38.7Ω），差异来自馈点对地电容 16 倍之差、
   频率、装盒态。**没法从 4.3 推，也没法从公式推，只能在 v3 上实测。**
   用户 2026-08-21 明确选择本版不做可调过孔位，故定长。下一版若要收敛，
   应在引线沿途留多个过孔位。

③ **开路端净空 1.0 → 3.95mm**，且左右不对称（见 KEEP_OPEN / KEEP_SHORT）

═══ 净空的真相（我在这上面错过一次，写下来免得再错）═════════════════════

4.3 原板的禁铜区是**一个横跨整块板的矩形**（extract_ifa.py 实测）：

    矩形 mil x[369.8465, 4304.5276] y[2377.7835, 2602.0866] → 99.941 × 5.697 mm

板框是 x[367.52, 4304.53]，也就是从板左边一路通到板右边。折算成天线周围的净空：

                    纵向(辐射臂→铺铜)   开路端外侧
        4.3 原板          4.0mm           14.7mm
        v3 (V3.4)         4.0mm            1.0mm     ← 横向缩了 14.7 倍
        v3 (V3.5)         4.0mm            3.95mm

**纵向 v3 一直是抄对的**，问题只在横向：v3 把那条通长带缩成了只比天线大 1mm 的小框。
开路端是整根天线电压最高、对周边金属最敏感的一头。

做不到 14.7mm 的原因是硬约束，不是疏忽：右边 H2 螺丝孔孔心 146/孔边 144.65，
四个孔是 92×50 阵列、和外壳及对扣板配合，动不了；左边那条板边上还站着
J8/J2(U.FL)、U17、C57/C58/C59。本版靠天线整体左移 6mm 腾出 3.95mm。

（⚠️ 我曾把 **2.4 板**那个阶梯形禁铜区错当成 4.3 的，得出"纵向也漏抄了 13.4mm"的
  结论——那是错的。2.4 是首次打样、参数吃不准，用户明确说过不要参考。认板子看馈点
  坐标：4.3 的 ANT 在 (2335,2295)mil，2.4 的在 (2640,2925)mil。）

**X 镜像**：原设计开路端在馈点左侧。本板翻面对扣后，基准板 ESP32-C6 的 2.4GHz 天线
落在本板**最左**边缘（X≈−1.3..4.0mm，子 agent 用图纸手性+13.45 标注+玻璃悬出三条证据定的），
开路端是辐射最强的一头，必须甩到最右；同时馈点要靠近 1090 开关缩短射频走线。
整体镜像同时满足这两点。镜像是刚体变换，不改变电长度，不影响调好的中心频率。
（你现有那块板恰恰放反了：开路端距 C6 天线投影仅 10.8mm。）

**为什么必须做成 footprint 而不是板级图形**：IFA 的馈点与短路点在结构上直接导通
（倒 F 的定义；也正因如此，设计文档 §11.4 警告偏置 Tee 挂公共口会把 3.3V 短到地）。
只有把连接铜箔放进 footprint 内部，KiCad 才允许它连通两个不同网络的自家焊盘而不报短路——
这也是原 LCEDA 工程的做法（铜箔是封装内 POLY，两个 pad 各挂各的网络）。

**禁铜带纵向**用 4.3 板的配方：5.697mm 高，下沿在馈点上方 1.976mm——即天线根部约 2mm
仍压在 GND 铺铜上。这是 IFA 的正确形态（地平面必须顶到天线根部），不是画漏了。
但**焊盘也落在这条带外**，所以 V3.5 单独给两个脚加了局部隔离槽，见下方 _keepout 三处。

⚠️ 用户 2026-08-02 澄清：**4.3 的净空是后来专门收敛过的，不是"简化"**——2.4 那版
全部挖空过于浪费，所以 4.3 做了适当调整。2.4 是首次打样，很多参数吃不准，
**连天线长度都是错的**。故：以 4.3 为准，2.4 的做法不要参考。

═══ V3.5 遗留的未验证项（别当成已解决）═════════════════════════════════════

  · 加长 5.8mm 是从单点实测线性外推的，dF/dL 斜率从未实测过
  · 短路引线 3.295mm 照抄 4.3，按算式对 6 层板偏短（见 ② 的警告）
  · In1 开窗纯几何推算，无先例、无实测，且方向上会让虚部更感性
  · 效率完全未知——VNA 只测阻抗匹配，测不出辐射效率。SWR 2.0 的失配损失仅 0.51dB，
    不是主要矛盾；真正的风险是靠末端容性加载"借"来的频率会牺牲效率。
    **唯一的判据是贴片后实收 1090 报文，并与 J6 外接天线做 A/B 对比。**

运行：~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 gen_ifa_footprint.py
"""
import os

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(T, "kicad", "expansion-board-v3.pretty", "ANT_IFA_1090MHz.kicad_mod")

W = 1.5                      # 铜箔线宽（原 59.0551 mil）
HW = W / 2
RF_W = 0.15                  # JLC06161H-3313六层叠层的50Ω微带线宽
PAD = 1.524                  # 焊盘边长（原 60 mil）
MIRROR = True                # X 镜像：开路端甩到右侧，远离 C6 2.4G 天线

# ── 完整六层预研冻结尺寸：PCB故意画长2mm，实板从开路端切短 ────────────────
# ⚠️ 尺寸口径以**铜箔外沿**为准（2026-08-21 用户指定）。三种量法差 0.75–1.5mm，
# 之前就是因为口径不一致，量出来主臂 60、腿 8.2，对不上设计值。
#   主臂外沿长 = ARM_L + 2×HW      （两端各一个端帽）
#   腿包络高   = LEG_H + 2×HW
ARM_OUT = 53.500             # 主臂**铜箔外沿→外沿** = 52.0mm中心线画长 + 两端各0.75mm。
                             # 历史D=5.0三点目标50.187mm；实际D=4.988/H=6修正50.230mm，
                             # 0.1mm制造口径仍取50.2mm；
                             # 开路端额外1.8mm故意画长待切：
                             # 短了补不回来。打样后接 NanoVNA 从末端一点点切，
                             # 波谷落 1090 时的物理长度就是这套叠层下的答案。
LEG_OUT = 7.500              # 腿**铜箔包络高**。照抄旧板实测值（旧板腿中心线 6.0mm）。
                             # 决定环路面积→带宽与辐射效率，经验区间 7–9mm。
CLEAR = 2.000                # 天线四周绝对无铜区（经验 1.5–2mm，取上限）

ARM_L = ARM_OUT - 2 * HW     # 主臂中心线跨度（短路脚中心 → 开路端中心）
LEG_H = LEG_OUT - 2 * HW     # 腿中心线长度（主臂中心 → 腿末端中心）
assert abs(ARM_L - 52.0) < 1e-9, "冻结制造尺寸必须画52.0mm中心线，不能误用50.2mm切短目标"

ARM_OPEN = 4.988 - ARM_L     # 辐射臂开路端 x
LEG_END = LEG_H - 5.988      # 脚末端 y（主臂在 y=-5.988）

# 几何（馈点原点，y 向下）。**三段全部 1.5mm 等宽**——线宽突变会引入寄生电感，
# 主臂/馈电脚/短路脚任何一段变细都会毁掉尺寸。
SEGS = [
    ((ARM_OPEN, -5.988), (4.988, -5.988)),  # 辐射臂
    ((-0.012, LEG_END), (-0.012, -5.988)),  # 馈电竖枝
    ((4.988, LEG_END), (4.988, -5.988)),    # 短路竖枝
]
FEED = (0.0, 0.0)            # 馈点几何参考（定义 D），**不是** pad 锚点
SHORT = (4.988, -0.023)      # 两脚中心距 D=4.988mm（用户口径的"5mm"，经验区间 4.5–5.5 正中）

# 焊盘**不许比线宽粗、也不许伸出腿末端**。
# 原来 1.524mm 方盘骑在腿上，两个后果：① 腿的可见长度被撑长（量出来 8.2 而非 7.5）；
# ② 盘比线宽 1.5mm 还宽 0.024mm，是一处线宽突变，本身就带寄生。
# 经验规则原话：焊盘不要作为天线脚的延伸悬空在净空区内；调试点放到 50Ω 传输线上去
# （本板即 IFA_MATCH 上的 U.FL）。所以这里 pad 退化成"腿上的一小段"：
#   · 尺寸 = 线宽，融进腿里看不出来
#   · 锚点放**腿的中点**，anchor 方块上下都不会露出腿末端
PAD_SZ = W
_LEG_MID = (LEG_END + (-5.988)) / 2
PAD_FEED = (-0.012, _LEG_MID)     # 与馈电竖枝同一条中心线
PAD_SHORT = (4.988, _LEG_MID)

# 禁铜带：**必须把整根脚全包进去**，下沿 = 脚末端。
# r1 沿用的 -1.976 是错的：那让两个焊盘和约 2mm 竖脚泡在 GND 铺铜里，
# 脚还没走完就被地"淹没"，电感量塌掉——正是 V3.4 实测 25.79Ω 的成因。
# 正确形态是脚全程暴露在无铜区，走到净空边缘才结结实实撞进大面积 GND。
KEEPOUT_Y = (-7.673, LEG_END)
KEEP_OPEN = KEEP_SHORT = CLEAR


def mx(p):
    return (-p[0], p[1]) if MIRROR else p


SEGS = [(mx(a), mx(b)) for a, b in SEGS]
FEED, SHORT = mx(FEED), mx(SHORT)
PAD_FEED, PAD_SHORT = mx(PAD_FEED), mx(PAD_SHORT)

xs = [c for a, b in SEGS for c in (a[0], b[0])]
ys = [c for a, b in SEGS for c in (a[1], b[1])]
# 焊盘已退化成腿上的一小段（等宽、在腿内），包络完全由走线端帽决定 → 等于外沿口径
_px = [p[0] for p in (PAD_FEED, PAD_SHORT)]
_py = [p[1] for p in (PAD_FEED, PAD_SHORT)]
BB = (min(min(xs) - HW, min(_px) - PAD_SZ / 2), min(min(ys) - HW, min(_py) - PAD_SZ / 2),
      max(max(xs) + HW, max(_px) + PAD_SZ / 2), max(max(ys) + HW, max(_py) + PAD_SZ / 2))
_ENV_W, _ENV_H = ARM_OUT, LEG_OUT
assert abs((BB[2] - BB[0]) - _ENV_W) < 0.01, f"铜箔包络宽 {BB[2]-BB[0]:.3f} != {_ENV_W:.3f}"
assert abs((BB[3] - BB[1]) - _ENV_H) < 0.01, f"铜箔包络高 {BB[3]-BB[1]:.3f} != {_ENV_H:.3f}"
assert abs((max(xs) - min(xs)) - ARM_L) < 0.01, "主臂中心线跨度 != ARM_L——谐振长度被破坏"
# 脚必须全程在净空里：净空下沿不得高于脚末端
assert KEEPOUT_Y[1] >= LEG_END - 1e-6, "禁铜带下沿高于脚末端——脚会被 GND 铺铜淹没"

# 禁铜带真正的下沿 = **脚末端铜箔边缘**（含端帽），不是脚的中心线末端。
_KO_BOT = KEEPOUT_Y[1] + HW
# 馈电脚出线通道必须容纳1.5→0.15mm taper。HFSS模型的顶层地槽半宽是1.2mm，
# 因此这里锁2.4mm总宽：taper宽端1.5mm，两侧各留0.45mm无铜间隙，与已跑六层模型一致。
# 旧值1.2mm只够0.15mm微带，1.5mm taper会各压进tracks_not_allowed区0.15mm。
_CH_W, _CH_D = 2.4, 1.5          # 通道宽 / taper长度（从馈电脚真实铜边开始）
assert _CH_W >= W, "馈线通道比taper宽端还窄，渐变走线会撞全层tracks_not_allowed"
assert abs((_CH_W - W) / 2 - 0.45) < 1e-9, "通道必须与HFSS每侧0.45mm顶层地间隙一致"
_ch_x = PAD_FEED[0]
# taper必须从馈电脚的**真实铜箔外沿**开始，而不是从6.0mm中心线端点开始。
# 馈电脚中心线端点到铜箔外沿还有HW=0.75mm；这段保持1.5mm等宽。旧几何让taper
# 与端帽重叠0.75mm，布尔并集后只剩后0.75mm渐变，并在铜箔外沿产生1.5→0.825mm突变。
_TAPER_START = (_ch_x, _KO_BOT)
_TAPER_END = (_ch_x, _TAPER_START[1] + _CH_D)
assert abs(_TAPER_START[1] - LEG_END - HW) < 1e-9, "taper必须从馈电脚铜箔外沿开始"
assert abs(_TAPER_END[1] - _TAPER_START[1] - 1.5) < 1e-9, "taper长度必须锁1.5mm"

# 禁铜区横向范围。镜像后开路端落在 BB[2] 一侧，未镜像时在 BB[0] 一侧——
# 余量跟着开路端走，不是跟着左右走。
if MIRROR:
    KX0, KX1 = BB[0] - KEEP_SHORT, BB[2] + KEEP_OPEN
else:
    KX0, KX1 = BB[0] - KEEP_OPEN, BB[2] + KEEP_SHORT

s = ['(footprint "ANT_IFA_1090MHz"',
     '\t(version 20231120)',
     '\t(generator "gen_ifa_footprint.py")',
     '\t(layer "F.Cu")',
     # net tie：声明 pad1(馈点) 与 pad2(短路点) 是**有意**短接的。
     # IFA 的两个端子在天线铜箔内部本就直接导通（倒 F 的定义），
     # 不声明的话 KiCad 判 shorting_items + solder_mask_bridge 各 2 条。
     '\t(attr smd allow_missing_courtyard)',
     '\t(net_tie_pad_groups "1, 2")',
     f'\t(descr "1090MHz 板载 IFA（完整六层rev2预研冻结画长）。主臂 {ARM_L}mm(名义切1.8mm至50.2mm；连续铜箔可按VNA继续切短) / 脚 {LEG_H}mm / '
	     f'两脚中心距 4.988mm / 全段 {W}mm 等宽 / 四周净空 {CLEAR}mm。'
	     f'包络 {BB[2]-BB[0]:.1f}x{BB[3]-BB[1]:.1f}mm。脚全程在净空内，末端撞入 GND 铺铜。'
	     f'馈电腿铜箔外沿后含1.5mm长 {W}→{RF_W}mm taper（几何rev2）。'
	     f'已 X 镜像使开路端远离 C6 2.4G 天线。")',
     '\t(tags "antenna IFA 1090MHz ADS-B")',
     '\t(property "Reference" "J**" (at 0 3.5 0) (layer "F.SilkS")',
     '\t\t(effects (font (size 1 1) (thickness 0.15))))',
     '\t(property "Value" "ANT_IFA_1090MHz" (at 0 5.2 0) (layer "F.Fab")',
     '\t\t(effects (font (size 1 1) (thickness 0.15))))']

# 天线铜箔做成**自定义形状焊盘**，不用 fp_line 图形。
# 原因：KiCad 的 net tie 只在**焊盘之间**生效，封装内的图形铜箔拿不到网络，
# DRC 仍判 "<no net> 铜箔短接 GND"（实测）。把铜箔本身做进焊盘就没有无网络铜箔了。
# 拆分：pad1(馈点网络) = 馈电竖枝 + 辐射臂；pad2(GND) = 短路竖枝。两者在 x=短路枝右缘相接，
# 由 net_tie_pad_groups 放行——这正是 IFA 馈点与短路点本就导通的物理事实。
# 层只给 F.Cu：不开阻焊、不上锡膏。原设计的辐射臂也是被阻焊盖住的（图形铜箔默认覆膜），
# 调好的中心频率是在"有阻焊"条件下测的，开窗反而会失谐；无锡膏则避免整条臂被印上锡。
_ax0, _ax1 = min(xs) - HW, max(xs) + HW          # 辐射臂 x 跨度（含线宽端帽）
_arm_y0, _arm_y1 = -5.988 - HW, -5.988 + HW
_sh_x = SHORT[0]                                  # 短路竖枝中心 x
_fd_x = PAD_FEED[0]   # 馈电竖枝中心 x —— 直接取 pad 锚点，别再自己算一遍符号
# ⚠️ 这里原本写 `FEED[0] + (0.012 if not MIRROR else -0.012)`，是错的：
# FEED 已经过 mx() 镜像，再叠一次 ±0.012 等于符号翻反，算出 -0.012 而 pad 锚点在 +0.012。
# 两者差 0.024mm，anchor 方块比 primitives 偏出去一点点，在板上看就是竖枝侧面
# **凸出来一小条**（用户 2026-08-22 在 KiCad 里一眼看出来的那个）。
_stub_y0, _stub_y1 = -5.988, LEG_END + HW
# ⚠️ 竖枝要画到**末端边缘**（LEG_END + 半线宽），不能只画到中心线。
# LEG_END 同时是净空区下沿，若铜箔止步于此：
#   · 短路脚只是和 GND 铺铜**相切**，靠不住；
#   · 馈电脚末端没有伸出净空的部分，50Ω 走线接不上（实测 DRC 报
#     'Pad 1 of ANT1 ↔ Pad 1 of ZP1' 未连通，走线起点落在铜箔外 0.125mm）。
# 画上端帽后脚末端伸出净空 0.75mm，短路脚结结实实撞进铺铜，馈电脚也接得上线。
# 包络高不变（BB 本来就按 max(ys)+HW 算，一直是 7.5mm）。


def _poly(pts, ox, oy):
    """相对焊盘锚点的多边形"""
    return "(gr_poly (pts " + " ".join(f"(xy {x-ox:g} {y-oy:g})" for x, y in pts) + \
           ") (width 0) (fill yes))"


# pad1：馈电竖枝 + 辐射臂（短路枝那一侧留给 pad2）
_p1 = [
    _poly([(_fd_x - HW, _stub_y1 + PAD / 2 - PAD / 2), (_fd_x + HW, _stub_y1 + PAD / 2 - PAD / 2),
           (_fd_x + HW, _arm_y0), (_fd_x - HW, _arm_y0)], *PAD_FEED),                  # 馈电竖枝
    _poly([(_sh_x + HW, _arm_y0), (_ax1 if not MIRROR else max(xs) + HW, _arm_y0),
           (_ax1 if not MIRROR else max(xs) + HW, _arm_y1), (_sh_x + HW, _arm_y1)], *PAD_FEED),  # 辐射臂
    # 馈电腿真实铜箔外沿→六层50Ω微带的连续渐变。中心线端点到外沿先保留HW=0.75mm
    # 的1.5mm等宽段；taper不再与端帽重叠。窄端落在FEED_TAPER_END，外部0.15mm
    # 微带再走2.853mm到ZP1；从6.0mm中心线端点算仍为0.750+1.500+2.853=5.103mm。
    _poly([(_TAPER_START[0] - HW, _TAPER_START[1]),
           (_TAPER_START[0] + HW, _TAPER_START[1]),
           (_TAPER_END[0] + RF_W / 2, _TAPER_END[1]),
           (_TAPER_END[0] - RF_W / 2, _TAPER_END[1])], *PAD_FEED),
]
# pad2：短路竖枝（含与辐射臂搭接的那一小段）+ V3.5 新增的短路脚引线
_p2 = [
    _poly([(_sh_x - HW, _arm_y0), (_sh_x + HW, _arm_y0),
           (_sh_x + HW, _stub_y1), (_sh_x - HW, _stub_y1)], *PAD_SHORT),
]
for _num, _at, _prims in (("1", PAD_FEED, _p1), ("2", PAD_SHORT, _p2)):
    s_pad = [f'\t(pad "{_num}" smd custom (at {_at[0]:g} {_at[1]:g})',
             f'\t\t(size {PAD_SZ} {PAD_SZ}) (layers "F.Cu")',
             '\t\t(options (clearance outline) (anchor rect))',
             '\t\t(primitives']
    s_pad += ['\t\t\t' + q for q in _prims]
    s_pad += ['\t\t)', '\t)']
    s += s_pad

# 全铜层禁铜区（rule area）：阻止 GND 铺铜灌到辐射臂上；
# route_rf.py 的缝合过孔搜索会把封装内 zone 当禁布区读，故也自动挡住过孔。
s += ['\t(zone',
      # ⚠️ **必须列全所有铜层**。这个封装是 4 层板时代画的（F/In1/In2/B 正好 4 层），
      # 96fc34e 转 6 层时新增的 In3/In4 没跟着加，结果 IFA 辐射体正下方
      # **In3(3V3_DIG) 和 In4(GND) 两个平面 100% 满铺铜**——天线下方有地/电源平面
      # 等于把辐射短路掉，IFA 基本不工作。改板层数时，凡是写死层名的地方都要复查。
      '\t\t(net 0) (net_name "") (layers "F.Cu" "In1.Cu" "In2.Cu" "In3.Cu" "In4.Cu" "B.Cu")',
      '\t\t(name "ifa_keepout") (hatch edge 0.5)',
      '\t\t(connect_pads (clearance 0))',
      '\t\t(min_thickness 0.25) (filled_areas_thickness no)',
      # pads/footprints 必须放行：禁铜区是 J7 自带的，禁 pads 会把天线自己的焊盘判违例。
      # 它的职责是挡 GND 覆铜和别人的走线/过孔；不让别的元件进来由 gen_pcb.py 的区域断言保证。
      '\t\t(keepout (tracks not_allowed) (vias not_allowed) (pads allowed)',
      '\t\t\t(copperpour not_allowed) (footprints allowed))',
      '\t\t(placement (enabled no) (sheetname ""))',
      '\t\t(fill (thermal_gap 0.5) (thermal_bridge_width 0.5))',
      '\t\t(polygon (pts',
      # 凹形：主体一直包到**脚末端边缘**(LEG_END+HW)，但在馈电脚那一列只到 LEG_END，
      # 缺口交给下面的 ifa_feed_channel（禁铺铜、但放行走线）。
      # 为什么必须包到脚末端边缘：脚的铜箔含端帽，比中心线多伸 HW=0.75mm。
      # 之前禁铜区只到 LEG_END，那 0.75mm 端帽就整个泡在 GND 铺铜里——
      # 用户在 KiCad 里量到的 0.740mm 就是它。腿只要有一截被铜围住，
      # 那一截就不算数（V2 实测：泡 31% ≈ 整条腿失效），短路脚和馈电脚都一样。
      f'\t\t\t(xy {KX0:g} {KEEPOUT_Y[0]:g}) (xy {KX1:g} {KEEPOUT_Y[0]:g})'
      f' (xy {KX1:g} {_KO_BOT:g})'
      f' (xy {_ch_x+_CH_W/2:g} {_KO_BOT:g}) (xy {_ch_x+_CH_W/2:g} {KEEPOUT_Y[1]:g})'
      f' (xy {_ch_x-_CH_W/2:g} {KEEPOUT_Y[1]:g}) (xy {_ch_x-_CH_W/2:g} {_KO_BOT:g})'
      f' (xy {KX0:g} {_KO_BOT:g})',
      '\t\t))',
      '\t)']

# ── 馈电脚出线通道（**只挖 F.Cu**）────────────────────────────────────
# 两个脚的铜箔都带端帽、比净空下沿多伸 HW=0.75mm。短路脚这样是对的——它要
# 用末端截面结结实实撞进 GND 铺铜。但馈电脚这 0.75mm 会被铺铜**三面围住**
# （实测最近灌铜 0.938mm，短路脚那边更是只有 0.227mm），等于把"腿泡在铜里"
# 这个刚修掉的毛病又局部请了回来。
#
# 所以在馈电脚正下方开一条通道，把它的端帽和 50Ω 走线的起始段一起罩进净空。
# ⚠️ 通道**只禁 F.Cu**：In1 必须留着，那是这段 taper/50Ω 微带的参考面，
#    连它一起挖会让走线失去参考、阻抗突变。
s += ['\t(zone',
      '\t\t(net 0) (net_name "") (layers "F.Cu")',
      '\t\t(name "ifa_feed_channel") (hatch edge 0.5)',
      '\t\t(connect_pads (clearance 0))',
      '\t\t(min_thickness 0.25) (filled_areas_thickness no)',
      # 走线/过孔放行：这条通道存在的意义就是让馈线走出去
      '\t\t(keepout (tracks allowed) (vias allowed) (pads allowed)',
      '\t\t\t(copperpour not_allowed) (footprints allowed))',
      '\t\t(placement (enabled no) (sheetname ""))',
      '\t\t(fill (thermal_gap 0.5) (thermal_bridge_width 0.5))',
      '\t\t(polygon (pts',
      f'\t\t\t(xy {_ch_x-_CH_W/2:g} {KEEPOUT_Y[1]:g}) (xy {_ch_x+_CH_W/2:g} {KEEPOUT_Y[1]:g})'
      f' (xy {_ch_x+_CH_W/2:g} {_KO_BOT+_CH_D:g}) (xy {_ch_x-_CH_W/2:g} {_KO_BOT+_CH_D:g})',
      '\t\t))',
      '\t)']


# **腿上不开阻焊窗**（2026-08-21 改）。原来给两个脚各开了一个 1.6mm 见方的窗，
# 想方便 NanoVNA 搭线，但那等于在天线脚上留了一块裸铜——既是线宽/覆膜的突变，
# 也诱使人直接在脚上测，而在脚上测到的值不是真实工作状态。
# 调试口改到 IFA_MATCH 上的 U.FL（π 网络之后），那里有完整参考地，测的是完整链路。
# F.Fab 上画出铜箔包络，方便版面复核
s.append(f'\t(fp_rect (start {BB[0]:g} {BB[1]:g}) (end {BB[2]:g} {BB[3]:g})'
         f' (stroke (width 0.1) (type default)) (fill no) (layer "F.Fab"))')
s.append(')')

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w") as f:
    f.write("\n".join(s) + "\n")

# ── 把关键几何导出给 gen_pcb.py / 布线脚本 ─────────────────────────────────
# 不让别处自己写一份坐标副本。这块板在"改了一处、漏了另一处"上栽过 5 次
# （板厚、层数、孔径、版本号、封装禁铜层）。
_GEOM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ifa_geom.py")
with open(_GEOM, "w") as _f:
    _f.write('"""由 gen_ifa_footprint.py 自动生成，请勿手改。改天线跑那个脚本，这里会同步。"""\n'
             f"KEEPOUT_LOCAL = ({KX0:.4f}, {KEEPOUT_Y[0]:.4f}, {KX1:.4f}, {KEEPOUT_Y[1]:.4f})\n"
             f"BBOX_LOCAL = ({BB[0]:.4f}, {BB[1]:.4f}, {BB[2]:.4f}, {BB[3]:.4f})\n"
             f"ARM_SPAN = {max(xs)-min(xs):.4f}   # 主臂中心线跨度 = 谐振长度\n"
             f"LEG_END_LOCAL = {mx((0, LEG_END))[1]:.4f}   # 脚末端 y（= 净空下沿）\n"
             f"FEED_LOCAL = ({FEED[0]:.4f}, {FEED[1]:.4f})\n"
             f"FEED_CHANNEL_WIDTH = {_CH_W:.4f}   # F.Cu无铜通道；容纳1.5mm taper及两侧各0.45mm间隙\n"
             f"FEED_CHANNEL_DEPTH = {HW+_CH_D:.4f}   # 中心线端点→taper窄端：0.75mm等宽段+1.5mm taper\n"
             f"RF_WIDTH = {RF_W:.4f}   # JLC06161H-3313六层50Ω微带线宽\n"
             f"FEED_LEG_END = ({_ch_x:.4f}, {LEG_END:.4f})"
             "   # 6.0mm馈电脚中心线端点；到铜箔外沿还有0.75mm等宽段\n"
             f"FEED_LEG_COPPER_END = ({_TAPER_START[0]:.4f}, {_TAPER_START[1]:.4f})"
             "   # 馈电脚真实铜箔外沿 / taper宽端\n"
             f"FEED_TAPER_START = ({_TAPER_START[0]:.4f}, {_TAPER_START[1]:.4f})"
             "   # 完整1.5mm taper宽端\n"
             f"FEED_TAPER_END = ({_TAPER_END[0]:.4f}, {_TAPER_END[1]:.4f})"
             "   # taper窄端；外部0.15mm微带从这里接出\n"
             f"TAPER_LENGTH = {_CH_D:.4f}\n")

print(f"OK: {OUT}")
print(f"  铜箔包络 {BB[2]-BB[0]:.3f} × {BB[3]-BB[1]:.3f} mm，馈点局部 x {BB[0]:.3f}..{BB[2]:.3f}")
print(f"  辐射臂中心线跨度 {max(xs)-min(xs):.3f} mm（谐振长度）")
print(f"  开路端在馈点 {max(xs):+.3f}mm（{'右' if max(xs) > 0 else '左'}），短路点 {SHORT[0]:+.3f}mm")
print(f"  禁铜区 x {KX0:.3f}..{KX1:.3f}  y {KEEPOUT_Y[0]:.3f}..{KEEPOUT_Y[1]:.3f}（全 6 铜层）")
print(f"    四周净空 {CLEAR}mm；脚长 {LEG_H}mm 全程在净空内，末端 y={LEG_END:+.3f} 即净空下沿")
print(f"  → 脚末端直接撞进 GND 铺铜（无引线、无隔离槽、全段 {W}mm 等宽）")
print(f"  馈电: 中心线端点 y={LEG_END:.3f} → 铜边 y={_TAPER_START[1]:.3f} 等宽{HW:.3f}mm；")
print(f"        taper {_TAPER_START} → {_TAPER_END}，{W}→{RF_W}mm，已并入pad1铜箔")
