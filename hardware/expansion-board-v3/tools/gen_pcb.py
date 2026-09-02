#!/usr/bin/env python3
"""从网表生成 PCB（KiCad 自带 pcbnew Python 运行）。

布局策略：**区域装箱**，不再手工填坐标。
每个功能块分配一个矩形区域，区域内按封装实际尺寸自动装箱（保证不重叠）；
RF 链等对顺序敏感的，靠成员列表顺序保证左→右信号流向。
区域之间不相交 → 从根上消灭元件碰撞，不用反复跑 DRC 打地鼠。

运行：~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 gen_pcb.py
断言：封装全加载成功；每个网表节点找得到同号焊盘；每个元件恰好归属一个区域；区域装得下。
"""
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xml.etree.ElementTree as ET
import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")   # 副本上跑整条链用
BUILD = os.environ.get("PK_BUILD_DIR") or os.path.join(T, "build")   # 中间产物落在工程内，可核对、不会被系统清掉
os.makedirs(BUILD, exist_ok=True)
NETXML = os.path.join(BUILD, "expansion.net.xml")
OUT = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")
OFFICIAL_FP = os.path.expanduser("~/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints")
PROJECT_FP = os.path.join(T, "kicad", "expansion-board-v3.pretty")

# 板框 100×62：X 卡免费档；Y 上下各比屏板多 1mm（给 J3 排针让走线位）
X0, Y0, X1, Y1 = 50.0, 50.0, 150.0, 112.0
BOFF_X, BOFF_Y = 48.75, 51.0          # 基准板坐标 → KiCad


def kb(bx, by):
    return (round(bx + BOFF_X, 3), round(by + BOFF_Y, 3))


# ============ 固定件（几何有硬约束，不参与装箱）============
PINNED = {
    # 对插排针。两板是**翻面对扣**的：本板元件面朝上、基准板元件面朝下，
    # 因此本板坐标系相对基准板元件面视图**绕竖轴镜像**（X 翻转，上下排 Y 不变）。
    # 基准板 J3 引脚列 X = 34.54..82.80（脚1/2 在右端 82.80，见 board_pinout-zh_CN.md:76-79）
    # 镜像后本板 X = 102.5-82.80=19.70（脚1/2，左端） .. 102.5-34.54=67.96（脚39/40）
    # → 中心 X = 102.5-58.67 = 43.83。曾误按 58.67 直接摆放（没做镜像），X 偏 14.84mm。
    "J1": (*kb(43.83, 55.33), 90),   # 引脚轴 y=105.06(偶/上排) / 107.60(奇/下排)
    # GNSS 两个 U.FL 已随 GNSS 切换子系统收拢到左条（见 PLACEMENT.py，v4 禁铜带要求）
    "J5": (146.5, 88, 0),            # 978 U.FL（右边缘）
    # 978 偏置Tee 的**射频侧**：扼流与 ESD 必须贴在天线节点上，否则等于挂长残桩，
    # 100nH 选 SRF>1090MHz 的绕线型就白费了。曾被溢出链甩到板子最左边（出图才发现）。
    # 直流侧的 Q2/F2/R17 不敏感，仍参与装箱。
    "L8": (140.5, 84.0, 0),          # 100nH 偏置扼流 → ANT_978
    "D3": (140.5, 92.0, 0),          # 天线口 ESD（Cj≤0.6pF）
    "C39": (140.5, 88.0, 0),         # 978 匹配末级 → 天线（原离 J5 28.1mm≈0.55dB）
    "J4": (53.5, 92, 270),           # USB-C（左边缘，需外壳开口）
    # 板载 1090 IFA（v4 HFSS 冻结几何，2026-08-24 §8）。馈点 x=85.46，铜箔包络 53.5mm，
    # 封装自带禁铜区 x77.7–135.2 / y50.1–58.5（全 6 铜层），开路端 x≈135（H2 螺丝孔前）。
    # 顶部整条 y50.1–58.5 无铜（top_keepout_v3.py 补板级两段），所有器件/走线必须让出。
    "ANT1": (85.46, 57.75, 0),         # 臂顶距板边 1.012mm；锚点 = 馈点 - 馈电腿偏移
                                         # π 网络钉死在馈点水平线 y62.86–63.64（route_ifa_feed 断言锁）
}

# ============ 区域定义（x0,y0,x1,y1，互不相交）============
# 禁区（全部由下方 assert 机检，这里只作导读，不作依据）：
#   IFA 天线本体+全铜层禁铜区 x95.3–141.3 / y50.1–58.6（ANT1 封装自带，总长 44mm）
#   J1 焊盘包络 x67.9–117.3 / y102.2–110.5（对扣镜像后左移 14.84）
#   安装孔 (54,56)(146,56)(54,106)(146,106) 各 r≈3
REGIONS = {
    # ── 顶带 y50–58.5：整条是 IFA 天线禁铜带（v4 冻结），ANT_TOP 区已撤销 ──
    # （U17/C57/C58/C59 + GNSS 切换子系统已收拢左条，见 PLACEMENT.py 固定坐标）
    "GNSS_BIAS": (82, 61.6, 97, 74.5),    # 1090 开关控制脚；下界避开 ANT1 焊盘包络(y≤60.8)
    "RF1090_W":  (99, 61.6, 143.0, 66.5), # π匹配(ZP1/ZS1/ZP2)紧贴 IFA 馈点 + 偏置Tee/ESD + Data Slicer
    "CHAIN1090": (99, 67.5, 143.0, 75),   # 1090 接收链（顺序敏感：左→右）
    "MCU":       (82, 75.5, 117.5, 99.5), # RP2040 区（下界受 J1 外框 100.42，不是焊盘 102.23）
    "SUBG":      (119.5, 75.2, 138.5, 99.5),  # CC1312R+晶振+978 匹配（左界受 J1 外框右沿 118.53）
    "LEFT_COL":  (59.8, 61.6, 81, 88),    # 传感器+GNSS模块（左界受 J4 焊盘右沿 59.0；下界避开 ANT1 包络）
    # 电源块独立成区，正对 J1 的 5V 脚（脚1/3 在 x≈68.5/71）——原先塞在右下角 172mm² 装不下，
    # 去耦电容 C1–C6 全被溢出链甩到板子另一头，而去耦电容必须贴着稳压器。
    "PWR":       (59.8, 89, 81, 99.5),    # 电源（LDO + 去耦，紧邻 J1 的 5V 入口）
    "SUBG_TAIL": (119.5, 102.5, 143, 109.8),  # 978 直流侧溢出（Q2/R17/F2，就近在 SUBG 下方）
    # ── 以下是纯溢出目标区，必须排在所有源区之后 ──
    "LEFT_EDGE": (52.2, 60.0, 59.0, 85.5),  # 左边缘空地（J6 搬到射频区后上界从 72.6 放开到 60，下界受 J4 顶 86.4）
    "LEFT_TAIL": (59.8, 102.5, 65.7, 109.8),  # J1 左侧窄条（避开安装孔 (54,106)）
}
# 主区装不下时的备用区（按顺序尝试）。没有备用区就直接断言失败，不静默挤压。
OVERFLOW = {"GNSS_BIAS": ["MCU"], "RF1090_W": ["CHAIN1090"],
            "CHAIN1090": ["MCU"], "MCU": ["LEFT_EDGE"], "SUBG": ["SUBG_TAIL"],
            "LEFT_COL": ["LEFT_EDGE", "LEFT_TAIL"], "PWR": ["LEFT_EDGE", "LEFT_TAIL"],
            "LEFT_EDGE": ["LEFT_TAIL"]}

