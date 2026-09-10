#!/bin/bash
# 导出 VNA 校准板的 Gerber + 钻孔，打包成可直接下单的 zip。
#
# 2 层板比 6 层简单得多，但有两处仍然照 6 层的规矩来：
#   · 钻孔 PTH/NPTH **分开两个文件**（--excellon-separate-th）
#     本板虽然没有 NPTH，但分开导出不会错，合并才可能出事
#   · 钻孔原点用板子左下角**绝对原点**，不用辅助原点——两边不一致会整板偏移
#
# 与 6 层板不同的是：2 层板用 Protel 扩展名（.gtl/.gbl）不会冲突，
# 但这里仍统一用 KiCad 默认的 .gbr，跟 v3/v4 的输出保持一致口径，
# 免得两套命名混用时又要去核对层序。
#
# 用法：bash tools/gerber.sh
set -euo pipefail

T="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLI=~/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
PCB="$T/kicad/cal-board.kicad_pcb"
OUT="$T/release"
STAMP="${PK_RELEASE_STAMP:-$(/bin/date +%Y%m%d)}"
ZIP="$OUT/cal-board-gerber-$STAMP.zip"

[ -f "$PCB" ] || { echo "❌ 找不到 $PCB，先跑 tools/gen_cal_board.py"; exit 1; }
[ ! -e "$ZIP" ] || { echo "不覆盖已有发布包: $ZIP"; exit 1; }
mkdir -p "$OUT"
WORK="$(/usr/bin/mktemp -d "$T/build-$STAMP.XXXXXX")"
G="$WORK/gerber"
mkdir -p "$G"

echo "════ 打板前门槛：铜层 DRC 必须干净 ════"
"$CLI" pcb drc --refill-zones --format json -o "$WORK/drc.json" "$PCB" > /dev/null
/Applications/ServBay/bin/python3 - "$WORK/drc.json" <<'PY'
import json, sys, collections
d = json.load(open(sys.argv[1]))
u = len(d.get("unconnected_items", []))
cu = collections.Counter(v["type"] for v in d.get("violations", [])
                         if not v["type"].startswith("silk"))
silk = collections.Counter(v["type"] for v in d.get("violations", [])
                           if v["type"].startswith("silk"))
print(f"  未连通 {u} / 铜层违例 {dict(cu)} / 丝印 {dict(silk)}")
# 未连通和铜层违例是致命的（断路或短路）；丝印只影响可读性
assert u == 0, f"❌ 还有 {u} 处未连通，不能打板"
assert not cu, f"❌ 还有铜层 DRC 违例 {dict(cu)}，不能打板"
if silk:
    print(f"  ⚠️ 丝印 {sum(silk.values())} 项轻微重叠，不影响制板")
PY

echo
echo "════ Gerber（2 层 + 阻焊/丝印/助焊/板框）════"
"$CLI" pcb export gerbers -o "$G/" --no-protel-ext --no-x2 --no-netlist \
    --subtract-soldermask \
    --layers F.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts \
    "$PCB" 2>&1 | tail -2

echo
echo "════ 钻孔（Excellon，PTH/NPTH 分开）════"
"$CLI" pcb export drill -o "$G/" --format excellon --drill-origin absolute \
    --excellon-separate-th "$PCB" 2>&1 | tail -2

echo
echo "════ 打包 ════"
/Applications/ServBay/bin/python3 - "$G" "$ZIP" <<'PY'
from pathlib import Path
import sys, zipfile
gdir, zpath = Path(sys.argv[1]), Path(sys.argv[2])
import re
cand = sorted(p for p in gdir.iterdir()
              if p.is_file() and not p.name.endswith(".gbrjob")
              and "drl_map" not in p.name)

def is_empty(p):
    """没有任何绘图/钻孔指令的层。本板全部元件在顶层，所以 B_Paste 必然是空的；
    又没有安装孔，NPTH 也是空的。**空的钻孔文件要剔掉**——部分板厂的 CAM
    读到无刀具定义的 .drl 会直接报错。空的 Paste 层无害但一并省掉，保持干净。"""
    t = p.read_text(errors="ignore")
    if p.suffix == ".drl":
        return not re.search(r"^T\d+C[\d.]+", t, re.M)
    return not (re.search(r"^[XY].*D0[123]\*", t, re.M) or "G36*" in t)

files = [p for p in cand if not is_empty(p)]
skipped = [p.name for p in cand if is_empty(p)]
if skipped:
    print(f"  跳过空层: {', '.join(skipped)}")
assert len(files) >= 8, f"文件数不对({len(files)}): {[f.name for f in files]}"
assert any(f.name.endswith("PTH.drl") for f in files), "没有钻孔文件"
assert any("F_Cu" in f.name for f in files) and any("Edge_Cuts" in f.name for f in files)
with zipfile.ZipFile(zpath, "x", zipfile.ZIP_DEFLATED) as z:
    for p in files:
        z.write(p, p.name)
print(f"  打包 {len(files)} 个文件")
PY
/bin/ls -1 "$G" | /usr/bin/sed 's/^/  /'
echo
echo "✅ 可直接下单: $ZIP"
echo
echo "下单参数（华秋 2 层免费打样档）:"
echo "  层数 2 层 · 板厚 1.6mm · 外层铜厚 1oz"
echo "  尺寸 95 × 92mm（10×10cm 以内）· 绿油白字"
echo "  最小线宽 1.20mm · 最小孔径 0.40mm  ← 工艺余量极大，任何免费档都能做"
echo "  表面处理：喷锡即可。本板没有细间距器件（最小 0402），不必沉金"
