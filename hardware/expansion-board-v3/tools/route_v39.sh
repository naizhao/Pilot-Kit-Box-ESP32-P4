#!/bin/bash
# V3.9 全板重布线。与 run_route.sh 的唯一区别：**不跑 gen_pcb.py**。
#
# run_route.sh 的第①步是 `gen_pcb.py` 的 `pcbnew.NewBoard()`——从空板重建，
# 板级丝印和 4 个命名 RF zone 都不在 PLACEMENT.py / ROUTES.json 里，跑一次丢一次
# （RG-V3-01/02 就是这么来的；⑦' 只能补回丝印，补不回 zone）。
# V3.9 候选板是从 V3.8 母版长出来的，那 29 条 gr_text 和 11 个 zone 必须原样保住，
# 所以这条链从②开始，把板子当既有事实，只做布线。
#
# 布线前先把残留走线**全部拆掉**。V3.9 候选板上那 1530 段是 V3.8 母版布线被拆掉
# 176 段之后的残骸——半通不通，人接不上，freerouting 也会被它们约束出更差的解。
# 元件、覆铜、丝印、板框一概不动，只清 segment 和 via。
#
# 用法：bash tools/route_v39.sh
#   PK_FR_PASSES=3     freerouting 优化轮次
#   PK_FR_TIMEOUT=1500 看门狗秒数
set -euo pipefail

T="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KP=~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
CLI=~/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
JAVA=/Applications/ServBay/package/openjdk/25/current/bin/java
[ -x "$JAVA" ] || { echo "找不到 Java 25: $JAVA"; exit 1; }

B="$T/build"
PCB="$T/kicad/expansion-board-v3.kicad_pcb"
mkdir -p "$B"
FR="${PK_FREEROUTING_JAR:-$B/tools/freerouting-2.3.0.jar}"
FR_SHA256="3cf18d608437740bc497db6b8ef5888e2e60a08de0def20691d1bad0c0e0ee24"
[ -f "$FR" ] || { echo "找不到 Freerouting 2.3.0: $FR"; exit 1; }
[ "$(/usr/bin/shasum -a 256 "$FR" | /usr/bin/awk '{print $1}')" = "$FR_SHA256" ] \
    || { echo "Freerouting JAR SHA-256 不匹配"; exit 1; }

filt() { grep -vE "Debug:|stdpbase|wxApp|memory leak|ctor|GetWidth called" || true; }
step() { echo; echo "════════ $* ════════"; }

cp "$PCB" "$B/pcb.before-v39route.kicad_pcb"
echo "布线前板子已备份 → build/pcb.before-v39route.kicad_pcb"

step "① 清空残留布线（元件/覆铜/丝印/板框不动）"
$KP - "$PCB" <<'PY' 2>&1 | filt
import sys, pcbnew
pcb = sys.argv[1]
board = pcbnew.LoadBoard(pcb)
tracks = [t for t in board.GetTracks() if t.Type() == pcbnew.PCB_TRACE_T]
vias = [t for t in board.GetTracks() if t.Type() == pcbnew.PCB_VIA_T]
for item in list(board.GetTracks()):
    board.Remove(item)
board.BuildConnectivity()
board.Save(pcb)
print(f"  拆除 走线 {len(tracks)} 段 / 过孔 {len(vias)} 个")
print(f"  保留 封装 {len(board.GetFootprints())} / 覆铜 {len(list(board.Zones()))}")
PY

step "② QFN 规整扇出环（先放孔，再让走线避让）"
# freerouting 是边布线边打孔的，先布的线会把后面的孔位堵死。先按规则把扇出孔
# 放好，走线自然绕着走——这是 2026-08-12 评审定下的顺序。
if [ "${PK_FANOUT:-1}" = "1" ]; then
    $KP "$T/tools/fanout_ring.py" apply 2>&1 | filt | tail -8
else
    echo "（PK_FANOUT=0，跳过）"
