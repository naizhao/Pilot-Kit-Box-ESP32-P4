#!/usr/bin/env python3
"""生成手工贴片（热风枪/加热台）用的分阶段点位清单。

2026-08-20 评审提出：初期先手工吹片，需要一份点位清单；另外板上有不少
调试位，要在清单里标出来。

## 为什么分阶段，而不是一张大表从头贴到尾

这块板有 136 个贴片位、5 个功能域。一次全贴完再上电，出了问题要在 136 个
焊点里找，而且射频那几颗（QPL9547 ¥8.5、TA0970A ¥7.5×2、AD8319 ¥21.9）
单价最高、最靠后——前面电源要是有问题，这些就一起烧了。

所以按**电源 → 数字 → 传感器 → 射频**分五阶段，每阶段贴完就能独立验证，
出问题的范围锁定在刚贴的那十几个件里。贵的射频件排最后。

## 阶段内的顺序：先大后小、先中间后外围

不管是整板回流还是逐个热风吹，结论一样：

  · 整板回流——大件对位要反复调整，周围先摆了 0402 会被镊子碰歪
  · 逐个热风——吹外围时热风会掀翻已贴好的小件；大件热容大，反过来不怕

所以每个阶段内部按**封装面积从大到小**排。

## 模块归属来自原理图分页

不是我按位号猜的，是网表里的 sheetpath：
  /Power/ /MCU_RP2040/ /Sensors_GNSS/ /SubGHz_978/ /RF_1090/ /Interface_J3/

用法：gen_assembly.py    写 ../ASSEMBLY.md
"""
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

import pcbnew

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from board_meta import BOARD_REV                    # noqa: E402

# 已经打出来的板子上的丝印错误。
#
# 这里必须硬编码，不能靠检测——**源文件一修好，检测器就报 0 了，但那批板子
# 已经印出来了**。拿着 V3.2 实物贴片的人照样会被误导，文档不能因为源文件修好
# 就把警告撤掉。
#
# 每条：(位号A, 位号B, 两者尺寸是否相同)。尺寸相同 = 能塞进对方位置 = 会真贴错。
AS_BUILT_SILK_SWAPS = {
    "V3.2": [("R33", "C25", True), ("C46", "C47", True),
             ("L14", "C30", True), ("C18", "C6", False)],
}

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PCB = os.path.join(T, "kicad", "expansion-board-v3.kicad_pcb")
SCH = os.path.join(T, "kicad", "expansion-board-v3.kicad_sch")
NET = "/tmp/expansion.net.xml"
# v3 已归档：根目录的 ASSEMBLY.md 是冻结的公开快照（英文正名），
# 归档管线的再生成产物一律落 internal/，不覆盖公开文件
OUT = os.path.join(T, "internal", "ASSEMBLY.md")

if not os.path.exists(NET):
    CLI = os.path.expanduser("~/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli")
    subprocess.run([CLI, "sch", "export", "netlist", "--format", "kicadxml",
                    "-o", NET, SCH], check=True, capture_output=True)

