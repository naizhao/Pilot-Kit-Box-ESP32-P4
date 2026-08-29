#!/bin/bash
# 从 PLACEMENT.py 跑完整条布线链，每步的输出都落在 build/ 下可核对。
#
# 为什么要有这个脚本：这条链有 8 步，中间任何一步的产物都影响后面。
# 之前靠手敲命令，跑到第 5 步才发现第 2 步用的是上一版板子的情况出现过不止一次。
#
# ⚠️ 跑之前请关掉 KiCad——脚本会直接改写 kicad/ 下的板子。
# 当前板子会先备份到 build/pcb.before-run.kicad_pcb。
#
# 用法：bash tools/run_route.sh
set -euo pipefail

if [ "${PK_ALLOW_REROUTE:-}" != "1" ]; then
    echo "❌ run_route.sh 会从零重布并覆盖当前固化布线。"
    echo "   确认确实要重新布线后，用 PK_ALLOW_REROUTE=1 /bin/bash tools/run_route.sh"
    exit 1
fi

T="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KP=~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
CLI=~/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
FR=~/Applications/freerouting-2.2.4.jar
# ⚠️ freerouting 2.2.4 是 Java 25 编的（class file 69），PATH 上的 java 是 21，
# 直接跑会 UnsupportedClassVersionError。/usr/libexec/java_home 也找不到 25——
# 它只认 /Library/Java 下的，而本机唯一的 25 在 ServBay 里，所以写死路径。
JAVA=/Applications/ServBay/package/openjdk/25/current/bin/java
[ -x "$JAVA" ] || { echo "找不到 Java 25: $JAVA"; exit 1; }
B="$T/build"
PCB="$T/kicad/expansion-board-v3.kicad_pcb"
mkdir -p "$B"

# pcbnew 在无 GUI 下会往 stderr 吐一堆 wxApp/Debug 噪音，滤掉才看得见真正的输出
filt() { grep -vE "Debug:|stdpbase|wxApp|memory leak|ctor|GetWidth called" || true; }

step() { echo; echo "════════ $* ════════"; }

# ⚠️ 这条链的第①步是 gen_pcb 的 pcbnew.NewBoard()——**从零重建，现有布线全部丢弃**，
# 包括在 KiCad 里手工挪的过孔和手工拉的线。所以先把当前布线导出成快照，
# 万一跑出来的结果不如现在，可以用 tools/rebuild.sh 原样恢复。
cp "$PCB" "$B/pcb.before-run.kicad_pcb"
$KP "$T/tools/export_routes.py" "$PCB" 2>&1 | filt | head -2
cp "$T/tools/ROUTES.json" "$B/routes.before-run.json"
echo "（布线快照已存：build/routes.before-run.json）"

step "① 按 PLACEMENT.py 生成板子"
$KP "$T/tools/gen_pcb.py" 2>&1 | filt | tail -6

step "② QFN 规整扇出环（先放孔，再让走线避让）"
# 顺序问题，2026-08-12 评审指出：freerouting 是**边布线边打孔**，先布的线把后面
# 的孔位堵死了——U8.23/U10.45 出不去就是这么来的。正确顺序是先按规则把扇出孔
# 放好，走线自然绕着它们走。
# 试过的替代方案（真缺线）：不加 2 / via_keepout 实心矩形 7 / 口字环 3，
# 都只是"禁止孔打在坏位置"，没改顺序，所以治标不治本。
# PK_FANOUT=0 可跳过这步做对照。
if [ "${PK_FANOUT:-1}" = "1" ]; then
    $KP "$T/tools/fanout_ring.py" apply 2>&1 | filt | tail -8
else
    echo "（PK_FANOUT=0，跳过）"
fi

step "②' 射频先布 + GND 缝合过孔（避开已放的扇出孔）"
$KP "$T/tools/route_rf.py" 2>&1 | filt | tail -8