# 溢出目标必须排在源区**之后**——区域按声明顺序装箱，目标区若已装完，
# 溢出进去的元件不会再被摆放，会全部堆在原点：实测炸出 55 条短路 + 36 条外框重叠。
# 板边余量：铜到板边 0.5mm 是 DRC 硬规则，但位号丝印默认摆在本体上方约 1.5mm，
# 区域贴到 0.5mm 就会把丝印顶到板框外（实测 10 条 silk_edge_clearance）。
# 元件区一律内缩 2.2mm。天线等贴边件走 PINNED，不受此限。
EDGE_MARGIN = 2.2
for _n, (_a, _b, _c, _d) in REGIONS.items():
    assert _a >= X0 + EDGE_MARGIN and _b >= Y0 + EDGE_MARGIN and \
           _c <= X1 - EDGE_MARGIN and _d <= Y1 - EDGE_MARGIN, \
        f"区域 {_n}({_a},{_b},{_c},{_d}) 距板边不足 {EDGE_MARGIN}mm，位号丝印会出界"

_ORD = list(REGIONS)
for _src, _tgts in OVERFLOW.items():
    for _t in _tgts:
        assert _t in REGIONS, f"OVERFLOW 目标区 {_t} 不存在"
        assert _ORD.index(_t) > _ORD.index(_src), \
            f"溢出目标 {_t} 声明在源区 {_src} 之前，溢出的元件将不会被摆放"

# 区域必须互不相交——这是"从根上消灭元件碰撞"的前提，新增区域时靠机检而不是靠眼睛
for _n1, _r1 in REGIONS.items():
    for _n2, _r2 in REGIONS.items():
        if _n1 >= _n2:
            continue
        assert not (_r1[0] < _r2[2] and _r2[0] < _r1[2] and _r1[1] < _r2[3] and _r2[1] < _r1[3]), \
            f"区域 {_n1}{_r1} 与 {_n2}{_r2} 相交"
# 非 RF 区域按高度降序装箱：同行等高，减少行内空高浪费。
# RF 区域禁止排序——成员顺序即信号流向，打乱会让链路来回折。
# 行内水平均摊的区域（非射频）。射频区拉开间距=加长射频走线，禁用。
# SUBG 也做水平均摊：它的射频段只有 L9/L11/L12 三颗串联件，实测拉开后
# 串联主路径 18.27mm≈0.37dB，与拉开前基本持平（主要长度在 L12→C39 那段跨到天线口）；
# 换来的是 C62/C63 那一带有空间放位号。
SPREAD_X = {"LEFT_COL", "PWR", "MCU", "GNSS_BIAS", "LEFT_EDGE", "LEFT_TAIL", "SUBG"}
SORT_BY_HEIGHT = {"LEFT_COL", "PWR", "LEFT_TAIL", "LEFT_EDGE", "MCU"}
# 需要旋转的元件（信号流向/引脚朝向要求）
ROTATE = {"U12": 180}

# 扇出光环（mm）：细间距芯片四周额外留出的净空，供引脚出线和入平面过孔用。
# 依据：默认装箱间距 0.75mm 时，U8(QFN-56/61焊盘) 有 36 处布不通、U10 有 6 处——
# 邻居全贴在 0.75mm 上，焊盘根本出不来。QFN 单排焊盘沿边扇出，一侧 14 根 ×
# (0.15线宽+0.15间距)=4.2mm 需在 7mm 边长内散开，2mm 净空够用；入平面过孔另需 ~1mm 半径。
HALO = {
    "U8": 2.0, "U10": 2.0,          # RP2040 / CC1312R，QFN 大芯片
    # 注：试过加到 3.0/2.5 想给信号出线让路，结果 MCU→MCU_TAIL→SUBG→PWR 溢出连锁装不下。
    #     板子整体 53% 填充但局部腾挪空间有限，光环不是解决信号扇出的有效杠杆。
    "U4": 1.2, "U5": 1.2, "U6": 1.2,  # BNO085 LGA-28 / LDO / BMP388 LGA-16
    "U7": 1.2, "U11": 1.0, "U2": 1.0,
}

MEMBERS = {
    # "ANT_TOP" 区已撤销（v4 禁铜带占顶带）：U17/C57/C58/C59 + J2/J8 + 偏置链
    # 共 14 件收拢左条 x52–64，全部走 PLACEMENT.py 固定坐标（GNSS 子系统重排）。
    # 顺序即 1090 信号流向：ANT1(x=102 馈点) → π匹配 → 隔直 → 开关 → 公共口去 LNA；
    # 外接支路 C30 与 J6 紧挨开关，避免外接天线走长线（LNA 前每 mm 都进 NF）
    "RF1090_W":  ["ZP1", "ZS1", "ZP2", "C53", "U16", "C54", "C30", "J6", "D2", "Q3", "R18", "F3", "L14", "C37", "C38",
                  "R30", "R31", "C46", "R32", "C47", "R33", "R34", "C49", "R35", "C51",
                  "U15", "R36", "C52"],      # 后半段是 Data Slicer
    "GNSS_BIAS": ["R22", "R23", "C55", "C56", "R24", "R25"],   # 含 1090 开关控制脚
                                              # （Q4/F4/L2/Q5/F5/L15/R26/R27 已收拢左条，走 PLACEMENT 固定坐标）
    # 顺序 = 信号流向，装箱器按序左→右
    "CHAIN1090": ["C36", "C48", "R11", "C21", "U11", "L1", "C31", "FL1", "C32", "U12", "C33", "FL2", "C34",
                  "R19", "C35", "U14", "R21"],   # U13/R20(AD8319 支路) 2026-09-02 删
    "LEFT_COL":  ["U7", "U4", "U6", "U5"],   # GNSS 模块 + IMU + 气压计（U1-U3 是电源，随页归 PWR）
    # 978 射频件按信号流向连续摆放（顺序即拓扑，缩短自动布线距离）
    "SUBG":      ["L10", "C40", "L13", "C45",           # 交叉耦合/RX_TX
                  "L9", "L11", "L12",                   # 串联主路径 N3→N4→N5（C39 已钉到天线口）
                  "C44", "C41", "C42", "C43",           # 分流对地
                  "U10", "Y2", "Y3", "C67", "L7", "C60", "C61", "C62",
                  "C63", "C64", "C65", "C66", "C68", "C69",
                  "Q2", "R17", "F2"],   # C39/L8/D3 已钉到天线口   # 大件先排，其余按页流入
}