# 贴片阶段。顺序 = 调试依赖顺序：电源不通后面全白搭；MCU 起来了才能读传感器。
# 贵的射频件排最后——前面任何一级出问题，它们还没上板。
STAGES = [
    ("A", "电源", "/Power/",
     "3 个 LDO 加各路的输入输出电容，**这 9 件自成闭环**，贴完就能上电量电压，"
     "不用碰任何贵件。",
     "实验电源限流 100mA，5V 正极夹 `J1.1`、负极夹 `U10.49`（CC1312R 的 EP 大焊盘，"
     "此时还没贴件，焊盘是空的正好夹）。\n"
     "  - 空载电流应该只有几 mA；一上电顶到限流就断电查 U1/U2/U3 有没有连锡\n"
     "  - `U9.8` 量 3V3_DIG = 3.3V ±5%\n"
     "  - `U14.1` 量 3V3_RF = 3.3V ±5%\n"
     "  - `Q2.2` 量 3V3_GNSS = 3.3V ±5%\n"
     "  三路都对了才往下走。这一步没过，后面贴什么都是白贴。"),
    ("B", "MCU + Flash", "/MCU_RP2040/",
     "RP2040 + QSPI Flash + 12MHz 晶振 + USB-C 座。",
     "USB-C 插电脑，镊子短接 `SW2`(BOOTSEL) 再上电 → 电脑应出现 **RPI-RP2** U 盘。\n"
     "  - 认不到 U 盘：先量 `C28.1` 上的 RP_1V1 有没有 1.1V（RP2040 内部 LDO 出的）\n"
     "  - 认到了但 Flash 不识别：查 U9 的四条 QSPI 线，SOIC-8 引脚在外，烙铁能补"),
    ("C", "传感器 + GNSS", "/Sensors_GNSS/",
     "IMU / 气压 / 磁力计 / GNSS，都是 I2C 和 UART 慢速接口。",
     "刷个扫描固件，I2C 上逐个点名：BNO085、BMP388、QMC5883P 应各自应答。\n"
     "  - GNSS 看 `U7.2`(GNSS_TXD) 有没有 NMEA 输出，**室内收不到星是正常的**，"
     "有 `$GPTXT` 和 `$GPGSV` 输出就说明模块活着"),
    ("D", "978 收发", "/SubGHz_978/",
     "CC1312R 和它的 48MHz / 32.768kHz 两颗晶振。",
     "接调试器到 `TP1`(SWCLK) / `TP2`(SWDIO)，能读到 CC1312R 的 ID 就算通。\n"
     "  - 起不来先量 `C60.1` 的 SUBG_VDDR（内部 DC/DC 输出，约 1.7V）"
     "和 `C67.1` 的 SUBG_DCOUPL（约 1.28V）"),
    ("E", "1090 接收链", "/RF_1090/",
     "**全板最贵的一段**（QPL9547 ¥8.5 / TA0970A ¥7.5×2 / AD8319 ¥21.9），"
     "排最后——前面四级都验过了才让它们上板。",
     "`TP3`~`TP6`(DEMOD0-3) 和 `TP7`(RECOVERED_CLK) 接示波器。\n"
     "  - 射频件方向错了很难看出来，贴之前对着 `render/silk.svg` 核一遍 pin1"),
    ("F", "对外接口", "/Interface_J3/",
     "2×20 排针，热容量大。",
     "跟主板对插，量 5V 和 GND 通断。**放最后**：先贴上它，前面几阶段板子就没法"
     "平放在加热台上了。"),
]

# 不贴的位置。DNP 是设计上就默认不贴，不是漏了。
DNP = re.compile(r"DNP")
# ⚠️ MountingHole 必须列进来：它只画在 PCB 上、原理图里没有，所以网表读不到，
# sheetpath 是空的。不显式排除的话它既不属于任何阶段、也不在"不贴"名单里，
# 会从清单中静默消失——这正是下面那条 assert 抓到的。
NO_PART_FP = ("TestPoint_Pad", "SolderJumper", "ANT_IFA", "MountingHole")
# AD8313 和 AD8319 是二选一的双检波实验位，默认贴 AD8319(U13)。
ALT_POSITION = {"U14": "AD8313 备用检波位，与 U13(AD8319) 二选一，默认不贴"}

# 手工难度。按「有没有藏在底下的焊盘」分——这决定了能不能用烙铁救。
def difficulty(fp, w, h):
    if re.search(r"QFN|DFN|LGA|LFCSP|LCC-18|SMD3838", fp):
        return "★★★ 底部焊盘，只能热风，烙铁够不着"
    if re.search(r"SOT-363|SC-70", fp):
        return "★★  0.65mm 间距，引脚在外但很密"
    if re.search(r"SOIC|MSOP|SOT-23|SOD-|SMD32", fp):
        return "★   引脚在外，烙铁可补焊"
    if re.search(r"_0402_", fp):
        return "★★  0402，热风易吹跑，风量调小"
    return "★   常规"