fi

step "②' 射频先布 + GND 缝合过孔"
$KP "$T/tools/route_rf.py" 2>&1 | filt | tail -8

step "②\" IFA 馈电脚 → π 匹配首级"
$KP "$T/tools/route_ifa_feed.py" 2>&1 | filt | tail -3

step "③ 导 DSN（射频转 keepout + EP 内层禁布区）"
# 射频走线在这一步转成 keepout 交给 freerouting 避开——DSN 吃不下已布走线，
# 直接喂进去它会崩在 PolylineTrace.combine 的无限递归上。
$KP "$T/tools/export_dsn.py" ${PK_DSN_MODE:-} 2>&1 | filt | tail -20

step "④ freerouting"
# -mp 是 max passes，0 表示**不限轮次**——它只在轮次跑满后才写 SES，
# 设 0 会一直优化下去、永远不输出，被看门狗 kill 就什么都没有。取 3 轮。
FR_PASSES="${PK_FR_PASSES:-3}"
FR_THREADS="${PK_FR_THREADS:-1}"
if [ -s "$B/exp.ses" ]; then
    SES_BACKUP="$B/exp.before-v39.$(/bin/date +%Y%m%d-%H%M%S).ses"
    /bin/mv "$B/exp.ses" "$SES_BACKUP"
    echo "上一轮 SES 已保留: $SES_BACKUP"
fi
"$JAVA" -jar "$FR" --gui.enabled=false -de "$B/exp.dsn" -do "$B/exp.ses" \
    -mp "$FR_PASSES" -mt "$FR_THREADS" \
    > "$B/freerouting.log" 2>&1 &
FR_PID=$!
( sleep "${PK_FR_TIMEOUT:-1500}"; kill "$FR_PID" 2>/dev/null ) & WD_PID=$!
wait "$FR_PID" 2>/dev/null || true
kill "$WD_PID" 2>/dev/null || true
grep -E "unrouted|Routing.*completed|items|not 45 degree" "$B/freerouting.log" | tail -4
if [ ! -s "$B/exp.ses" ]; then
    echo "❌ freerouting 未产出 SES。板子停在第②\" 步（射频已布、数字段未布）。"
    echo "   备份在 $B/pcb.before-v39route.kicad_pcb"
    echo "   它有随机性，直接重跑往往就过；连续失败查 build/freerouting.log"
    exit 1
fi

step "⑤ 把 SES 合并回板子"
$KP "$T/tools/import_ses.py" 2>&1 | filt | tail -8

step "⑥ 收尾布线（route_fix）"
$CLI pcb drc --format json -o "$B/drc.json" "$PCB" > /dev/null
$KP "$T/tools/route_fix.py" apply "$B/drc.json" 2>&1 | filt | tail -10

step "⑦ EP 散热过孔"
$KP "$T/tools/thermal_vias.py" apply 2>&1 | filt | tail -12

step "⑧ 验收"
$CLI pcb drc --refill-zones --format json -o "$B/drc-final.json" "$PCB" > /dev/null
$KP "$T/tools/check_route.py" "$B/drc-final.json" 2>&1 | filt
echo
echo "── 丝印与 zone 是否原样保住（这条链存在的理由）──"
/usr/bin/python3 - "$PCB" <<'PY'
import re, sys
text = open(sys.argv[1], encoding="utf-8").read()
zones = re.findall(r'\(name "([^"]+)"\)', text)
rf = [z for z in zones if z.startswith(("ifa_rf50_corridor", "ANT1090_"))]
print(f"  gr_text {len(re.findall(chr(10)+chr(9)+r'\(gr_text ', text))} 条（应 29）")
print(f"  命名 zone {len(zones)} 个（应 11），其中 RF zone {len(rf)} 个（应 4）: {rf}")
PY
echo
echo "备份: $B/pcb.before-v39route.kicad_pcb"
