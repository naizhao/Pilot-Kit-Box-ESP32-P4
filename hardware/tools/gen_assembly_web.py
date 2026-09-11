#!/usr/bin/env python3
"""给手工贴片网页（web/assembly/）生成板子数据。

用法（**必须用 KiCad 自带的 Python**，系统 python3 没有 pcbnew）：

    ~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/\\
        Versions/Current/bin/python3 hardware/tools/gen_assembly_web.py

产出 `web/assembly/data/{v4-pwr,v4-nopwr,v3}.json`，契约见
`hardware/test_assembly_web_contract.py`（跑在系统 python3 上）。

## 几何一律现读 .kicad_pcb，不读 render/ 下的图

`render/*.svg` 和 `*.png` 是某次导出的快照。写这个脚本时 v4 的 render/ 停在
08-28，而 PCB 已经走到 09-04 的 V4.4——拿它当底图，高亮框会指到旧位置上，
而且**看起来完全正常**。焊盘轮廓、板框、元件外框全部从 PCB 当场算。

BOM 只用来补「这是什么料」（材质、耐压、料号、备注），补不到就降级成
值+封装，网页上明确显示「无料号信息」而不是留白——留白会被当成"不用管"。

## 分组和顺序

按料聚合：一组 = 一种料 = BOM 里的一行。组间按封装面积降序（先大后小，
跟 ASSEMBLY.md 的贴片原则一致，大件对位时周围还没有小件被镊子碰歪）；
组内按板上位置从上到下、从左到右扫，减少找焊盘时的眼睛跳跃。
"""

from __future__ import annotations

import collections
import csv
import datetime
import json
import math
import os
import re
import sys
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path

try:
    import pcbnew
except ImportError:  # pragma: no cover - 只在用错解释器时触发
    sys.exit(
        "需要 KiCad 自带的 Python。试：\n"
        "  ~/Applications/KiCad/KiCad.app/Contents/Frameworks/"
        "Python.framework/Versions/Current/bin/python3 " + " ".join(sys.argv)
    )

ROOT = Path(__file__).resolve().parents[2]
OUT_DIR = ROOT / "web" / "assembly" / "data"

NM = 1e6  # KiCad 内部单位是纳米


# ── 变体规则 ──────────────────────────────────────────────────────
# 唯一基准：hardware/expansion-board-v4/SELECTIVE_PLACEMENT-zh_CN.md ①
#
# ⚠️ 这是本文件里**唯一**手抄的东西，所以下面每一条都有断言兜着。
# 手抄 BOM 漏掉 R30 那次没有任何东西报错，照着表贴会让 3V3_RF 与 3V3_DIG
# 经它直流相连——「没报错」正是这类错误的特征。
POWER_SECTION = (
    ["U18", "U19", "U20", "L16", "L17", "J9", "RT1"]
    + [f"C{n}" for n in range(70, 81)]      # C70~C80
    + [f"R{n}" for n in range(37, 47)]      # R37~R46
)
POWER_SECTION_COUNT = 28                    # 文档原文写死的数量，对不上就是抄错了
CC_PULLDOWN = ["R7", "R8"]                  # 5.1k CC 下拉，与电源区逻辑相反

assert len(POWER_SECTION) == POWER_SECTION_COUNT, (
    f"电源区位号展开成 {len(POWER_SECTION)} 个，文档写的是 {POWER_SECTION_COUNT} 个"
)

# ── 「本来就没有器件」的判据 ────────────────────────────────────────
# 安装孔、测试点、焊盘跳线在 PCB 上标了 exclude-from-pos，现读就能滤掉。
# **`ANT1` 滤不掉**——它是板载 IFA 天线，就是 PCB 铜箔本身，但在 PCB 上的属性
# 跟一个普通 0402 电容完全一样（SMD + 2 个焊盘），没有任何几何或属性上的区别。
#
# 唯一的数据源判据是 BOM 的型号列：v4 写「PCB 铜箔（无器件）」，
# v3 写同样的话。下面拿它做判据，并断言 ANT1 确实被命中——BOM 改了措辞
# 判据就会失效，而失效的表现是**天线铜箔被当成元件列进贴片表**，
# 没有任何东西会报错。
NO_PART_MARKER = "无器件"
NO_PART_MUST_MATCH = {"ANT1"}


class Variant:
    """一个可贴装版本：板子 + 选贴规则 + BOM 富化源。"""

    def __init__(self, vid, title, board_dir, pcb_name, subtitle="",
                 force_place=(), force_skip=(), bom=None, archived=False):
        self.id = vid
        self.title = title
        self.subtitle = subtitle
        self.dir = ROOT / "hardware" / board_dir
        self.pcb = self.dir / "kicad" / f"{pcb_name}.kicad_pcb"
        # force_place：PCB 里标了 DNP 但这个版本要贴（R7/R8 在实板上按带电源版标的 DNP）
        self.force_place = set(force_place)
        self.force_skip = set(force_skip)
        self.bom = bom
        # archived：该版本的 PCB 已被后续版本覆盖，几何**重算不出来了**，
        # 只能沿用上次生成的 json。跑的时候不 build，只刷新标题并列进索引。
        # 一块板的 kicad_pcb 在仓库里只有一份，历史版本只存在于 release/ 的 zip 里。
        self.archived = archived