def size_mm(fp):
    bb = fp.GetCourtyard(pcbnew.F_CrtYd).BBox()
    if bb.GetWidth() == 0:
        bb = fp.GetBoundingBox(False, False)
    return pcbnew.ToMM(bb.GetWidth()), pcbnew.ToMM(bb.GetHeight())


# ---------------- 读网表 ----------------
# ⚠️ 网表必须比原理图新，否则整块板会按过时的数据重建，而且**不报错**。
#
# 2026-08-20 实际踩到：目录从 expansion-board-v3.1 改名成 v3 后，这份网表还是
# 11 天前导的，里面的封装库名全是 `expansion-board-v3.1:`。gen_pcb 拿它去
# load_fp，报的却是 `'NoneType' object has no attribute 'FootprintLoad'`
# ——KiCad 在库目录不存在时返回 None，完全看不出根因是网表陈旧。
#
# 这份文件此前没有任何脚本负责刷新（只有本脚本读、没人写），全靠人记得手动导。
_SCH = os.path.join(T, "kicad", "expansion-board-v3.kicad_sch")
newest_schematic = max(
    os.path.getmtime(path)
    for path in __import__("glob").glob(os.path.join(T, "kicad", "*.kicad_sch"))
)
if (not os.path.exists(NETXML)
        or os.path.getmtime(NETXML) < newest_schematic):
    import subprocess
    _CLI = os.path.expanduser("~/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli")
    _r = subprocess.run([_CLI, "sch", "export", "netlist", "--format", "kicadxml",
                         "-o", NETXML, _SCH], capture_output=True, text=True)
    assert _r.returncode == 0, f"导出网表失败:\n{_r.stdout}{_r.stderr}"
    print(f"网表比原理图旧，已重新导出 → {os.path.relpath(NETXML, T)}")

tree = ET.parse(NETXML)
comps = []
DNP_REFS = set()
for c in tree.iter("comp"):
    fp = c.find("footprint"); sh = c.find("sheetpath"); vl = c.find("value")
    comps.append((c.get("ref"), fp.text if fp is not None else "",
                  sh.get("names") if sh is not None else "/",
                  vl.text if vl is not None and vl.text else ""))
    if any(p.get("name") == "dnp" for p in c.findall("property")):
        DNP_REFS.add(c.get("ref"))
nets = [(n.get("name"), [(x.get("ref"), x.get("pin")) for x in n.iter("node")])
        for n in tree.iter("net")]

# 未显式分配的按原理图页归入默认区域
SHEET_REGION = {"/Power/": "PWR", "/Sensors_GNSS/": "LEFT_COL", "/MCU_RP2040/": "MCU",
                "/SubGHz_978/": "SUBG", "/RF_1090/": "RF1090_W"}
_explicit = {r for v in MEMBERS.values() for r in v}
assigned = {r: list(v) for r, v in MEMBERS.items()}
for ref, _, sheet, _val in comps:
    if ref in PINNED or ref in _explicit:
        continue
    reg = SHEET_REGION.get(sheet)
    assert reg, f"{ref} 无区域归属（页 {sheet}）"
    assigned.setdefault(reg, []).append(ref)

# ---------------- 建板 ----------------
board = pcbnew.NewBoard(OUT)
_ds = board.GetDesignSettings()
_ds.SetCopperLayerCount(6)
# 6 层叠层 SIG-GND-SIG-SIG-GND-SIG：F.Cu(L1信号+RF) / In1.Cu(L2完整GND平面,RF参考)
# / In2.Cu(L3信号) / In3.Cu(L4信号+3V3_DIG覆铜) / In4.Cu(L5完整GND平面) / B.Cu(L6信号)
# 最小线宽 0.2mm（KiCad 默认）对 0.4mm pitch 的 QFN 扇出是死路：
# 中心到邻脚边缘 0.3mm，需 线宽/2 + 间距0.2 ≤ 0.3 → 线宽 ≤ 0.2 恰好卡死，无余量。
# 嘉立创四层板最便宜档就支持 5mil(0.127mm) 线宽/间距，放到 0.13mm 留出扇出空间。
MIN_TRACK_W = 0.13
_ds.m_TrackMinWidth = pcbnew.FromMM(MIN_TRACK_W)

