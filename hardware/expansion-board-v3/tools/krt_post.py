#!/usr/bin/env python3
"""KRT 布线的前置分析与后置收尾——它自己不做这些，不补就出不了能用的板。

## 为什么需要这个脚本

KRT（KiCadRoutingTools，装在 KiCad 插件目录）布线本体很好——octilinear、Rust A*、
17 秒布完，而且直接读写 .kicad_pcb，绕开了 freerouting 吐不出 SES 那个死结
（见 memory: project_freerouting_45deg_postroute_blocks_ses）。

但它交出来的板子不能直接用，2026-08-22 一次跑完踩了四个坑：

1. **不重新填充覆铜**。新走线/过孔穿过旧的填充铜，DRC 报 clearance 505 +
   hole_clearance 200——看着像布线质量差，其实全是 Track/Via↔Zone。填一次
   之后 505→33、200→0，而 Track↔Track 自始至终只有 4 条。
   **判据：先看违例的参与者类型，带 Zone 的先去填铜再下结论。**

2. **reconciliation 阶段不看 --nets 过滤**。它的 kicad-oracle reconnect 照着
   KiCad DRC 的未连通表干活，把 7 条射频布到了 B.Cu/In2.Cu 上、打了 18 个过孔
   ——那是在 50Ω 线的参考面上开洞。必须事后清掉。

3. 清完射频会留下断头铜，删断头铜又露出新的断头铜，要**迭代到收敛**。

4. 它会放不合规的小过孔（实测 0.25/0.15，板规要求直径 ≥0.3、环宽 ≥0.07）。
   改 0.45 会撞邻居；0.30/0.15 正好：直径等于下限、环宽 0.075>0.07，两项都过。

## 平面网名单必须从板上读，不能写死

第一轮我按印象写死 `!GND !3V3_DIG !3V3_RF !3V3_GNSS !RP_1V1` 排除"平面网"，
结果 3V3_GNSS **根本没有覆铜平面**（板上可缝合网络只有 GND/3V3_DIG/3V3_RF/RP_1V1），
它需要走线连接，却被我当平面网排除了——55 条真缺线里它一家独占 14 条。
所以 `nets` 模式直接从板上的覆铜区读，板子怎么变名单就怎么变。

用法：
    krt_post.py nets     打印该排除的网络（喂给 route.py 的 --nets）
    krt_post.py power    打印 POWER/POWER_LO 网络与线宽（--power-nets）
    krt_post.py finish   对当前板做全套收尾（填铜→清射频越层→清断头→修过孔）
"""
import collections
import json
import os
import subprocess
import sys

import pcbnew

T = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(T, "tools"))
BDIR = os.environ.get("PK_BOARD_DIR") or os.path.join(T, "kicad")
PCB = os.path.join(BDIR, "expansion-board-v3.kicad_pcb")
PRO = os.path.join(BDIR, "expansion-board-v3.kicad_pro")

from gen_sch import RF50_NETS                                    # noqa: E402

VIA_MIN_D, VIA_MIN_DRILL = 0.30, 0.15    # 板规下限：直径 0.3 / 环宽 0.07
MODE = sys.argv[1] if len(sys.argv) > 1 else "finish"


def plane_nets(board):
    """板上**真的有覆铜区**的网络。射频不算（它有自己的走廊）。"""
    out = set()
    for z in board.Zones():
        if z.GetIsRuleArea():
            continue
        n = z.GetNetname()
        if n and n not in RF50_NETS:
            out.add(n)
    return out


if MODE == "nets":
    b = pcbnew.LoadBoard(PCB)
    # 排除两类：有覆铜平面的（靠铜面+缝合过孔连接，不该拉走线）、已布好的射频
    ex = sorted(plane_nets(b)) + sorted(RF50_NETS)
    print(" ".join(['"*"'] + [f'"!{n}"' for n in ex]))
    sys.exit(0)