step "②\" IFA 馈电脚 → π 匹配首级（route_rf 的 pad-to-pad 布不了这条）"
# 天线 pad1 是一整块自定义焊盘，锚点在主臂上，route_rf.py 的 _link 用
# pad.GetPosition() 会从主臂中间拉线横穿净空区。这条必须单独按馈电脚末端布。
# 位置在 export_dsn 之前 —— 射频走线要转成 keepout 交给 freerouting 避开。
$KP "$T/tools/route_ifa_feed.py" 2>&1 | filt | tail -3

step "③ 导 DSN（射频转 keepout + EP 内层禁布区）"
$KP "$T/tools/export_dsn.py" ${PK_DSN_MODE:-} 2>&1 | filt | tail -20

step "④ freerouting"
# ⚠️ -mp 是 max passes，**0 表示不限轮次**，不是"不做优化轮次"。
# 原来这里写 -mp 0 并注释成"只求先全布通"，理解反了。之前一直没暴露，是因为
# 板上有非 45° 走线，freerouting 的 45 度化后处理在第一轮就把自己卡死了，
# 看着像"跑完一轮就停"。2026-08-22 把预布走线全改成 45° 之后真相才露出来：
#     pass #1  5m34s  73 unrouted
#     pass #2  3m43s  47 unrouted
#     pass #3  2m47s  35 unrouted     ← 一直优化下去，永远不写 SES
# 它只在轮次跑满之后才输出 SES，被看门狗 kill 掉就什么都没有。
# 取 3 轮：从上面的实测看收益在快速衰减，而每轮都在变慢的反面——总时长约 12 分钟。
FR_PASSES="${PK_FR_PASSES:-3}"
# 看门狗仍然保留。45° 死循环这条路虽然堵上了，但 freerouting 还有别的卡死方式
# （memory: PolylineTrace.combine 无限递归，加 -Xss 没用）。macOS 没有 timeout(1)。
rm -f "$B/exp.ses"
"$JAVA" -jar "$FR" -de "$B/exp.dsn" -do "$B/exp.ses" -mp "$FR_PASSES" \
    > "$B/freerouting.log" 2>&1 &
FR_PID=$!
( sleep "${PK_FR_TIMEOUT:-1500}"; kill "$FR_PID" 2>/dev/null ) & WD_PID=$!
wait "$FR_PID" 2>/dev/null || true
kill "$WD_PID" 2>/dev/null || true
grep -E "unrouted|Routing.*completed|items|not 45 degree" "$B/freerouting.log" | tail -4
if [ ! -s "$B/exp.ses" ]; then
    echo "❌ freerouting 未产出 SES（可能卡在 45° 后处理）。"
    echo "   板子仍是第②\" 步的状态（射频已布、数字段未布），备份在 $B/pcb.before-run.kicad_pcb"
    echo "   freerouting 有随机性，直接重跑本脚本往往就过；连续失败再查 build/freerouting.log"
    exit 1
fi

step "⑤ 把 SES 合并回板子（射频走线从快照恢复）"
$KP "$T/tools/import_ses.py" 2>&1 | filt | tail -8

step "⑥ 收尾布线（route_fix）"
$CLI pcb drc --format json -o "$B/drc.json" "$PCB" > /dev/null
$KP "$T/tools/route_fix.py" apply "$B/drc.json" 2>&1 | filt | tail -10

step "⑦ EP 散热过孔"
$KP "$T/tools/thermal_vias.py" apply 2>&1 | filt | tail -12

step "⑦' 丝印（gen_pcb 从零建板不生成，必须补）"
# 见 rebuild.sh 同一步的说明：板级品牌丝印和位号字号都不在 PLACEMENT.py 里，
# 不补这两步，每跑一次流程就丢一次。
$KP "$T/tools/silk_texts.py" import 2>&1 | filt | head -2
$KP "$T/tools/silk_size.py" 0.8 2>&1 | filt | head -2

step "⑧ 验收"
$CLI pcb drc --format json -o "$B/drc-final.json" "$PCB" > /dev/null
$KP "$T/tools/check_route.py" "$B/drc-final.json" 2>&1 | filt

echo
echo "备份: $B/pcb.before-run.kicad_pcb"
echo "对比: $KP $T/tools/diff_layout.py $B/pcb.before-run.kicad_pcb"