def find_silk_swaps(parts):
    """找出位号丝印被印到别人身上的对子。

    2026-08-20 评审发现 C25/R33 丝印错位，一查全板有 4 对。板子已经印出来了，
    改不了，只能在清单里标出来让人别信丝印。

    ## 判据踩过的两个坑

    ① **方向要反过来**。第一版是「对每个文字，找离它最近的元件」，漏掉了
       C46/C47——它俩的文字都被挤到 U10 那颗大 QFN 边上，最近元件都算成 U10，
       于是"看起来没跟谁对调"。实际人是**站在元件旁边找字**，所以要对每个元件
       找最近的文字，再看那个文字是不是它自己的。

    ② **不能用封装名判断能不能贴错**。`R_0603_1608Metric` 和 `C_0603_1608Metric`
       名字不同，物理尺寸却都是 1.6×0.8mm，照样能塞进对方位置。必须比实际
       包围盒尺寸。只有 C18(0805) vs C6(0603) 这种才是真塞不进去。
    """
    import math

    def d2box(p, bx):
        l, t, r, bo = bx
        return math.hypot(max(l - p[0], 0, p[0] - r), max(t - p[1], 0, p[1] - bo))

    swaps = []
    ks = list(parts)
    for i in range(len(ks)):
        for j in range(i + 1, len(ks)):
            a, c = parts[ks[i]], parts[ks[j]]
            if not (a.get("tvis") and c.get("tvis")):
                continue
            # 互换 = A 的文字更贴 B 的本体，同时 B 的文字更贴 A 的本体
            if (d2box(a["t"], c["bx"]) < d2box(a["t"], a["bx"]) and
                    d2box(c["t"], a["bx"]) < d2box(c["t"], c["bx"])):
                fit = (abs(a["w"] - c["w"]) < 0.3 and abs(a["h"] - c["h"]) < 0.3)
                swaps.append((ks[i], ks[j], fit))
    return swaps


tree = ET.parse(NET)
sheet, value = {}, {}
for c in tree.iter("comp"):
    r = c.get("ref")
    sp = c.find("sheetpath")
    sheet[r] = sp.get("names") if sp is not None else "?"
    v = c.find("value")
    value[r] = v.text if v is not None else "?"

board = pcbnew.LoadBoard(PCB)
parts = {}
dirty_ref = []
for f in board.GetFootprints():
    r = f.GetReference()
    # ⚠️ PCB 上的位号可能带首尾空格——在 KiCad 里手工编辑位号时很容易敲进去，
    # 界面上和丝印上都看不出来（`C52 ` 和 `C52` 印出来一模一样）。
    # 但它会让位号跟网表对不上：这个件既不属于任何阶段、也不在"不贴"名单里，
    # 从清单里静默消失。下面的 assert 就是这么抓到的。
    # 更要命的是出 SMT 单时——BOM 的位号来自网表（干净），坐标文件来自 PCB（带空格），
    # 两边对不上，那个位置嘉立创就不会贴。
    if r != r.strip():
        dirty_ref.append(r)
        r = r.strip()
    fp = f.GetFPIDAsString().split(":")[-1]
    bb = f.GetBoundingBox()
    nb = f.GetBoundingBox(False, False)          # 不含文字，否则位号会把盒子撑大
    parts[r] = {
        "fp": fp, "val": value.get(r, "?"), "sheet": sheet.get(r, "?"),
        "x": f.GetPosition().x / 1e6, "y": f.GetPosition().y / 1e6,
        "rot": f.GetOrientationDegrees(),
        "area": (bb.GetWidth() / 1e6) * (bb.GetHeight() / 1e6),
        "w": nb.GetWidth() / 1e6, "h": nb.GetHeight() / 1e6,
        "bx": (nb.GetLeft() / 1e6, nb.GetTop() / 1e6,
               nb.GetRight() / 1e6, nb.GetBottom() / 1e6),
        "t": (f.Reference().GetPosition().x / 1e6,
              f.Reference().GetPosition().y / 1e6),
        "tvis": f.Reference().IsVisible(),
    }