VARIANTS = [
    # ── V4.6（当前板，含 AIRBAND）────────────────────────────────────
    Variant(
        "v4.6-pwr", "V4.6 带电源版", "expansion-board-v4", "expansion-board-v4",
        subtitle="含 AIRBAND 接收（U21/U22/Y4/J10）；贴满电源区，"
                 "R7/R8 绝不能贴（与 CH224K 并存会烧板）",
        force_skip=CC_PULLDOWN,
        bom=("csv", "internal/BOM_MASTER.csv"),
    ),
    Variant(
        "v4.6-nopwr", "V4.6 不带电源版", "expansion-board-v4", "expansion-board-v4",
        subtitle="含 AIRBAND 接收（U21/U22/Y4/J10）；电源区一个不贴，"
                 "R7/R8 必贴（不贴则电脑不认，刷不进固件）",
        force_skip=POWER_SECTION,
        force_place=CC_PULLDOWN,
        bom=("csv", "internal/BOM_MASTER.csv"),
    ),
    # ── V4.5（存档；V4.4 与 V4.5 贴片无差别，只改过一条 GNSS 走线，
    #        所以这份数据贴 V4.4 的板同样适用）────────────────────────
    Variant(
        "v4.5-pwr", "V4.5 带电源版（V4.4 通用）", "expansion-board-v4", "expansion-board-v4",
        subtitle="无 AIRBAND；贴满电源区，R7/R8 绝不能贴（与 CH224K 并存会烧板）",
        force_skip=CC_PULLDOWN,
        bom=("csv", "internal/BOM_MASTER.csv"),
        archived=True,
    ),
    Variant(
        "v4.5-nopwr", "V4.5 不带电源版（V4.4 通用）", "expansion-board-v4", "expansion-board-v4",
        subtitle="无 AIRBAND；电源区一个不贴，R7/R8 必贴（不贴则电脑不认，刷不进固件）",
        force_skip=POWER_SECTION,
        force_place=CC_PULLDOWN,
        bom=("csv", "internal/BOM_MASTER.csv"),
        archived=True,
    ),
    Variant(
        "v3", "V3.10", "expansion-board-v3", "expansion-board-v3",
        # 别在这里写位号数量：卡片上显示的是**过滤掉 DNP 和无器件之后**的待贴数，
        # 跟 PCB 上的位号总数不是一回事，两个数并排放会让人以为漏了件。
        subtitle="单一版本，全部在 F 面，无电源变体",
        bom=("md", "fab/BOM_PURCHASE.md"),
    ),
]


# ── PCB 几何 ──────────────────────────────────────────────────────

def board_outline(board):
    """Edge.Cuts 拆成折线段。圆弧按 1° 采样，画出来就是圆滑的。"""
    segs = []
    for d in board.GetDrawings():
        if d.GetLayer() != pcbnew.Edge_Cuts:
            continue
        kind = d.GetShapeStr()
        if kind == "Line":
            segs.append([(d.GetStart().x, d.GetStart().y),
                         (d.GetEnd().x, d.GetEnd().y)])
        elif kind == "Rect":
            (x0, y0), (x1, y1) = ((d.GetStart().x, d.GetStart().y),
                                  (d.GetEnd().x, d.GetEnd().y))
            corners = [(x0, y0), (x1, y0), (x1, y1), (x0, y1), (x0, y0)]
            segs += [[corners[i], corners[i + 1]] for i in range(4)]
        elif kind in ("Arc", "Circle"):
            c = d.GetCenter()
            r = d.GetRadius()
            if kind == "Circle":
                start_deg, span_deg = 0.0, 360.0
            else:
                start = d.GetStart()
                start_deg = math.degrees(math.atan2(start.y - c.y, start.x - c.x))
                span_deg = d.GetArcAngle().AsDegrees()
            steps = max(2, int(abs(span_deg)))
            pts = []
            for i in range(steps + 1):
                a = math.radians(start_deg + span_deg * i / steps)
                pts.append((c.x + r * math.cos(a), c.y + r * math.sin(a)))
            segs += [[pts[i], pts[i + 1]] for i in range(len(pts) - 1)]
        elif kind == "Polygon":
            poly = d.GetPolyShape()
            for oi in range(poly.OutlineCount()):
                o = poly.Outline(oi)
                pts = [(o.CPoint(i).x, o.CPoint(i).y) for i in range(o.PointCount())]
                pts.append(pts[0])
                segs += [[pts[i], pts[i + 1]] for i in range(len(pts) - 1)]
    return segs


def poly_set_to_lists(poly_set, origin, ndigits=3):
    """SHAPE_POLY_SET → [[[x, y], ...], ...]，坐标换算成相对板框左上角的 mm。"""
    ox, oy = origin
    out = []
    for i in range(poly_set.OutlineCount()):
        o = poly_set.Outline(i)
        pts = [[round((o.CPoint(j).x - ox) / NM, ndigits),
                round((o.CPoint(j).y - oy) / NM, ndigits)]
               for j in range(o.PointCount())]
        if len(pts) >= 3:
            out.append(pts)
    return out


def pad_polygon(pad, origin):
    """焊盘的实际轮廓。

    走 GetEffectivePolygon 而不是「形状 + 尺寸」自己画：圆形、椭圆、圆角矩形、
    倒角矩形、自定义形状在这里全部统一成多边形，网页只需要会画多边形，
    不可能因为漏处理某种形状而画错。
    """
    layers = pad.GetLayerSet()
    layer = pcbnew.F_Cu if layers.Contains(pcbnew.F_Cu) else pcbnew.B_Cu
    polys = poly_set_to_lists(pad.GetEffectivePolygon(layer, pcbnew.ERROR_INSIDE), origin)
    return polys[0] if polys else []