# Default 类的线宽/间距。U8 是 QFN-56、0.4mm pitch，扇出空间直接被这两个数吃掉：
# 中心到邻脚边缘 0.3mm，要塞下一根线需 线宽/2 + 间距 ≤ 0.3。
#   0.20/0.15 → 0.10+0.15 = 0.25，勉强；两根线并排就不行了
#   0.15/0.127 → 0.075+0.127 = 0.202，宽松不少
# 嘉立创四层板最便宜档就支持 5mil(0.127mm) 线宽/间距，收到这个数不加钱。
# 只动 Default——RF50(0.34/0.15) 和 POWER(0.5/0.2) 各有各的电气理由，不碰。
# 实测（可复现流水线，只改这一个变量）：
#   线宽 0.20 间距 0.15 → 未连接 35，其中 U8 相关 14
#   线宽 0.15 间距 0.15 → 未连接 30，其中 U8 相关  5   ← 采用
#   线宽 0.13 间距 0.15 → 未连接 36，其中 U8 相关 10   （更细反而更差）
#   线宽 0.15 间距 0.127→ 未连接 31，但多出 61 条 hole_clearance
# 间距不能低于 0.15：孔间距 = 铜间距 + 过孔环宽(0.5mm焊盘-0.3mm孔)/2 = 铜间距 + 0.1，
# 而板规要求孔到铜 ≥0.25mm，所以铜间距 0.15 是硬下限，0.127 会差 0.021mm。
DEFAULT_TRACK_W = float(os.environ.get("PK_TRACK_W", 0.15))
DEFAULT_CLR = float(os.environ.get("PK_CLR", 0.15))
# Default 类过孔：0.6mm 焊盘 / 0.3mm 钻 → 环宽 0.15mm（嘉立创绝对下限）。
# 历史用 0.5/0.3（环宽 0.1）低于嘉立创 0.15 下限，板规 min_via_annular_width=0.1 还让 DRC 查不出。
# 嘉立创 6 层标准档能力是 0.15mm 孔 / 0.25mm 盘（官网 capabilities 页已核）。
# 原来用 0.3/0.6 比工厂下限保守了一倍，代价是过孔占地大：GND 缝合墙和 QFN 扇出
# 把射频引脚区堵死，SUBG_RFN 的通道只剩 0.010mm。收到 0.2/0.4 仍留足裕度
# （孔径比 1.6/0.2=8 < 上限 14），单孔占地降 44%。
DEFAULT_VIA_DIA = float(os.environ.get("PK_VIA_DIA", 0.45))
DEFAULT_VIA_DRILL = float(os.environ.get("PK_VIA_DRILL", 0.3))
MIN_VIA_ANNULAR = 0.15      # 板规：修历史 134 个过孔环宽不足
ZONE_MIN_CLR = 0.15         # 覆铜 zone 间距：修 codex 查明的 0.50mm bug（GND 接不上铜真因）
# DRC 实际读的是 .kicad_pro 里的规则，板文件重生成不会动它 → 必须同步，否则两处漂移
_pro = os.path.join(BDIR, "expansion-board-v3.kicad_pro")
if os.path.exists(_pro):
    import json
    with open(_pro) as _f:
        _d = json.load(_f)
    _r = _d["board"]["design_settings"]["rules"]
    _dirty = False
    if _r.get("min_track_width") != MIN_TRACK_W:
        print(f"同步工程规则 min_track_width: {_r.get('min_track_width')} → {MIN_TRACK_W}")
        _r["min_track_width"] = MIN_TRACK_W
        _dirty = True
    # 板规过孔环宽：历史 0.1 低于嘉立创 0.15 绝对下限，且让 DRC 查不出
    if _r.get("min_via_annular_width") != MIN_VIA_ANNULAR:
        print(f"同步工程规则 min_via_annular_width: {_r.get('min_via_annular_width')} → {MIN_VIA_ANNULAR}")
        _r["min_via_annular_width"] = MIN_VIA_ANNULAR
        _dirty = True
    # 覆铜 zone 默认间距：codex 查明历史遗留 0.50mm 比 0.15 规则严 3 倍，GND 接不上铜的真因
    _zdef = _d["board"]["design_settings"]["defaults"]["zones"]
    if _zdef.get("min_clearance") != ZONE_MIN_CLR:
        print(f"同步 zone min_clearance: {_zdef.get('min_clearance')} → {ZONE_MIN_CLR}")
        _zdef["min_clearance"] = ZONE_MIN_CLR
        _dirty = True

    # 网络类归类规则同样从设计里生成，不手工维护。
    # 曾经手工列表已经过期：ANT_1090/DET_COUPLE/GNSS_RF/RF1090_FEED 等网络名早已不存在
    # （加天线切换时改过），而新增的 SW1_J1/J2/J3、ANT1090_*、ANT_GNSS_* 一条都没覆盖，
    # 结果这些射频线掉回 Default 的 0.2mm 线宽（实测），不是 RF50 的 0.34mm。
    import sys as _sys
    _sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from gen_sch import RF50_NETS as _RF50, RF_NETS as _RF_NETS
    _all_nets = {n for n, _ in nets}
    _rf50 = sorted(_RF50)                          # 3V3_RF 是供电轨，归 POWER（定义见 gen_sch）
    # POWER 分两档。整类 0.5mm 是出不了 QFN 引脚的：0.5 线从 0.25mm 宽的焊盘引出
    # 需要 0.25+0.2=0.45mm 通道，而 U8 引脚 pitch 只有 0.4mm。
    # 低压轨实际电流都很小（3V3_DIG≈150mA，见 export_dsn.py 注释），0.25mm 线
    # 1oz 铜载流约 0.9A，余量 6 倍。VCC_5V/USB_VBUS 是 USB 输入干路，保持 0.5mm。
    _power_hi = sorted(n for n in _all_nets if n in {"VCC_5V", "USB_VBUS"})
    _power_lo = sorted(n for n in _all_nets
                       if (n.startswith(("3V3_", "RP_1V1", "SUBG_VDDR"))
                           or n.endswith("_FUSE"))
                       and n not in _power_hi)
    _power = _power_hi + _power_lo
    _fine = sorted({
        "SWCLK", "SWDIO", "DEMOD0", "DEMOD1", "DEMOD3", "RECOVERED_CLK",
        "PULSES", "ADSB_RXD", "ADSB_TXD", "GNSS_RXD", "GNSS_PPS", "IMU_INT",
        "BIAS_EN_1090", "RP_XOUT", "RP_XIN", "RP_XT2", "SUBG_IRQ", "SUBG_TMSC",
        "SUBG_SCK", "SUBG_MOSI", "DEMOD2",
    } & _all_nets)
    _pat = ([{"netclass": "RF50", "pattern": n} for n in _rf50]
            + [{"netclass": "POWER", "pattern": n} for n in _power_hi]
            + [{"netclass": "POWER_LO", "pattern": n} for n in _power_lo]
            + [{"netclass": "FINE", "pattern": n} for n in _fine])
    _stale = [p["pattern"] for p in _pat if p["pattern"] not in _all_nets]
    assert not _stale, f"网络类规则指向不存在的网络（设计已改名？）：{_stale}"
    for _c in _d["net_settings"].get("classes", []):
        if _c.get("name") != "Default":
            continue
        if _c.get("track_width") != DEFAULT_TRACK_W or _c.get("clearance") != DEFAULT_CLR:
            print(f"同步 Default 类: 线宽 {_c.get('track_width')}→{DEFAULT_TRACK_W} "
                  f"间距 {_c.get('clearance')}→{DEFAULT_CLR}")
            _c["track_width"] = DEFAULT_TRACK_W
            _c["clearance"] = DEFAULT_CLR
            _dirty = True
        # Default 过孔 0.5→0.6mm 焊盘（环宽 0.1→0.15，满足嘉立创下限）
        if _c.get("via_diameter") != DEFAULT_VIA_DIA:
            print(f"同步 Default 类过孔: {_c.get('via_diameter')}→{DEFAULT_VIA_DIA}")
            _c["via_diameter"] = DEFAULT_VIA_DIA
            _c["via_drill"] = DEFAULT_VIA_DRILL
            _dirty = True

    _classes = _d["net_settings"].setdefault("classes", [])
    _want_lo = {"name": "POWER_LO", "track_width": 0.25, "clearance": 0.15,
                "via_diameter": DEFAULT_VIA_DIA, "via_drill": DEFAULT_VIA_DRILL}
    _lo = next((c for c in _classes if c.get("name") == "POWER_LO"), None)
    if _lo is None:
        _base = next(c for c in _classes if c.get("name") == "Default")
        _lo = dict(_base); _lo.update(_want_lo); _classes.append(_lo)
        print("新增网络类 POWER_LO: 线宽 0.25 / 净空 0.15（低压轨出 QFN 引脚用）")
        _dirty = True
    elif any(_lo.get(k) != v for k, v in _want_lo.items()):
        _lo.update(_want_lo); print("同步 POWER_LO 类参数"); _dirty = True
    # 其余各类的过孔尺寸也跟着收
    for _c in _classes:
        if _c.get("via_diameter") not in (None, DEFAULT_VIA_DIA):
            print(f"同步 {_c.get('name')} 过孔: {_c.get('via_diameter')}/{_c.get('via_drill')}"
                  f" → {DEFAULT_VIA_DIA}/{DEFAULT_VIA_DRILL}")
            _c["via_diameter"] = DEFAULT_VIA_DIA
            _c["via_drill"] = DEFAULT_VIA_DRILL
            _dirty = True
    # 板规下限跟着放开，否则新过孔会被自己的 min_through_hole_diameter 判违例
    _rules = _d["board"]["design_settings"]["rules"]
    # ⚠️ 这几项是**板规下限**，要按工厂能力设，**不能等于常规过孔尺寸**。
    # 早先把它们同步成 DEFAULT_VIA_DIA/DRILL(0.4/0.2)，结果每次 gen_pcb 都把
    # via-in-pad 需要的 0.3/0.15 冲掉——QFN 0.5mm pitch 上盘中孔必须用 0.3mm 盘
    # （0.4mm 盘超出焊盘会撞邻居，见 project_via_in_pad_qfn 的核算）。
    # 下限放到嘉立创 6 层标准档：via 0.25/0.15、环宽 0.05；这里留一档余量取
    # 0.3/0.15/环宽0.07。常规过孔仍由 netclass 的 0.4/0.2 控制，不受影响。
    VIP_DIA, VIP_DRILL = 0.30, 0.15
    for _k, _v in (("min_through_hole_diameter", VIP_DRILL),
                   ("min_hole_to_hole", 0.2),
                   ("min_via_diameter", VIP_DIA),
                   ("min_via_annular_width", 0.07)):
        if _rules.get(_k) != _v:
            print(f"同步板规 {_k}: {_rules.get(_k)} → {_v}")
            _rules[_k] = _v
            _dirty = True

    if _d["net_settings"].get("netclass_patterns") != _pat:
        print(f"同步网络类规则: RF50 {len(_rf50)} 条 / POWER {len(_power_hi)} 条"
              f" / POWER_LO {len(_power_lo)} 条 / FINE {len(_fine)} 条")
        _d["net_settings"]["netclass_patterns"] = _pat
        _dirty = True

    if _dirty:
        with open(_pro, "w") as _f:
            json.dump(_d, _f, indent=2)


