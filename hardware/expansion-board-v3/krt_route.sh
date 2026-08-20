#!/bin/sh
# 整板布线。改完布局跑这一条。
#
#   sh krt_route.sh          完整流程
#   sh krt_route.sh --keep   跳过重建板子（只重跑布线）
#
# ⚠️ 在 KiCad 里手工调过布局的话，先跑 tools/export_placement.py 固化到 PLACEMENT.py，
#    否则第 ② 步重建板子会把你的调整覆盖掉。
#
# ── 为什么是这个顺序（每一步都是踩出来的）─────────────────────────────
# 射频**必须先布**：要留在 F.Cu（In1 是它的参考面）、不许打过孔、细间距焊盘处要
# neck-down。这些约束在空板上好办，数字线布完之后就无解——实测最后布只有 1/6 通，
# 先布 26/27 通。
#
# 但 freerouting 的 DSN 导入器**吃不下已布好的走线**，喂它预布射频就卡死在
# "Opening '...dsn'"。二分实测：裸板✅ / +锁 In1 +板边内缩✅ / +186 缝合过孔✅ /
# **+97 段预布射频 ❌卡死**。
#
# 所以中间加一道翻译：export_dsn.py 把射频走线逐段转成 Specctra keepout 禁布区，
# freerouting 不用"读懂"那些走线，只要知道"这块地方不许走"。
#
# 数字段为什么还是交给 freerouting：同一块板对比，freerouting 出 2+4 条铜层 DRC；
# KiCadRoutingTools 整板出 **95 条 clearance**，还把 In2 荒着不用、并偷偷改松
# .kicad_pro 的规则来迁就自己的输出。KRT 只用来布射频。
set -e
D=$(cd "$(dirname "$0")" && pwd)
PY=~/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
CLI=~/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
JAVA=/Applications/ServBay/package/openjdk/25/25.0.4/bin/java
FR=~/Applications/freerouting-2.2.4.jar
KRT=/tmp/krt
KPY=/tmp/krtenv/bin/python
PCB="$D/kicad/expansion-board-v3.kicad_pcb"
q() { grep -vE "Debug:|stdpbase|wxApp|memory leak|ctor found|GetWidth called"; }

# 关掉 KRT 的 per-net rescue（env_knobs.py:74）：它对布不通的网络会拿 100 万+ 次
# 迭代反复重试，**不受 --max-iterations 约束**（实测为一条网络耗掉一小时），
# 而且会把工艺档从 standard 抬到 advanced（0.25/0.15 过孔）来"救"——那要加钱，
# 不能让工具偷偷替我们决定。布不通的本质是布局问题，加迭代救不回来。
export KICAD_NET_RESCUE=0

# 射频网络清单从 gen_sch.RF50_NETS 现取，不手抄——手抄过一次就漏了两条。
RF=$($PY -c "import sys;sys.path.insert(0,'$D/tools');from gen_sch import RF50_NETS;print(' '.join(sorted(RF50_NETS)))" 2>/dev/null | tail -1)
[ -n "$RF" ] || { echo "取不到射频网络清单"; exit 1; }
RF_W=$(for n in $RF; do printf "0.34 "; done)

if [ "$1" != "--keep" ]; then
  echo "── ① 网表 ──"
  "$CLI" sch export netlist --format kicadxml -o /tmp/expansion.net.xml \
      "$D/kicad/expansion-board-v3.kicad_sch" >/dev/null 2>&1
  echo "── ② 摆位 + 覆铜 ──";  "$PY" "$D/tools/gen_pcb.py"  2>&1 | q | tail -1
  echo "── ③ 丝印 ──";        "$PY" "$D/tools/gen_silk.py" 2>&1 | q | grep 位号
  echo "── ④ 缝合过孔 ──";    "$PY" "$D/tools/route_rf.py" 2>&1 | q | grep -E "缝合|覆铜"
fi

mkdir -p "$KRT/kicad_files"
cp "$PCB" "$KRT/kicad_files/w.kicad_pcb"
cp "$D/kicad/expansion-board-v3.kicad_pro" "$KRT/kicad_files/w.kicad_pro"

# --power-nets 拿的是"粗线主干 + 细间距焊盘处自动 neck-down"这个行为：
# 射频线全程死宽 0.34mm 在 0.4mm pitch 封装旁边被相邻焊盘加间距夹死，
# 几何上不成立——不是算法不行。In1.Cu 不列进 --layers：那是参考面。
echo "── ⑤ 射频（空板、只走 F.Cu、0.34mm 主干 + neck-down）──"
(cd "$KRT" && "$KPY" -u route.py kicad_files/w.kicad_pcb kicad_files/w_rf.kicad_pcb \
    --keep-input-copper --layers F.Cu --clearance 0.15 --track-width 0.15 \
    --max-iterations 200000 --power-nets $RF --power-nets-widths $RF_W \
    --nets $RF) > /tmp/krt1.log 2>&1 || true
printf "  成功 %s / 失败 %s\n" "$(grep -c '  SUCCESS' /tmp/krt1.log)" "$(grep -c 'ROUTE FAILED' /tmp/krt1.log)"
cp "$KRT/kicad_files/w_rf.kicad_pcb" "$PCB"

echo "── ⑥ 导 DSN（射频转 keepout + 锁 In1 + 板边内缩）──"
"$PY" "$D/tools/export_dsn.py" 2>&1 | q

echo "── ⑦ freerouting 布数字段（约 10-15 分钟）──"
rm -f /tmp/exp.ses
FREEROUTING__PROFILE__EMAIL="${FREEROUTING_EMAIL:-you@example.com}" \
  "$JAVA" -jar "$FR" -de /tmp/exp.dsn -do /tmp/exp.ses -mp 40 >/tmp/fr.log 2>&1 || true
grep -E "session completed" /tmp/fr.log | tail -1 | sed 's/^/  /'

echo "── ⑧ 回灌 + 贴回射频 + 重灌覆铜 ──"
"$PY" "$D/tools/import_ses.py" 2>&1 | q

echo "── ⑨ DRC + 出图 ──"
"$CLI" pcb drc --schematic-parity -o /tmp/krt-drc.json --format json "$PCB" >/dev/null 2>&1
"$PY" "$D/tools/check_route.py" /tmp/krt-drc.json 2>&1 | q
sh "$D/tools/render.sh" >/dev/null 2>&1 && echo "  出图 → $D/render/"