def courtyard_polygon(fp, origin):
    """元件外框。让人一眼看出「这是个大芯片还是个 0402」，不只是几个焊盘。"""
    for layer in (pcbnew.F_CrtYd, pcbnew.B_CrtYd):
        polys = poly_set_to_lists(fp.GetCourtyard(layer), origin)
        if polys:
            return polys[0]
    return []


# ── BOM 富化 ──────────────────────────────────────────────────────

def _split_refs(cell):
    return [r.strip() for r in re.split(r"[,\s，]+", cell or "") if r.strip()]


def load_bom_csv(path):
    """BOM_MASTER.csv：一行一种料，位号列是逗号分隔的列表。"""
    rows = {}
    with open(path, encoding="utf-8-sig", newline="") as fh:
        for row in csv.DictReader(fh):
            info = {
                "category": (row.get("类型") or "").strip(),
                "spec": (row.get("规格") or "").strip(),
                "material": (row.get("材质") or "").strip(),
                "voltage": (row.get("耐压") or "").strip(),
                "part_no": (row.get("品牌/型号") or "").strip(),
                "lcsc": (row.get("嘉立创料号") or "").strip(),
                "note": (row.get("备注") or "").strip(),
                "grade": (row.get("采购等级") or "").strip(),
            }
            for ref in _split_refs(row.get("位号")):
                rows[ref] = info
    return rows


def load_bom_md(path):
    """BOM_PURCHASE.md：markdown 表格，列序见文件里的表头。"""
    rows = {}
    header = None
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if header is None:
            if "位号" in cells:
                header = cells
            continue
        if set("".join(cells)) <= set("-: "):
            continue
        if len(cells) != len(header):
            continue
        rec = dict(zip(header, cells))
        info = {
            "category": rec.get("类别", ""),
            "spec": rec.get("值/规格", ""),
            "material": "",
            "voltage": "",
            "part_no": rec.get("推荐型号", ""),
            "lcsc": "" if rec.get("立创料号", "") in ("—", "-") else rec.get("立创料号", ""),
            "note": rec.get("说明", ""),
            "grade": rec.get("采购", ""),
        }
        for ref in _split_refs(rec.get("位号")):
            rows[ref] = info
    return rows


# ── 元件大类 ──────────────────────────────────────────────────────
# 「贴完电容该贴电阻了」——按大类跳转要有个跨板一致的分类。
#
# **不能用 BOM 的「类型」列**：两块板的口径根本不同。v4 写的是元件类型
# （电容 71 / 电阻 35），v3 写的是**用途**（去耦 35 / 上拉下拉 15 /
# 射频-978匹配 9），v3 里压根没有「电容」这一类。而且都太碎，
# v4 有 31 类、v3 有 37 类，做成导航跟没有一样。
#
# 位号前缀是行业标准（C=电容 R=电阻 L=电感 …），而且**从 PCB 现读**，
# 不受 BOM 措辞影响。长前缀排前面，避免 R 先匹配掉 RT。
KIND_RULES = [
    ("ANT", "天线"), ("FID", "基准点"),
    # FL 必须排在 F 前面，否则 FL1 会被当成保险丝。规则要求前缀后面紧跟数字，
    # 所以 F 匹配不到 FL1——但顺序仍然写对，别指望下一个人也记得这条。
    ("FL", "滤波器"),
    ("RT", "热敏"), ("SW", "开关/跳线"), ("BT", "电池"),
    ("ZS", "匹配网络"), ("ZP", "匹配网络"), ("TP", "测试点"),
    ("C", "电容"), ("R", "电阻"), ("L", "电感"), ("D", "二极管"),
    ("Q", "晶体管"), ("U", "芯片"), ("J", "连接器"), ("Y", "晶振"),
    ("F", "保险丝"), ("H", "安装孔"),
]

# 贴片顺序的**一级**分段。
#
# 原来只按封装面积降序排（ASSEMBLY.md 的「先大后小」工艺原则），结果是
# 0805电容 → 0603电阻 → 0603电容 → 0402电阻 这样交错。工艺上说得通，
# 但手工贴片时**找料才是主要成本**——手边一盒电容，就该一次贴完所有电容，
# 而不是贴两个就去翻电阻盒。
#
# 所以改成：先按大类分段，段内再按尺寸从大到小（工艺原则退到二级，仍然生效，
# 因为同类里大的先贴，热风吹外围也不会掀翻已贴的小件）。
#
# 大类之间的顺序仍守「先大后小」：芯片/连接器这些大件先上板，它们热容大不怕
# 后续热风；数量最多的电容电阻放最后，此时周围大件已就位。
KIND_ORDER = [
    "芯片", "连接器", "滤波器", "晶振", "电感", "二极管", "晶体管",
    "保险丝", "热敏", "电容", "电阻", "匹配网络", "开关/跳线", "电池", "天线",
]


REF_RE = re.compile(r"^([A-Za-z]+)(\d+)$")


def ref_sort_key(ref):
    """位号的自然顺序：R9 排在 R10 前面。

    **必须按数字比，不能按字符串比**——字符串序会得到 R1 R10 R11 R2，
    对着清单核对时看着就像漏了一堆。
    """
    m = REF_RE.match(ref)
    return (m.group(1), int(m.group(2))) if m else (ref, 0)