skip = {}
for r, p in parts.items():
    if "MountingHole" in p["fp"]:
        skip[r] = "M2.5 安装孔（⌀2.7mm NPTH），不是元件"
    elif "TestPoint" in p["fp"]:
        skip[r] = "测试点焊盘，探针/飞线用，不贴件"
    elif "SolderJumper" in p["fp"]:
        skip[r] = "短接焊盘，用镊子短接，不贴件"
    elif any(s in p["fp"] for s in NO_PART_FP):
        skip[r] = "板载天线，PCB 走线本身，无器件"
    elif DNP.search(p["val"]):
        skip[r] = f"DNP，设计上默认不贴（{p['val']}）"
    elif r in ALT_POSITION:
        skip[r] = ALT_POSITION[r]

out = [f"# {BOARD_REV} 手工贴片点位清单\n",
       "> 由 `tools/gen_assembly.py` 从原理图分页 + PCB 坐标自动生成，改板后重跑。\n",
       "\n配套看图：板上位置 `render/top.png`，丝印和 pin1 `render/silk.svg`。\n"]

for rev, known in AS_BUILT_SILK_SWAPS.items():
    out += ["\n---\n",
            f"\n# ⚠️ 手上是 {rev} 那批板的话，先看这个\n",
            f"\n**{rev} 实物板上有 {len(known)} 对位号丝印是对调的。**",
            f"源文件已在 {BOARD_REV} 修好，但 {rev} 那批已经印出来了，改不了——",
            "贴这几个位置时**只认下表的坐标，不要看板上的字**。\n",
            "\n阴在于：每个元件旁边都有位号，排版看着完全正常，只是内容张冠李戴，",
            "不像缺字漏字那样一眼能发现。目检、DRC、Gerber 预览全都发现不了。\n",
            "\n| 位号 | 值 | 元件实际在 | 板上那个位置印的却是 | 会不会真贴错 |",
            "|---|---|---|---|---|"]
    for a, c, fit in sorted(known, key=lambda s: (not s[2], s[0])):
        for r, other in ((a, c), (c, a)):
            p = parts.get(r)
            if not p:
                continue
            risk = ("🔴 **会**，两者尺寸相同，塞得进对方位置" if fit
                    else "🟢 不会，尺寸不同塞不进去")
            out.append(f"| **{r}** | {p['val']} | ({p['x']:.1f}, {p['y']:.1f}) | "
                       f"`{other}` | {risk} |")

swaps = find_silk_swaps(parts)
out += [f"\n> **当前源文件检测结果：{len(swaps)} 对互换**"
        f"{'（已全部修复 ✅）' if not swaps else ' ⚠️ 还有没修的！'}\n",
        "\n> 判据是「站在元件旁边，最近的那个位号文字是不是它自己的」。",
        "反过来按「文字找最近元件」会漏——被大芯片挡住的那几个就是这么漏掉的。\n"]

