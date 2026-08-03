#!/bin/sh
# 一键重新布线。改完布局或想重布时跑这一条就行。
#
#   sh reroute.sh            完整流程（改过布局用这个）—— freerouting 一把梭，实测最优
#   sh reroute.sh --keep     跳过重建板子，只重跑布线（只想换个布线结果时用）
#
# ⚠️ 如果你在 KiCad 里手工调了布局，先跑 tools/export_placement.py 固化，
#    否则本脚本重建板子时会把你的调整覆盖掉。
set -e
D=$(cd "$(dirname "$0")" && pwd)
PY=~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
CLI=~/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
JAVA=/Applications/ServBay/package/openjdk/25/25.0.4/bin/java
FR=~/Applications/freerouting-2.2.4.jar
PCB="$D/kicad/expansion-board-v1.kicad_pcb"
q() { grep -vE "Debug:|stdpbase|wxApp|memory leak|ctor found"; }

if [ "$1" != "--keep" ]; then
  echo "── ① 从原理图重建网表 ──"
  "$CLI" sch export netlist --format kicadxml -o /tmp/expansion.net.xml \
      "$D/kicad/expansion-board-v1.kicad_sch" >/dev/null 2>&1
  echo "── ② 按 PLACEMENT.py 摆位 + 覆铜 + 电源岛 ──"
  "$PY" "$D/tools/gen_pcb.py" 2>&1 | q | tail -2
  echo "── ③ 丝印 ──"
  "$PY" "$D/tools/gen_silk.py" 2>&1 | q | grep 位号
fi

# 射频不预布。freerouting 的 DSN 导入器吃不下已布走线（会卡死在 Opening），
# 改由 export_dsn.py 给 RF50 类加 (circuit (use_layer F.Cu)) 让它自己布在顶层。
echo "── ④ GND 过孔墙 + 平面缝合过孔 ──"
"$PY" "$D/tools/route_rf.py" 2>&1 | q | grep -E "清理|缝合|覆铜"

echo "── ⑤ 导出 DSN（锁 In1 平面 + 板边内缩 + 射频限 F.Cu）──"
"$PY" "$D/tools/export_dsn.py" 2>&1 | q | tail -1

# freerouting 对**同一输入是确定性的**——实测同一块板连跑 3 次，结果都是 32 unrouted，
# 一字不差。我一度以为它有随机波动（见过 22/25/28/32），其实那些数字来自**不同的**
# 板子输入（改过布局），不是同一输入的抖动。所以默认只跑 1 次；
# 想对比多轮时用 FR_RUNS=3 sh reroute.sh。
FR_RUNS=${FR_RUNS:-1}
echo "── ⑥ freerouting 布数字段（跑 $FR_RUNS 次取最优，每次约 8-16 分钟）──"
BEST=99999
rm -f /tmp/exp.ses
for i in $(seq 1 "$FR_RUNS"); do
  rm -f /tmp/exp-$i.ses
  FREEROUTING__PROFILE__EMAIL=pilotbra@icloud.com \
    "$JAVA" -jar "$FR" -de /tmp/exp.dsn -do /tmp/exp-$i.ses -mp 40 >/tmp/fr-$i.log 2>&1 || true
  N=$(grep -oE "\(([0-9]+) unrouted\)" /tmp/fr-$i.log | tail -1 | tr -dc 0-9)
  [ -n "$N" ] || N=99999
  echo "     第 $i 次: $N unrouted"
  if [ -s /tmp/exp-$i.ses ] && [ "$N" -lt "$BEST" ]; then
    BEST=$N; cp /tmp/exp-$i.ses /tmp/exp.ses; cp /tmp/fr-$i.log /tmp/fr.log
  fi
done
[ -s /tmp/exp.ses ] || { echo "  N 次全部失败"; exit 1; }
echo "     采用最优: $BEST unrouted"

echo "── ⑦ 回灌布线 + 重灌覆铜 ──"
cp /tmp/exp.ses /tmp/expansion.ses
"$PY" "$D/tools/import_ses.py" 2>&1 | q | grep -E "导入后|重灌"

echo "── ⑧ DRC + 出图 ──"
"$CLI" pcb drc --schematic-parity -o /tmp/reroute-drc.json --format json "$PCB" >/dev/null 2>&1
python3 - <<'EOF'
import json, collections
d = json.load(open("/tmp/reroute-drc.json"))
c = collections.Counter(v["type"] for v in d["violations"])
print("  铜层 DRC:", {k: v for k, v in c.items() if not k.startswith("silk")} or "0 违例")
print("  丝印 DRC:", {k: v for k, v in c.items() if k.startswith("silk")} or "0 违例")
u = d["unconnected_items"]
print(f"  未连接: {len(u)} 处焊盘 ≈ {len(u)//2} 条")
EOF
sh "$D/tools/render.sh" >/dev/null 2>&1
echo "  出图 → $D/render/"
echo "完成。"