def edge(x1, y1, x2, y2):
    s = pcbnew.PCB_SHAPE(board); s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(pcbnew.VECTOR2I_MM(x1, y1)); s.SetEnd(pcbnew.VECTOR2I_MM(x2, y2))
    s.SetLayer(pcbnew.Edge_Cuts); s.SetWidth(pcbnew.FromMM(0.1)); board.Add(s)


edge(X0, Y0, X1, Y0); edge(X1, Y0, X1, Y1); edge(X1, Y1, X0, Y1); edge(X0, Y1, X0, Y0)

netmap = {}
for name, _ in nets:
    ni = pcbnew.NETINFO_ITEM(board, name); board.Add(ni); netmap[name] = ni


def load_fp(lib, name):
    p = PROJECT_FP if lib == "expansion-board-v3" else os.path.join(OFFICIAL_FP, lib + ".pretty")
    fp = pcbnew.FootprintLoad(p, name)
    assert fp, f"封装加载失败: {lib}:{name}"
    # FootprintLoad 是按**路径**加载的，返回对象的 FPID 只有封装名、没有库名。
    # 不补这一句，板上 178 个 fpid 全是裸名（`R_0603_1608Metric`），而原理图侧
    # 带库（`Resistor_SMD:R_0603_1608Metric`）。后果不是"报一堆不匹配"，而是
    # **schematic parity 整项静默失效**——KiCad 定位不到库，footprint_symbol_mismatch
    # 压根不跑，parity 报 0 看着一切正常。V4 在 2026-09-01 踩过同一个坑
    # （补前缀后 parity 从 0 变 212），V3 这边是 196。
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    return fp


FPS = {}
for ref, fpid, _, val in comps:
    assert ":" in fpid, f"{ref} 无封装"
    fp = load_fp(*fpid.split(":", 1)); fp.SetReference(ref)
    # ⚠️ 元件值必须从网表灌进来。封装库加载出来的 Value 是**封装名**
    # （C10 = "C_0603_1608Metric" 而不是 "100nF"），而这里一直没有 SetValue，
    # 于是板上 174 个元件的 Value 全是封装名。两个后果：
    #   · 装配图/板上读值印的是封装名，看板子的人对不上 BOM
    #   · fanout_channel.py:56 的 `if "DNP" in f.GetValue()` 判据永远为假、
    #     静默失效——DNP 件从来没被真正排除过
    # V4 在 2026-08-28 已修（那边是 170 处），V3 拖到 2026-09-02 阶段 D 才补。
    fp.SetValue(val)
    fp.SetDNP(ref in DNP_REFS)
    FPS[ref] = fp; board.Add(fp)

# ---------------- 放置：固定件 + 区域装箱 ----------------
for ref, (x, y, rot) in PINNED.items():
    FPS[ref].SetPosition(pcbnew.VECTOR2I_MM(float(x), float(y)))
    if rot:
        FPS[ref].SetOrientationDegrees(rot)