def part_kind(ref):
    """位号 → 元件大类。认不出来的返回空串，由调用方断言拦下。"""
    for prefix, kind in KIND_RULES:
        if ref.startswith(prefix) and ref[len(prefix):len(prefix) + 1].isdigit():
            return kind
    return ""


# 封装名里的尺寸码：C_0402_1005Metric → 0402。手工贴片时「0402 还是 0603」
# 决定用多大的镊子和风量，比完整封装名有用得多。
SIZE_RE = re.compile(r"_(\d{4})_")


def part_size(fp_name):
    m = SIZE_RE.search(fp_name)
    if m:
        return m.group(1)
    # 非无源件：取封装名里能认的第一段（QFN-48-1EP_7x7mm → QFN-48）
    head = re.split(r"[_（(]", fp_name)[0]
    return head[:14]


# ── 介质 ──────────────────────────────────────────────────────────
# 0402 的 100pF C0G 和 100pF X7R 外观完全一样，混在桌上就分不回来了，
# 而 C34/C35 决定 1090MHz 检波输入高通、**必须锁 C0G/NP0**。所以介质要单独
# 拎出来当标签，不能埋在型号字符串里。
#
# 麻烦在于介质散落在三个字段，还会互相矛盾：C21 那组材质列写 X7R、
# 型号写「风华 0402CG101J500NT（C0G，基础库）」。BOM_PURCHASE 解释过——
# **材质列是要求下限，型号才是实际配的料**（锁 C1546 是图它基础库便宜，
# C0G 是顺带白拿的）。准备料时手里拿的是实物，所以按型号解析，
# 材质列只作为「要求」附注。
DIELECTRIC_RE = re.compile(r"\b(C0G|NP0|X7R|X5R|X6S|X7S|Y5V|Z5U)\b", re.I)


def parse_dielectric(*texts):
    """按给定顺序找第一个介质代号。C0G 和 NP0 是同一种，统一叫 C0G。"""
    for text in texts:
        m = DIELECTRIC_RE.search(text or "")
        if m:
            code = m.group(1).upper()
            return "C0G" if code in ("C0G", "NP0") else code
    return ""


# 采购等级从严到宽。合并同料号的多行时取最严的那个——**降级会把
# 「必买指定」悄悄变成「已有」**，而那正是不能随便拿别的料替代的信号。
GRADE_ORDER = ["必买指定", "有推荐", "通用", "已有"]


def strictest_grade(grades):
    for g in GRADE_ORDER:
        for got in grades:
            if g in (got or ""):
                return got
    return next((g for g in grades if g), "")


def common_prefix(specs):
    """合并组的规格取公共前缀：'100pF' + '100pF C0G' → '100pF'。

    取第一个的话会拿用途当规格——U.FL 那 5 个座子的规格分别写着
    GNSS_EXT / 978 / 1090_EXT / IFA_TEST / 内置patch，挑任何一个顶上去，
    另外 4 个位置就被标错了用途。
    """
    if not specs:
        return ""
    out = specs[0]
    for spec in specs[1:]:
        i = 0
        while i < min(len(out), len(spec)) and out[i] == spec[i]:
            i += 1
        out = out[:i]
    out = out.strip(" （(·-—_/")
    # 公共前缀可能被切没（规格写法完全不同），那就退回最短的那个完整规格，
    # 空标题比错标题好不到哪去
    return out or min(specs, key=len)


def load_bom(variant):
    if not variant.bom:
        return {}
    kind, rel = variant.bom
    path = variant.dir / rel
    if not path.exists():
        print(f"  ⚠️  {path} 不存在，本变体降级为「值 + 封装」")
        return {}
    # 陈旧检测：BOM 比 PCB 老，说明改板后没重生成 BOM，料号可能对不上新位号。
    # 只警告不失败——降级后仍然能贴，只是少了料号，而拦住生成会让人无表可用。
    if path.stat().st_mtime < variant.pcb.stat().st_mtime:
        print(f"  ⚠️  {path.name} 比 PCB 旧，料号信息可能过期")
    return (load_bom_csv if kind == "csv" else load_bom_md)(path)


# ── 采购批次 ──────────────────────────────────────────────────────
# `~/Downloads/BOM_采购清单*.xlsx` 里**一个工作表就是一批料**
# （采购清单 / 补 / 补2 / 补3 / 补4）。贴片时知道"这是第几批买的"，
# 就能直接去翻对应那袋料。
#
# 这是用户的个人采购数据，**单独输出到 batches.json 并 gitignore**，
# 不混进会提交的主数据文件。读不到就不显示批次，其余功能照常。
BATCH_GLOB = os.path.expanduser("~/Downloads/BOM_采购清单*.xlsx")
XL_NS = "{http://schemas.openxmlformats.org/spreadsheetml/2006/main}"
# 采购表「规格要求」列里经常直接写了位号（"C73/C74/C75"、"L17 升压电感"），
# 这是最强的对应证据——比型号字符串匹配可靠得多。
BATCH_REF_RE = re.compile(r"\b((?:C|R|L|D|Q|U|J|Y|FL|F|RT|ZS|ZP|SW)\d{1,3})\b")


def _norm_spec(s):
    return re.sub(r"[\s±%]+", "", (s or "").strip().lower())


def _norm_fp(s):
    s = (s or "").strip()
    m = re.match(r"^(\d{4})", s)          # "0402"、"0603"
    return m.group(1) if m else re.split(r"[/\s]", s)[0].upper()