if MODE == "power":
    d = json.load(open(PRO))
    cls = {c["name"]: c for c in d["net_settings"]["classes"]}
    pat = collections.defaultdict(list)
    for p in d["net_settings"].get("netclass_patterns", []):
        pat[p["netclass"]].append(p["pattern"])
    b = pcbnew.LoadBoard(PCB)
    skip = plane_nets(b) | set(RF50_NETS)
    names, widths = [], []
    for cname in ("POWER", "POWER_LO"):
        w = cls.get(cname, {}).get("track_width")
        for n in pat.get(cname, []):
            if n in skip:            # 有平面的不用拉线，射频已布
                continue
            names.append(n)
            widths.append(str(w))
    print(" ".join(f'"{n}"' for n in names) + "|" + " ".join(widths))
    sys.exit(0)

# ── finish：全套收尾 ────────────────────────────────────────────────────
CLI = os.path.expanduser("~/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli")
BUILD = os.path.join(T, "build")
DRC = os.path.join(BUILD, "drc-krt.json")


def refill():
    """覆铜重填必须在子进程做：本进程若调用过 board.Remove()，SWIG 容器已失效
    （board.Zones() 直接 SIGSEGV，重新 LoadBoard 也救不回来）。"""
    subprocess.run([sys.executable, "-c",
                    "import pcbnew;b=pcbnew.LoadBoard(%r);"
                    "pcbnew.ZONE_FILLER(b).Fill(b.Zones());b.Save(%r)" % (PCB, PCB)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def drc():
    subprocess.run([CLI, "pcb", "drc", "--format", "json", "-o", DRC, PCB],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    d = json.load(open(DRC))
    return d, collections.Counter(v["type"] for v in d.get("violations", []))


# ① 射频越层：清掉不在 F.Cu 的射频走线和所有射频过孔
b = pcbnew.LoadBoard(PCB)
kill = []
for t in b.GetTracks():
    if t.GetNetname() not in RF50_NETS:
        continue
    if t.Type() == pcbnew.PCB_VIA_T:
        kill.append(t)
    elif b.GetLayerName(t.GetLayer()) != "F.Cu":
        kill.append(t)
n_rf = len(kill)
for t in kill:
    b.Remove(t)
if n_rf:
    b.Save(PCB)
print(f"① 清射频越层: {n_rf} 项（走线离开 F.Cu 或射频打过孔 = 在 In1 参考面上开洞）")
del b

# ② 不合规过孔：直径 < 0.3 或环宽 < 0.07 的，改成板规下限
b = pcbnew.LoadBoard(PCB)
n_via = 0
for t in b.GetTracks():
    if t.Type() != pcbnew.PCB_VIA_T:
        continue
    d_mm, k_mm = pcbnew.ToMM(t.GetWidth()), pcbnew.ToMM(t.GetDrill())
    if d_mm < VIA_MIN_D - 1e-6 or (d_mm - k_mm) / 2 < 0.07 - 1e-6:
        t.SetWidth(pcbnew.FromMM(VIA_MIN_D))
        t.SetDrill(pcbnew.FromMM(VIA_MIN_DRILL))
        n_via += 1
if n_via:
    b.Save(PCB)
print(f"② 修不合规过孔: {n_via} 个 → {VIA_MIN_D}/{VIA_MIN_DRILL}"
      f"（改 0.45 会撞邻居，0.30 正好压线合规）")
del b

# ③ 覆铜重填 —— 必须在清理之后、判 DRC 之前
refill()
d, c = drc()
print(f"③ 覆铜重填后: {dict(c)}")

# ④ 断头铜迭代清理：删过孔会露出新的断头走线，一轮清不干净
prev = None
for i in range(1, 7):
    n = sum(c.get(k, 0) for k in ("track_dangling", "via_dangling"))
    if n == 0 or n == prev:
        break
    prev = n
    subprocess.run([sys.executable, os.path.join(T, "tools", "clean_dangling.py"), DRC, "apply"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    refill()
    d, c = drc()
    print(f"④.{i} 清断头铜 → 剩余 dangling "
          f"{sum(c.get(k, 0) for k in ('track_dangling', 'via_dangling'))}")

u = d.get("unconnected_items", [])
print(f"\n收尾完成：未连通 {len(u)}  铜层 DRC {dict(c)}")