# 对扣校验：本板 J1 每个焊盘必须落在基准板 J3 同号引脚的**镜像**位置上。
# 判据来自 docs/hardware/board_pinout-zh_CN.md:76-79（上排=偶 40..2，下排=奇 39..1，
# 下排紧贴板边=Y 大）与 BASEBOARD_REF.md（引脚列 X 34.54..82.80，引脚轴 Y 54.06/56.60）。
# 贴片排针焊盘外扩到 ±2.525（引脚轴 ±1.27），故 Y 期望值 = 55.33 ∓ 2.525。
_J3_PIN1_X, _J3_PITCH = 82.80, 2.54          # 基准板：脚1/2 在最右列
for _num, (_bx, _by) in {
    "1":  (102.5 - _J3_PIN1_X,                 55.33 + 2.525),   # 奇=下排
    "2":  (102.5 - _J3_PIN1_X,                 55.33 - 2.525),   # 偶=上排
    "39": (102.5 - _J3_PIN1_X + 19 * _J3_PITCH, 55.33 + 2.525),
    "40": (102.5 - _J3_PIN1_X + 19 * _J3_PITCH, 55.33 - 2.525),
}.items():
    _p = next(p for p in FPS["J1"].Pads() if p.GetNumber() == _num)
    _gx, _gy = kb(_bx, _by)
    _ax, _ay = pcbnew.ToMM(_p.GetPosition().x), pcbnew.ToMM(_p.GetPosition().y)
    assert abs(_ax - _gx) < 0.01 and abs(_ay - _gy) < 0.01, (
        f"J1 脚{_num} 对扣位置错：实际 ({_ax:.3f},{_ay:.3f}) 期望 ({_gx:.3f},{_gy:.3f})")
print(f"对扣校验通过：J1 脚1 在左端 x={kb(102.5 - _J3_PIN1_X, 0)[0]:.2f}（基准板脚1 在右端 82.80 的镜像）")

GAP = 0.75

# 区域不得压到任何固定件的焊盘包络上。注意：区域现在只用于兜底摆放新元件，
# 已冻结坐标的元件不受区域约束——真正的碰撞检查交给 DRC 的 courtyards_overlap。
for _ref in PINNED:
    _xs, _ys = [], []
    # 占地 = 焊盘 ∪ 封装内禁布区 ∪ 铜层图形。
    # 不能只看焊盘：IFA 天线只有 2 个 1.5mm 小焊盘，本体是 41mm 铜箔加禁铜区，
    # 只看焊盘会让区域直接压到天线上。也不能用 GetBoundingBox（含丝印位号文字，过大）。
    # 必须含 courtyard：J1 排针的塑料件比焊盘向上多出 1.81mm，只看焊盘会让 MCU 区
    # 下界压进塑料件里（实测 R20/R21/R36 与 J1 外框重叠）。
    _cy_bb = FPS[_ref].GetCourtyard(pcbnew.F_CrtYd).BBox()
    _boxes = [_cy_bb] if _cy_bb.GetWidth() > 0 else []
    _boxes += [_p.GetBoundingBox() for _p in FPS[_ref].Pads()]
    _boxes += [_z.GetBoundingBox() for _z in FPS[_ref].Zones()]
    _boxes += [_g.GetBoundingBox() for _g in FPS[_ref].GraphicalItems()
               if _g.GetLayer() in (pcbnew.F_Cu, pcbnew.B_Cu)]
    for _bb in _boxes:
        _xs += [pcbnew.ToMM(_bb.GetLeft()), pcbnew.ToMM(_bb.GetRight())]
        _ys += [pcbnew.ToMM(_bb.GetTop()), pcbnew.ToMM(_bb.GetBottom())]
    if not _xs:
        continue
    _a = (min(_xs) - GAP, min(_ys) - GAP, max(_xs) + GAP, max(_ys) + GAP)
    for _reg, _r in REGIONS.items():
        assert not (_a[0] < _r[2] and _r[0] < _a[2] and _a[1] < _r[3] and _r[1] < _a[3]), (
            f"区域 {_reg}{_r} 与固定件 {_ref} 焊盘包络 "
            f"({_a[0]:.1f},{_a[1]:.1f},{_a[2]:.1f},{_a[3]:.1f}) 重叠")

# 位号丝印预留高度。实测：0603 本体 1.01mm、行距 2.3mm → 行间净空仅 1.29mm，
# 而横排位号需 0.5(离本体) + 1.1(字框) = 1.6mm，只差 0.31mm 就塞不进，
# 于是密集区几十个位号全部无处可放（谁先抢到那条缝谁赢，纯顺序运气）。
# 在非射频区把这段高度算进装箱尺寸，行距自然拉开。射频区不加——拉开=加长射频走线。
# 【实测结论：本板买不起】取 0.6 只需补 0.31mm 缺口，但连锁挤出 18 个元件——
# 其中 C1–C6 是 LDO 去耦电容，被甩到板子另一头。用电气正确性换丝印是亏的。
# 故设 0：密集无源阵列不追求 100% 丝印覆盖，改用装配图（F.Fab + 位号）查件，
# 这也是业界密板的通行做法（贴片按 CPL 坐标走，不依赖丝印）。
# 板子若以后放宽到 120×62 或元件减少，把这个值调回 0.6 即可恢复。
LABEL_RESERVE = {"SUBG":1.2,"MCU":0.8,"LEFT_COL":0.3,"GNSS_BIAS":0.8}   # 区域名 → 预留高度(mm)；见下方按区实测
_cur_reg = None


def _wh(ref):
    w, h = size_mm(FPS[ref])
    if ROTATE.get(ref) in (90, 270):
        w, h = h, w
    pad = 2 * HALO.get(ref, 0.0)     # 光环加在装箱尺寸上，元件仍摆在方框中心
    return w + pad, h + pad + LABEL_RESERVE.get(_cur_reg, 0.0)