def load_batches():
    """读采购清单，返回三张索引表：位号 → / 料号 → / (规格,封装) → 批次。"""
    import glob
    files = sorted(glob.glob(BATCH_GLOB), key=os.path.getmtime)
    if not files:
        return None
    # 文件名会变成 "(2)"、"(3)"……取最新的那个，别写死
    path = files[-1]
    z = zipfile.ZipFile(path)
    # xlsx 存字符串有两种方式：共享字符串表，或单元格内联(t="inlineStr")。
    # 全内联的文件**根本没有 sharedStrings.xml**——脚本生成的 xlsx 常是这种
    # （BOM_采购清单_V4.6_AIRBAND.xlsx 就是），直接 z.read 会 KeyError。
    try:
        shared = ["".join(t.text or "" for t in si.iter(XL_NS + "t"))
                  for si in ET.fromstring(z.read("xl/sharedStrings.xml")).iter(XL_NS + "si")]
    except KeyError:
        shared = []
    names = [s.get("name") for s in ET.fromstring(z.read("xl/workbook.xml")).iter(XL_NS + "sheet")]

    by_ref, by_lcsc, by_spec = {}, {}, {}
    total = 0
    for idx, name in enumerate(names, 1):
        try:
            sheet = z.read(f"xl/worksheets/sheet{idx}.xml")
        except KeyError:
            continue
        for row in ET.fromstring(sheet).iter(XL_NS + "row"):
            cells = {}
            for c in row.iter(XL_NS + "c"):
                col = re.match(r"([A-Z]+)", c.get("r")).group(1)
                # 内联字符串的值在 <is><t> 里，没有 <v>。不认它的话整列文本
                # 全被 continue 跳过，表面上「读到了 0 行」而不报错。
                if c.get("t") == "inlineStr":
                    is_node = c.find(XL_NS + "is")
                    if is_node is not None:
                        cells[col] = "".join(t.text or "" for t in is_node.iter(XL_NS + "t"))
                    continue
                v = c.find(XL_NS + "v")
                if v is None:
                    continue
                cells[col] = shared[int(v.text)] if c.get("t") == "s" else v.text
            if cells.get("A") in (None, "类型"):
                continue
            total += 1
            info = {
                "n": idx, "name": name,
                "spec": cells.get("B", ""), "fp": cells.get("C", ""),
                "qty": cells.get("F", ""), "mpn": cells.get("G", ""),
                "bought": cells.get("J", ""),
            }
            for ref in BATCH_REF_RE.findall(cells.get("I", "") or ""):
                by_ref.setdefault(ref, info)
            lcsc = (cells.get("H") or "").strip()
            if re.match(r"^C\d+$", lcsc):
                by_lcsc.setdefault(lcsc, info)
            key = (_norm_spec(cells.get("B")), _norm_fp(cells.get("C")))
            if key[0]:
                by_spec.setdefault(key, info)

    print(f"  采购批次：{os.path.basename(path)}  {len(names)} 批 / {total} 行")
    return {"file": os.path.basename(path), "names": names,
            "by_ref": by_ref, "by_lcsc": by_lcsc, "by_spec": by_spec}


def size_conflict(a, b):
    """两个封装是否**明确**冲突。

    只在两边都是 4 位数字尺寸码时才判（0402 vs 0603 =冲突）。写法不同不算
    冲突——采购表把电感封装写成「4x4mm 一体成型」而板上是 `L_Bourns-SRN4018`，
    全量比对会把正确的匹配全部误杀。
    """
    na, nb = _norm_fp(a), _norm_fp(b)
    return na.isdigit() and nb.isdigit() and len(na) == 4 == len(nb) and na != nb


def match_batch(group, idx, warn=None):
    """给一组料找采购批次。三级判据，可靠的优先。"""
    if not idx:
        return None
    # ① 位号直接写在「规格要求」里——最强证据，没有歧义。
    #    但仍然查一次封装：位号是手写的，写错一个数字就会把人指到
    #    另一盘料上（0402 和 0603 的 100pF 外观一样，拿错看不出来）。
    for part in group["parts"]:
        got = idx["by_ref"].get(part["ref"])
        if not got:
            continue
        if size_conflict(got["fp"], group["size"]):
            if warn is not None:
                warn.append(f"{part['ref']}：采购表写在「{got['spec']} {got['fp']}」行下，"
                            f"但板上是 {group['spec']} {group['size']}——采购表位号可能笔误")
            continue
        return {**got, "by": "位号"}

    # ② 立创料号。**必须交叉验证封装**：这张表的料号列栽过跟头
    #    （2026-08-29 核出 7 处料号错位，型号名是对的、料号列串了行）。
    #    封装对不上说明这个料号指向了别的料，宁可降级也不能显示错的批次——
    #    那会让人去翻错的料袋。
    if group["lcsc"]:
        got = idx["by_lcsc"].get(group["lcsc"])
        if got and _norm_fp(got["fp"]) == _norm_fp(group["size"]):
            return {**got, "by": "料号"}

    # ③ 规格 + 封装
    got = idx["by_spec"].get((_norm_spec(group["spec"]), _norm_fp(group["size"])))
    if got:
        return {**got, "by": "规格"}
    return None


# ── 组装 ──────────────────────────────────────────────────────────