out += ["\n---\n",
        "\n## 开工前\n",
       "\n**先做完 `BOARD_TEST.md` 的阶段 1、2。** 裸板的电源对地绝缘和内层连通性",
       "一旦贴了片就测不了了——去耦电容和芯片会并联在电源地之间，读数不再是开路。\n",
       "\n### 顺序原则：先大后小，先中间后外围\n",
       "\n不管整板回流还是逐个热风吹，结论一样：\n",
       "\n- **整板回流**——大件对位要反复调整，周围先摆了 0402 会被镊子碰歪",
       "\n- **逐个热风**——吹外围时热风会掀翻已贴好的小件；大件热容大，反过来不怕\n",
       "\n所以下面每个阶段内部**按封装面积从大到小排**，照着表从上往下贴就行。\n",
       "\n### 几个容易吃亏的点\n",
       "\n- **BNO085 / BMP388 / CC1312R 这类塑封件怕潮**。拆封超过一周的，"
       "贴之前 125°C 烘 4 小时，不然回流时内部水汽汽化会把封装顶裂（爆米花效应），"
       "外观完全看不出来，表现是芯片直接不工作\n",
       "- **底部焊盘的件（★★★）烙铁够不着**，吹歪了只能拆下来重来，"
       "所以对位要在加热前反复确认，别指望后期补救\n",
       "- **0402 风量一定调小**，标称的风量吹 0402 会直接把件吹飞，"
       "而且飞走的件常常落在别的焊盘上造成短路\n",
       "- **pin1 方向以板上丝印为准**（缺角/圆点），下表的「旋转」只是参考。"
       "这块板的元件朝向前期出过错——曾经有 25 个两脚件的 pin1/pin2 方向是反的\n",
       "\n---\n"]

tot = 0
for code, name, sh, why, verify in STAGES:
    items = sorted([(r, p) for r, p in parts.items()
                    if p["sheet"] == sh and r not in skip],
                   key=lambda kv: -kv[1]["area"])
    if not items:
        continue
    tot += len(items)
    out.append(f"\n## 阶段 {code}：{name}（{len(items)} 件）\n")
    out.append(f"{why}\n")
    out.append("\n| 位号 | 型号 | 封装 | 板上位置 | 旋转 | 手工难度 |")
    out.append("|---|---|---|---|---|---|")
    for r, p in items:
        out.append(f"| **{r}** | {p['val']} | {p['fp'][:26]} | "
                   f"({p['x']:.1f}, {p['y']:.1f}) | {p['rot']:.0f}° | "
                   f"{difficulty(p['fp'], p['w'], p['h'])} |")
    out.append(f"\n**贴完验证**：{verify}\n")

out.append(f"\n## 不贴的 {len(skip)} 个位置\n")
out.append("\n| 位号 | 为什么不贴 |")
out.append("|---|---|")
for r in sorted(skip, key=lambda r: (re.sub(r"\d+$", "", r),
                                     int(re.search(r"\d+$", r).group() or 0))):
    out.append(f"| {r} | {skip[r]} |")

# 上面拼串时有的带尾部 \n、有的不带，直接 join 会到处出现双空行。统一剥掉再拼。
open(OUT, "w").write("\n".join(l.rstrip("\n") for l in out) + "\n")

# 机检：分阶段的件数 + 不贴的件数，必须等于板上元件总数，不能悄悄漏人
assert tot + len(skip) == len(parts), \
    f"对不上：分阶段 {tot} + 不贴 {len(skip)} ≠ 板上 {len(parts)}"
print(f"OK: {tot} 件分 {len(STAGES)} 阶段 / {len(skip)} 个位置不贴 → {OUT}")
if dirty_ref:
    print(f"\n⚠️ PCB 上有 {len(dirty_ref)} 个位号带首尾空格，请在 KiCad 里改掉：")
    for r in dirty_ref:
        print(f"      {r!r}  →  {r.strip()!r}")
    print("   丝印上看不出区别，但会让 SMT 的 BOM(来自网表) 和坐标(来自 PCB) 对不上。")
if swaps:
    print(f"\n⚠️ 有 {len(swaps)} 对位号丝印被印到了对方身上，详见 {os.path.basename(OUT)} 开头：")
    for a, c, fit in swaps:
        print(f"      {a} ↔ {c}   {'🔴 尺寸相同，会真贴错' if fit else '🟢 尺寸不同，贴不错'}")
for code, name, sh, *_ in STAGES:
    n = sum(1 for r, p in parts.items() if p["sheet"] == sh and r not in skip)
    print(f"    阶段 {code} {name:14s} {n:3d} 件")