def pack(reg, refs):
    global _cur_reg
    _cur_reg = reg
    """把 refs 装进区域 reg，返回装不下的余量（按原顺序）。"""
    rx0, ry0, rx1, ry1 = REGIONS[reg]
    if reg in SORT_BY_HEIGHT:
        refs = sorted(refs, key=lambda r: -_wh(r)[1])
    # 第一遍：分行；行高累计超出区域下界就停，剩下的作为溢出返回
    rows, cur, curw, cy, left = [], [], 0.0, ry0, []
    for i, ref in enumerate(refs):
        w, h = _wh(ref)
        assert w <= rx1 - rx0, f"{ref} 宽 {w:.1f} 超出区域 {reg} 宽 {rx1-rx0:.1f}"
        if cur and curw + GAP + w > rx1 - rx0:          # 换行
            cy += max(x[2] for x in cur) + GAP
            rows.append(cur); cur, curw = [], 0.0
        if cy + h > ry1:                                # 这一行已经超出区域
            left = refs[i:]
            break
        cur.append((ref, w, h)); curw += (GAP if len(cur) > 1 else 0) + w
    if cur and not left:
        rows.append(cur)
    elif cur:
        rows.append(cur)
    # 第二遍：把富余空间**均摊**到行间与行内，而不是全部堆在区域下沿/右沿。
    # 只做紧凑装箱的话，64% 满的区域看起来就是"上面挤满、下面一大片空"（实测 LEFT_COL）。
    # 均摊后密度视觉均匀，同时也把元件间距拉开，给布线让路。
    rows_h = [max(h for _, _, h in row) for row in rows]
    slack_y = (ry1 - ry0) - (sum(rows_h) + GAP * max(0, len(rows) - 1))
    pad_y = max(0.0, slack_y) / (len(rows) + 1) if rows else 0.0
    cy = ry0 + pad_y
    for row, rowh in zip(rows, rows_h):
        mid = cy + rowh / 2
        # 行内水平均摊：射频区禁用——成员顺序即信号流向，拉开间距等于加长射频走线
        roww = sum(w for _, w, _ in row) + GAP * max(0, len(row) - 1)
        slack_x = (rx1 - rx0) - roww
        pad_x = (max(0.0, slack_x) / (len(row) + 1)) if reg in SPREAD_X else 0.0
        cx = rx0 + pad_x
        for ref, w, h in row:
            FPS[ref].SetPosition(pcbnew.VECTOR2I_MM(round(cx + w / 2, 3), round(mid, 3)))
            rot = ROTATE.get(ref)
            if rot:
                FPS[ref].SetOrientationDegrees(rot)
            cx += w + GAP + pad_x
        cy += rowh + GAP + pad_y
    cy -= GAP + pad_y
    assert cy <= ry1 + 0.01, f"区域 {reg} 内部装箱越界：{cy:.2f} > {ry1}"
    return left


# 按 REGIONS 声明顺序处理，保证溢出目标区在源区之后才装箱
# ============ 摆位：PLACEMENT.py 优先，装箱器兜底 ============
# 布局已由人工精修并冻结（见 export_placement.py 的说明）。装箱器只负责
# PLACEMENT.py 里没有的新元件——加了新件先跑一次 gen_pcb.py 让它摆个起点，
# 手工调好后再跑 export_placement.py 固化回去。
try:
    import PLACEMENT
    _FIXED = {t[0]: t[1:] for t in PLACEMENT.PLACEMENT}
except ImportError:
    _FIXED = {}
    print("⚠️ 没有 PLACEMENT.py，全部走区域装箱（首次生成才应如此）")

_placed = 0
for _ref, _t in _FIXED.items():
    if _ref not in FPS:
        continue
    _x, _y, _rot = _t[0], _t[1], _t[2]
    FPS[_ref].SetPosition(pcbnew.VECTOR2I_MM(float(_x), float(_y)))
    if len(_t) > 3 and _t[3] == "B" and not FPS[_ref].IsFlipped():
        FPS[_ref].Flip(FPS[_ref].GetPosition(), False)
    # Flip() 会镜像当前角度；导出的 _rot 是板上最终绝对角度，必须在翻面后设置。
    FPS[_ref].SetOrientationDegrees(_rot)
    _placed += 1
if _FIXED:
    _stale = sorted(set(_FIXED) - set(FPS) - {"H1", "H2", "H3", "H4"})
    assert not _stale, f"PLACEMENT.py 里有已不存在的元件（原理图改过？）：{_stale}"
    print(f"按 PLACEMENT.py 摆位 {_placed} 个元件")

# ⚠️ 位号丝印的位置也必须还原，否则**人工排的丝印一跑流程就全没了**。
# PLACEMENT.py 一直有 SILK_REF 这张表（export_placement.py 在写），
# 但这里从来只读了 PLACEMENT 那张表——存了等于没存。
# 2026-08-13 手工重排了 82 个位号，rebuild 复现出来 84 个坐标对不上，
# 就是这个漏洞：元件位置对了、位号全回到了 KiCad 的默认位置（本体上/下 1.43mm）。
_sref = 0
for _t in getattr(PLACEMENT, "SILK_REF", []) if _FIXED else []:
    _ref, _x, _y, _rot, _ly = _t
    if _ref not in FPS:
        continue
    _r = FPS[_ref].Reference()
    _r.SetPosition(pcbnew.VECTOR2I_MM(float(_x), float(_y)))
    _r.SetTextAngleDegrees(float(_rot))
    _r.SetLayer(board.GetLayerID(_ly))
    _sref += 1
if _sref:
    print(f"按 PLACEMENT.SILK_REF 还原位号丝印 {_sref} 个")

# 装箱器只处理没有固定坐标的新元件
_new = {r: [x for x in v if x not in _FIXED] for r, v in assigned.items()}
_new = {r: v for r, v in _new.items() if v}
if _new:
    print(f"⚠️ 以下元件无固定坐标，走区域装箱兜底: "
          f"{sorted(x for v in _new.values() for x in v)}")
for reg in REGIONS:
    if reg not in _new:
        continue
    left = pack(reg, _new[reg])
    for tgt in OVERFLOW.get(reg, []):
        if not left:
            break
        print(f"  {reg} 装不下 {len(left)} 件 → 溢出到 {tgt}: {left}")
        _new.setdefault(tgt, []).extend(left)
        left = []
    assert not left, (
        f"区域 {reg} 装不下 {len(left)} 件且无溢出区可用：{left}。"
        f"请扩大区域或在 OVERFLOW 里给它指定备用区")

# 安装孔
for i, (hx, hy) in enumerate([kb(5.25, 5.0), kb(97.25, 5.0), kb(5.25, 55.0), kb(97.25, 55.0)], 1):
    h = load_fp("MountingHole", "MountingHole_2.7mm_M2.5")
    h.SetReference(f"H{i}"); h.SetPosition(pcbnew.VECTOR2I_MM(hx, hy)); board.Add(h)

# ---------------- 焊盘挂网 ----------------
by_ref = {fp.GetReference(): fp for fp in board.GetFootprints()}
n_assign = expected = 0
for name, nodes in nets:
    ni = netmap[name]
    for ref, pin in nodes:
        expected += 1
        fp = by_ref.get(ref); assert fp, f"网表引用了不存在的封装 {ref}"
        hit = False
        for pad in fp.Pads():
            if pad.GetNumber() == pin:
                pad.SetNet(ni); n_assign += 1; hit = True
        assert hit, f"{ref} 无焊盘 {pin}"