def pad_span(part):
    """焊盘包络的对角线长度，用来判断两个件是不是同一个物理尺寸。

    不能拿封装名判断：ZS1 和 R21 都是 0603 的 0R 电阻、同一个料号 C21189，
    但 PCB 上 ZS1 用的是 `L_0603_1608Metric`（π 网络那个位置设计上可以贴
    0R 或电感，所以留了电感封装），R21 用 `R_0603_1608Metric`。
    按名字分它们会被拆成两盘料，按尺寸分才对得上实物。
    """
    pts = [q for pad in part["pads"] for q in pad["poly"]]
    if not pts:
        return 0.0
    xs = [q[0] for q in pts]
    ys = [q[1] for q in pts]
    return math.hypot(max(xs) - min(xs), max(ys) - min(ys))


def fp_area(part):
    """封装面积，用元件外框算；没有外框就用焊盘包络。用来做先大后小排序。"""
    poly = part["courtyard"] or [p for pad in part["pads"] for p in pad["poly"]]
    if not poly:
        return 0.0
    xs = [p[0] for p in poly]
    ys = [p[1] for p in poly]
    return (max(xs) - min(xs)) * (max(ys) - min(ys))


def build(variant):
    board = pcbnew.LoadBoard(str(variant.pcb))
    bbox = board.GetBoardEdgesBoundingBox()
    origin = (bbox.GetLeft(), bbox.GetTop())
    bom = load_bom(variant)

    on_board = {fp.GetReference() for fp in board.GetFootprints()}
    # 断言手抄的变体规则确实落在这块板上。位号写错（比如 C81 写成 C18）不会
    # 报错，只会静静地少贴/多贴一个件——这里当场失败。
    for ref in variant.force_skip | variant.force_place:
        assert ref in on_board, f"{variant.id}: 规则里的 {ref} 在 {variant.pcb.name} 上不存在"

    parts = []
    no_part_hits = set()
    for fp in board.GetFootprints():
        ref = fp.GetReference()
        # 不贴的四类：设计 DNP、PCB 上标了 exclude-from-pos 的（安装孔/测试点/
        # 焊盘跳线）、BOM 标「无器件」的（ANT1 铜箔天线，PCB 属性上认不出来）、
        # 本变体的选贴规则。前两类现读 PCB，改板后自动跟随。
        info = bom.get(ref) or {}
        no_part = NO_PART_MARKER in (info.get("part_no") or "")
        if no_part:
            no_part_hits.add(ref)
        skipped = (fp.IsDNP()
                   or bool(fp.GetAttributes() & pcbnew.FP_EXCLUDE_FROM_POS_FILES)
                   or no_part)
        if ref in variant.force_place:
            skipped = False
        if ref in variant.force_skip:
            skipped = True
        if skipped:
            continue

        pads = []
        for pad in fp.Pads():
            poly = pad_polygon(pad, origin)
            if poly:
                pads.append({"n": pad.GetNumber(), "poly": poly})
        if not pads:
            print(f"  ⚠️  {ref} 没有焊盘几何，跳过")
            continue

        pos = fp.GetPosition()
        parts.append({
            "ref": ref,
            "val": fp.GetValue(),
            "fp": fp.GetFPIDAsString().split(":")[-1],
            "layer": "B" if fp.GetLayerName() == "B.Cu" else "F",
            "x": round((pos.x - origin[0]) / NM, 3),
            "y": round((pos.y - origin[1]) / NM, 3),
            "rot": round(fp.GetOrientationDegrees(), 1),
            # 分类挂在**位号**上而不是组上：同一料号可以跨大类，
            # C21189 那盘 0R 里 ZS1 属匹配网络、R21~R23 属电阻。
            # 挂在组上会让 ZS1 从「匹配网络」里消失。
            "kind": part_kind(ref),
            "pads": pads,
            "courtyard": courtyard_polygon(fp, origin),
            "bom": bom.get(ref),
            # 这个位号自己那行 BOM 的规格。合并同料号的多行之后，组标题只剩公共
            # 部分（"U.FL"），而这 5 个座子分别是 GNSS_EXT / 978 / 1090_EXT /
            # IFA_TEST / 内置patch——**接错口就是接错天线**，用途不能在合并时丢掉。
            "use": (bom.get(ref) or {}).get("spec", ""),
        })

    # 判据自检：BOM 措辞一改，「无器件」就匹配不上，天线铜箔会被当成元件
    # 列进贴片表，而且看起来完全正常。这里当场失败。
    missed = NO_PART_MUST_MATCH & on_board - no_part_hits
    assert not missed, (
        f"{variant.id}: {sorted(missed)} 没被「{NO_PART_MARKER}」判据命中——"
        f"检查 {variant.bom[1] if variant.bom else 'BOM'} 的型号列措辞是否改了"
    )

    # 每个件都必须能归类。**漏一类就是漏贴一批件**，而按分类导航的人
    # 根本不会发现——那一类压根不在导航条上。
    unknown = sorted({p["ref"] for p in parts if not part_kind(p["ref"])})
    assert not unknown, (
        f"{variant.id}: 这些位号认不出元件大类：{unknown}——"
        f"给 KIND_RULES 补规则，否则它们不会出现在任何分类里"
    )

    groups = group_parts(parts)
    return {
        "id": variant.id,
        "title": variant.title,
        "subtitle": variant.subtitle,
        "source": str(variant.pcb.relative_to(ROOT)),
        # 产物会被提交，改板后不重跑就会悄悄过期。把源 PCB 的日期带出来显示在
        # 页面上——对不上就知道该重跑，而不是照着旧坐标贴完一整块板。
        "pcb_date": datetime.datetime.fromtimestamp(
            variant.pcb.stat().st_mtime).strftime("%Y-%m-%d %H:%M"),
        "board": {
            "w": round(bbox.GetWidth() / NM, 3),
            "h": round(bbox.GetHeight() / NM, 3),
            "edges": [[round((x1 - origin[0]) / NM, 3), round((y1 - origin[1]) / NM, 3),
                       round((x2 - origin[0]) / NM, 3), round((y2 - origin[1]) / NM, 3)]
                      for (x1, y1), (x2, y2) in board_outline(board)],
        },
        "total": len(parts),
        "groups": groups,
    }


