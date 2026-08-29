#!/bin/bash
# 导出嘉立创可直接下单的 Gerber + 钻孔 + 坐标 + BOM，打包成一个 zip。
#
# 嘉立创（JLCPCB）能直接识别 KiCad 的 Gerber 命名，不需要改名。要点：
#   · RS-274X，**不要**用 Protel 扩展名（--no-protel-ext）——6 层板用 Protel 那套
#     .gtl/.gbl 只有两面铜的位置，内层会命名冲突
#   · 钻孔用 Excellon，PTH/NPTH **分开两个文件**（--excellon-separate-th）：
#     合并的话安装孔（NPTH 2.7mm）会被当成金属化孔，孔壁镀铜、螺丝可能短路到地
#   · 钻孔原点用板子左下角绝对原点，不要用辅助原点——两边不一致会整板偏移
#   · 内层顺序靠文件名里的 In1..In4 表达，下单时叠层选「按文件名顺序」
#
# 叠层（下单时要在备注里写清楚，嘉立创默认可能给你排反）：
#   F.Cu(信号) / In1.Cu(GND) / In2.Cu(信号) / In3.Cu(3V3_DIG 电源) / In4.Cu(GND) / B.Cu(信号)
#
# 工艺要求（当前板实测值，都在标准档内）：
#   最小线宽 0.15mm  最小孔径 0.30mm  板厚 1.6mm  6 层
#   ⚠️ 最小孔径 0.30mm 是**特意压上去的**：嘉立创的孔径档位里只有 0.3mm 免费，
#      0.2mm 档要 104.65 元、还强制捆绑四线低阻全测(102.48)+指定板材(20.93)，
#      样板一单多花 228。全板过孔统一成 0.45mm 盘 / 0.3mm 孔（正好是免费档的
#      「外径0.4/0.45」规格），最小孔径 0.30mm 直接用免费档。
#      配套改动见 fanout_ring.py 的 PITCH_MIN/HOLE_GAP 和 .kicad_pro 的
#      min_hole_clearance（0.25→0.20，嘉立创 6 层的实际能力）。
#
# 用法：bash tools/gerber.sh
set -euo pipefail

T="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLI=~/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
PCB="$T/kicad/expansion-board-v3.kicad_pcb"
OUT="$T/fab"
G="$OUT/gerber"
rm -f "$OUT/expansion-board-v3-fab.zip"
mkdir -p "$G"

echo "════ 打板前硬门槛：DRC 必须干净 ════"
"$CLI" pcb drc --format json -o "$OUT/drc.json" "$PCB" > /dev/null
python3 - "$OUT/drc.json" <<'PY'
import json, sys, collections
d = json.load(open(sys.argv[1]))
u = len(d.get("unconnected_items", []))
cu = collections.Counter(v["type"] for v in d.get("violations", [])
                         if not v["type"].startswith("silk"))
silk = collections.Counter(v["type"] for v in d.get("violations", [])
                           if v["type"].startswith("silk"))
print(f"  未连通 {u} / 铜层违例 {dict(cu)} / 丝印 {dict(silk)}")
# 未连通和铜层违例是**致命**的：板子打出来就是断路或短路，几百块加两周全打水漂。
# 丝印只影响可读性，放行但要提醒。
assert u == 0, f"❌ 还有 {u} 处未连通，不能打板"
assert not cu, f"❌ 还有铜层 DRC 违例 {dict(cu)}，不能打板"
if silk:
    print(f"  ⚠️ 丝印有 {sum(silk.values())} 项重叠/压铜，不影响制板，但装配时位号可能看不清")
PY

echo
echo "════ Gerber（6 层 + 阻焊/丝印/助焊/板框）════"
"$CLI" pcb export gerbers -o "$G/" --no-protel-ext --subtract-soldermask \
    --layers F.Cu,In1.Cu,In2.Cu,In3.Cu,In4.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts \
    "$PCB" 2>&1 | grep -v "^$" | tail -3

echo
echo "════ 钻孔（Excellon，PTH/NPTH 分开）════"
"$CLI" pcb export drill -o "$G/" --format excellon --drill-origin absolute \
    --excellon-separate-th --generate-map --map-format gerberx2 "$PCB" 2>&1 | tail -2

echo
echo "════ 贴片坐标 + BOM ════"
"$CLI" pcb export pos -o "$OUT/positions.csv" --format csv --units mm --side both "$PCB" 2>&1 | tail -1
python3 "$T/tools/gen_bom.py" > /dev/null 2>&1 || true
[ -f "$T/BOM_PURCHASE.md" ] && cp "$T/BOM_PURCHASE.md" "$OUT/" || true

echo
echo "════ 打包 ════"
(cd "$G" && zip -q -r "$OUT/expansion-board-v3-fab.zip" .)
ls -1 "$G" | sed 's/^/  /'
echo
echo "✅ 交给嘉立创的文件: $OUT/expansion-board-v3-fab.zip"
echo "   贴片坐标: $OUT/positions.csv"
echo
echo "下单时必须手工确认的四项（默认值不一定对）:"
echo "  1) 层数 6 层，叠层顺序 F/In1(GND)/In2/In3(3V3)/In4(GND)/B"
echo "  2) 过孔工艺：常规即可（不需要 POFV 树脂塞孔）"
echo "  3) 最小孔径/外径 选「0.3mm(免费)(外径0.4/0.45)」——全板过孔统一 0.45盘/0.3孔"
echo "  4) 板厚 1.6mm，阻焊颜色随意，喷锡工艺选无铅"
