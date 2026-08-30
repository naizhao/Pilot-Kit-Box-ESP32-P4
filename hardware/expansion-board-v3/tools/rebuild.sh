#!/bin/bash
# 从固化的数据重建板子：布局(PLACEMENT.py) + 布线(ROUTES.json)。
#
# 跟 run_route.sh 的区别：
#   run_route.sh  从零**重新布线**（gen_pcb → 扇出 → freerouting → 收尾），跑 10 分钟，
#                 结果每次都不一样（freerouting 有随机性），手工修改一律丢失
#   rebuild.sh    只**复现**已固化的那份布线，秒级完成，结果逐字节确定
#
# 什么时候用哪个：
#   · 改了元件位置、想重新布线 → run_route.sh（记得先 export_routes.py 存一份当前的）
#   · 只想把板子恢复成固化的基线（比如 KiCad 里改坏了、或换台机器）→ rebuild.sh
#
# ⚠️ 手工在 KiCad 里改完布线后，**一定要跑 export_routes.py**，
#    否则下次 run_route.sh 一跑就没了——2026-08-12 手工做到未连通 0 的那批
#    修改（挪 3 个过孔 + 拉通 SUBG_VDDR/RP_1V1/SUBG_IRQ）差点就是这么丢的。
#
# 用法：bash tools/rebuild.sh
set -euo pipefail

T="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KP=/Users/samwu/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
CLI=/Users/samwu/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
BDIR="${PK_BOARD_DIR:-$T/kicad}"
B="${PK_BUILD_DIR:-$T/build}"
PCB="$BDIR/expansion-board-v3.kicad_pcb"
mkdir -p "$BDIR" "$B"

filt() { grep -vE "Debug:|stdpbase|wxApp|memory leak|ctor|GetWidth called" || true; }

[ -f "$T/tools/ROUTES.json" ] || { echo "没有 tools/ROUTES.json，先跑 export_routes.py"; exit 1; }

# 先备份当前板——万一它比固化的那份还新（有人在 KiCad 里改了没导出）
if [ -f "$PCB" ]; then
    cp "$PCB" "$B/pcb.before-rebuild.kicad_pcb"
    echo "当前板已备份 → build/pcb.before-rebuild.kicad_pcb"
fi

echo; echo "════════ ① 按 PLACEMENT.py 摆元件（从零新建）════════"
$KP "$T/tools/gen_pcb.py" 2>&1 | filt | tail -3

echo; echo "════════ ② 先重建 IFA 无铜走廊 ════════"
# route_ifa_feed 同时生成走线和规则区。必须放在快照导入之前：规则区会保留，
# 临时生成的走线会被 import_routes 全量清掉，最终布线只以 ROUTES.json 为准。
$KP "$T/tools/route_ifa_feed.py" 2>&1 | filt | grep -E '重布|馈电总路径'

echo; echo "════════ ③ 贴回 ROUTES.json 的唯一布线基线 ════════"
$KP "$T/tools/import_routes.py" 2>&1 | filt | grep -v "^⚠️"

echo; echo "════════ ③b 重放 V3.8 局部 ECO（QPL9547）════════"
$KP "$T/tools/route_eco_v38.py" 2>&1 | filt

echo; echo "════════ ④ 丝印（gen_pcb 从零建板不生成这些，必须补）════════"
# ⚠️ gen_pcb.py 是 pcbnew.NewBoard()，只放元件/覆铜/板框，**板级丝印不生成**。
# 2026-08-13 评审发现"背后的版权没有了"，根因就是这里：生成品牌丝印的工具
# 从来没进过流程，每跑一次布线，gr_text 就全丢（实测 gr_text=0 / fp_text=147）。
# 位号字号同理——PLACEMENT.py 只存位号**坐标**不存字号，
# 不补这一步就会退回 gen_pcb 的默认 1.0mm。
$KP "$T/tools/silk_texts.py" import 2>&1 | filt | head -2
$KP "$T/tools/silk_size.py" 0.8 2>&1 | filt | head -2
# 二极管阴极标记：官方 0402 封装只给了个半径 0.05mm 的圆点，实物上根本认不出
# 方向。gen_pcb 从封装库重建时那个圆点会回来、粗标记线不会，所以必须在这里补。
$KP "$T/tools/gen_polarity_marks.py" 2>&1 | filt | sed -n '1,8p'
PK_PRESERVE_SILK_REF=1 $KP "$T/tools/fix_silk_drc.py" 2>&1 | filt | head -12
$KP "$T/tools/fix_eco_silk_v38.py" 2>&1 | filt

echo; echo "════════ ⑤ 顶部全层禁铜带（板左/右缘→天线禁铜区，v4 同构）════════"
# gen_pcb 是 NewBoard 从零重建，文本插入的顶部 zone 每次都会丢——必须重放。
# 纯文本手术：pcbnew API 建多层 zone 后 Save 实测 Bus error（KiCad 8 SWIG）。
/Applications/ServBay/script/alias/python3 "$T/tools/top_keepout_v3.py" "$PCB"
$KP -c "import pcbnew;b=pcbnew.LoadBoard('$PCB');pcbnew.ZONE_FILLER(b).Fill(b.Zones());b.Save('$PCB')" 2>/dev/null
echo; echo "════════ ⑥ 验收 ════════"
$CLI pcb drc --format json -o "$B/drc-final.json" "$PCB" > /dev/null
$KP "$T/tools/check_route.py" "$B/drc-final.json" 2>&1 | filt