def group_parts(parts):
    """按料聚合：一组 = 一盘料。

    分组键优先用**立创料号**，不是规格字符串。BOM 里同一个料号常常按用途拆成
    好几行——C1546 拆成「100pF」和「100pF C0G」两行、C41432122 那款 U.FL 座
    按 5 个用途拆成 5 行。照规格分组的话，同一盘料要摸 5 次，而按料聚合的
    全部意义就是少摸几次。

    料号本身就确定了尺寸，所以用料号时**不再拿封装名做键**——封装库名会因为
    用途不同而不同（见 pad_span 里 ZS1 的例子）。尺寸一致性改用焊盘包络断言，
    比名字可靠。

    没有料号才退回「规格 + 封装」，这时必须带封装：同样标 1uF 的 0402 和
    0603 是两种料，合在一组会让「准备 10 个 1uF」拿错尺寸。
    """
    buckets = {}
    for part in parts:
        info = part["bom"]
        lcsc = (info or {}).get("lcsc", "")
        if lcsc:
            key = lcsc
        elif info and info["spec"]:
            key = f"{info['spec']}|{part['fp']}"
        else:
            key = f"{part['val']}|{part['fp']}"
        buckets.setdefault(key, []).append(part)

    groups = []
    for key, members in buckets.items():
        infos = [p["bom"] for p in members if p["bom"]] or [{}]
        # 同一料号的多行去重后合并。**介质不一致就是数据出问题了**——
        # 一个料号不可能既是 C0G 又是 X7R，合并会让人照着错的介质拿料，
        # 而射频位拿错介质是电路故障，不是外观问题。
        seen, rows = set(), []
        for info in infos:
            sig = (info.get("spec"), info.get("part_no"), info.get("note"))
            if sig not in seen:
                seen.add(sig)
                rows.append(info)

        dielectrics = {parse_dielectric(r.get("part_no"), r.get("spec"), r.get("material"))
                       for r in rows}
        dielectrics.discard("")
        assert len(dielectrics) <= 1, (
            f"{key}: 同一料号解析出多种介质 {sorted(dielectrics)}——"
            f"BOM 数据有问题，合并会让人拿错料"
        )

        # 同一料号必须对应同一个物理尺寸。名字可以不同（R_0603 / L_0603），
        # 尺寸不能——**把 0402 和 0603 合成一盘料，人就会照着拿错**，
        # 而贴上去之前看不出来。1.5 倍留给同尺寸封装的焊盘画法差异。
        spans = [pad_span(p) for p in members]
        if min(spans) > 0 and max(spans) / min(spans) > 1.5:
            big = max(members, key=pad_span)
            small = min(members, key=pad_span)
            raise AssertionError(
                f"{key}: 同一料号下焊盘尺寸差太多——"
                f"{big['ref']}({big['fp']}, {max(spans):.2f}mm) vs "
                f"{small['ref']}({small['fp']}, {min(spans):.2f}mm)。"
                f"检查 BOM 是不是把两种尺寸配了同一个料号"
            )

        fps = collections.Counter(p["fp"] for p in members)
        group_kind = collections.Counter(p["kind"] for p in members).most_common(1)[0][0]
        specs = [r.get("spec") for r in rows if r.get("spec")]
        notes = [r.get("note", "").strip() for r in rows]
        # 材质列是**要求下限**，跟实配介质可以不同（C21 组要求 X7R、实配 C0G）
        required = sorted({r.get("material", "").strip() for r in rows} - {""})

        # 组内按**位号**排：D2 → D3 → D4 → D5。
        #
        # 原来按板上位置排（y 分箱 + x），想的是减少找焊盘时的眼睛跳跃，
        # 结果贴出来是 D2 → D4 → D5 → D3——对着位号清单核对时完全对不上，
        # 也说不清自己贴到哪了。位号有序才能一眼看出漏没漏。
        members.sort(key=lambda p: ref_sort_key(p["ref"]))
        groups.append({
            "key": key,
            "spec": common_prefix(specs) if specs else members[0]["val"],
            # 拆行时各行的规格差异（用途/版本）留着，位号多的时候要靠它分辨
            "spec_variants": specs if len(specs) > 1 else [],
            # 同一盘料可能落在多个封装名下（R_0603 / L_0603），显示用得最多的那个，
            # 其余留在 fp_variants 里——不显示出来会让人以为板上没有那个封装
            "fp": fps.most_common(1)[0][0],
            "fp_variants": [f for f, _ in fps.most_common()[1:]],
            # 0402 / 0603 决定用多大镊子和风量，比完整封装名有用
            "size": part_size(fps.most_common(1)[0][0]),
            # 组的大类取组内多数位号，并**回填给每个位号**（见下面的 parts）。
            #
            # 一开始是让每个位号各自按前缀归类的，结果 C21189 那盘 0R 里
            # ZS1 归「匹配网络」、R21~R23 归「电阻」，这一组排在电阻段，
            # 中间那个匹配网络就把电阻段劈成了两截——贴电阻贴到一半冒出
            # 一个别的类。
            #
            # 分类的粒度必须跟「一盘料」对齐，因为分类导航是用来**找料**的：
            # ZS1 用的就是电阻那盘料，找料时它就该在电阻里。它的特殊用途
            # 由 use（"0R 串"）表达，不靠分类。
            "kind": group_kind,
            "dielectric": next(iter(dielectrics), ""),
            "required": " / ".join(required),
            "category": next((r.get("category") for r in rows if r.get("category")), ""),
            "voltage": next((r.get("voltage") for r in rows if r.get("voltage")), ""),
            "part_no": next((r.get("part_no") for r in rows if r.get("part_no")), ""),
            "lcsc": next((r.get("lcsc") for r in rows if r.get("lcsc")), ""),
            "note": " ｜ ".join(n for n in dict.fromkeys(notes) if n),
            "grade": strictest_grade([r.get("grade") for r in rows]),
            "has_bom": bool(members[0]["bom"]),
            "qty": len(members),
            # use 跟组标题一样就是冗余，去掉，省得每行都重复一遍 "100pF"。
            # kind 一律回填成组的大类，保证推进序列里同类连成一段。
            "parts": [
                {**{k: v for k, v in p.items()
                    if k != "bom" and not (k == "use" and v == common_prefix(specs))},
                 "kind": group_kind}
                for p in members
            ],
        })

    # 一级按大类分段——手边一盒电容就一次贴完所有电容，不用贴两个就去翻电阻盒。
    # 二级才是「先大后小」：同类里大件先贴，热风吹外围不会掀翻已贴的小件。
    #
    # KIND_ORDER 里没列到的大类排在最后（而不是排在最前），这样将来加了新
    # 元件类型，它只是顺序靠后，不会插在电容中间把分段打散。
    def sort_key(g):
        try:
            rank = KIND_ORDER.index(g["kind"])
        except ValueError:
            rank = len(KIND_ORDER)
        return (rank, -max(fp_area(p) for p in g["parts"]))

    groups.sort(key=sort_key)
    return groups


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    batch_idx = load_batches()
    if batch_idx is None:
        print(f"  ⚠️  没找到 {BATCH_GLOB}，本次不生成采购批次")

    index, batches_out = [], {}
    for variant in VARIANTS:
        print(f"→ {variant.id}  {variant.title}")
        path = OUT_DIR / f"{variant.id}.json"
        if variant.archived:
            # 存档版本：PCB 已被后续版本覆盖，几何重算不出来，沿用上次的数据。
            # 文件不在就**直接失败**——静默跳过会让网页上少一个版本而没人发现。
            if not path.exists():
                raise SystemExit(
                    f"存档版本 {variant.id} 的数据文件不存在：{path}\n"
                    f"它无法重新生成（当前 PCB 已是新版本）。"
                    f"只能从 release/ 里对应版本的 KiCad zip 解出 PCB 后重建。")
            doc = json.loads(path.read_text(encoding="utf-8"))
            # 标题/副标题仍以脚本为准，这样改了措辞能同步进数据文件
            doc["title"], doc["subtitle"] = variant.title, variant.subtitle
        else:
            doc = build(variant)
        path.write_text(json.dumps(doc, ensure_ascii=False, separators=(",", ":")),
                        encoding="utf-8")
        size_kb = path.stat().st_size / 1024
        print(f"   {'[存档沿用] ' if variant.archived else ''}"
              f"{doc['total']} 个位号 / {len(doc['groups'])} 种料 → "
              f"{path.relative_to(ROOT)}  {size_kb:.0f} KB")
        index.append({"id": variant.id, "title": variant.title,
                      "subtitle": variant.subtitle, "total": doc["total"],
                      "groups": len(doc["groups"])})

        if batch_idx:
            hits, warns = {}, []
            for group in doc["groups"]:
                got = match_batch(group, batch_idx, warns)
                if got:
                    hits[group["key"]] = got
            batches_out[variant.id] = hits
            by = collections.Counter(v["by"] for v in hits.values())
            print(f"   批次匹配 {len(hits)}/{len(doc['groups'])} 组"
                  f"（{'、'.join(f'{k} {n}' for k, n in by.most_common())}）")
            for w in dict.fromkeys(warns):
                print(f"   ⚠️  {w}")

    (OUT_DIR / "index.json").write_text(
        json.dumps(index, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"\n索引 → {(OUT_DIR / 'index.json').relative_to(ROOT)}")

    if batch_idx:
        # 采购批次是个人数据（含采购量、已购状态），**单独一个文件并 gitignore**，
        # 不混进会提交的主数据。网页读不到它就不显示批次，其余功能不受影响。
        bp = OUT_DIR / "batches.json"
        bp.write_text(json.dumps(
            {"file": batch_idx["file"], "names": batch_idx["names"], "variants": batches_out},
            ensure_ascii=False, separators=(",", ":")), encoding="utf-8")
        print(f"采购批次 → {bp.relative_to(ROOT)}（个人数据，已 gitignore）")


if __name__ == "__main__":
    main()