# SW2.2 位于板内密集区，花焊盘只能形成 1 根连接，KiCad 报
# starved_thermal。这是短接开关的 GND 大焊盘，对 F.Cu GND 用实心连接更稳妥。
_sw2_gnd = next(p for p in by_ref["SW2"].Pads() if p.GetNumber() == "2")
_sw2_gnd.SetLocalZoneConnection(pcbnew.ZONE_CONNECTION_FULL)

# ---------------- 覆铜：6 层 = 双 GND 平面 + 顶底 GND 覆铜 + 3V3_DIG 在 L4 ----------------
# F.Cu(L1)/B.Cu(L6) GND 覆铜（信号在间隙）；In1.Cu(L2)/In4.Cu(L5) 完整 GND 平面；
# In2.Cu(L3) 干净信号层（不铺）；In3.Cu(L4) 铺 3V3_DIG（主数字轨，单 rail 不分割），
# L4 信号在 3V3_DIG 覆铜间隙走、回流走 L5 GND。
M = 0.3
ZONE_LAYERS = [(pcbnew.F_Cu, "GND"), (pcbnew.In1_Cu, "GND"),
               (pcbnew.In3_Cu, "3V3_DIG"), (pcbnew.In4_Cu, "GND"),
               (pcbnew.B_Cu, "GND")]
_keep = []   # SetOutline 接管指针但 SWIG 不转移引用，必须 thisown=0 + 保活
for layer, netname in ZONE_LAYERS:
    z = pcbnew.ZONE(board); z.SetLayer(layer)
    zn = netmap.get(netname); assert zn, f"覆铜网络不存在 {netname}"
    z.SetNet(zn); z.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL)
    ch = pcbnew.SHAPE_LINE_CHAIN()
    for px, py in ((X0+M, Y0+M), (X1-M, Y0+M), (X1-M, Y1-M), (X0+M, Y1-M)):
        ch.Append(pcbnew.VECTOR2I_MM(px, py))
    ch.SetClosed(True)
    poly = pcbnew.SHAPE_POLY_SET(); poly.AddOutline(ch); poly.thisown = 0
    z.SetOutline(poly); z.SetIsFilled(False); _keep.append((poly, ch)); board.Add(z)

# ---------------- 电源岛：给没有平面的电源网一块铜 ----------------
# 只有 GND 和 3V3_DIG 建了平面，3V3_RF(13焊盘)/RP_1V1(5焊盘) 只能靠走线，
# 自动布线器啃不动（实测未连接里 3V3_RF 8 处、RP_1V1 6 处）。
# 放在 **B.Cu**：不动 In1（完整地平面，F.Cu 上 50Ω 共面波导的参考面），
# 也不动 In2（3V3_DIG 平面，数字 IC 靠它入平面）。B.Cu 无元件，挖掉一块地代价最小。
# 范围只盖各自负载所在带，不盖 LDO 本体——LDO 到岛的那一段交给布线器走一根线，
# 8 处未连接变成 1 处。
ISLANDS = [
    (pcbnew.B_Cu, "3V3_RF", (58, 60, 146, 84)),    # 1090 链 + slicer 的射频供电带
    # RP2040 内核供电，紧贴 U8。
    # 试过收窄到 11x4.7mm（想消掉 11 处 isolated_copper 碎铜），**功能上更差**：
    #   大岛 (86,84.5,115,101)：真缺线 18（射频 3），但 11 处浮空碎铜
    #   小岛 (83,84.3,94,89.0) ：碎铜清零，真缺线却涨到 31（射频 7）
    # 浮空碎铜是制造上的瑕疵不是断路，10 多条断线才是真问题——功能优先，保留大岛。
    # RP2040 内核供电。紧凑版 11x4.7mm 刚好罩住 C28/C29，上沿贴 3V3_RF 岛(y≤84)、
    # 下沿避开开关排(U15 y≥89.5)。原来给的 (86,84.5,115,101) 有 478mm²，
    # 被 B.Cu 走线切成 11 块碎铜（DRC isolated_copper x11）——RP_1V1 全网只有 5 个焊盘、
    # 挤在 7x6mm 内，负载也只有 RP2040 内核几十 mA，开那么大纯属自找麻烦。
    # 实测（可复现流水线）：收窄后未连接 30→26、isolated_copper 11→0。
    # 注：更早一次"收窄更差"的结论是在 DSN 规范化排序之前测的，不可信，已推翻。
    # 用 PK_1V1_BIG=1 可切回大岛做对照。
    (pcbnew.B_Cu, "RP_1V1", (86, 84.5, 115, 101) if os.environ.get("PK_1V1_BIG")
     else (83, 84.3, 94, 89.0)),
]
for _n1, _r1 in [(n, r) for _, n, r in ISLANDS]:
    for _n2, _r2 in [(n, r) for _, n, r in ISLANDS]:
        if _n1 >= _n2:
            continue
        assert not (_r1[0] < _r2[2] and _r2[0] < _r1[2] and _r1[1] < _r2[3] and _r2[1] < _r1[3]), \
            f"电源岛 {_n1} 与 {_n2} 相交，同层会打架"
for layer, netname, (ix0, iy0, ix1, iy1) in ISLANDS:
    z = pcbnew.ZONE(board); z.SetLayer(layer)
    zn = netmap.get(netname); assert zn, f"电源岛网络不存在 {netname}"
    z.SetNet(zn); z.SetPadConnection(pcbnew.ZONE_CONNECTION_FULL)
    z.SetAssignedPriority(1)          # 高于底铜 GND(0)，才能从地平面里挖出来
    ch = pcbnew.SHAPE_LINE_CHAIN()
    for px, py in ((ix0, iy0), (ix1, iy0), (ix1, iy1), (ix0, iy1)):
        ch.Append(pcbnew.VECTOR2I_MM(px, py))
    ch.SetClosed(True)
    poly = pcbnew.SHAPE_POLY_SET(); poly.AddOutline(ch); poly.thisown = 0
    z.SetOutline(poly); z.SetIsFilled(False); _keep.append((poly, ch)); board.Add(z)
print(f"电源岛: {[n for _, n, _ in ISLANDS]}（B.Cu，优先级1）")

board.Save(OUT)
assert len(board.GetFootprints()) == len(comps) + 4
print(f"OK: {len(comps)} 元件 + 4 安装孔; 挂网 {n_assign}/{expected}; "
      f"{len(assigned)} 个区域装箱 → {OUT}")
