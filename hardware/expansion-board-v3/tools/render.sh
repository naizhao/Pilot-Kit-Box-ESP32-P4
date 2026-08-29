#!/bin/sh
# 布局/布线后出图复核。DRC 只能证明"不违规"，证明不了"不离谱"——必须用眼睛看一眼。
# 用法: sh tools/render.sh [输出目录]
set -e
CLI=~/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
D=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$D/render}
mkdir -p "$OUT"
PCB="$D/kicad/expansion-board-v3.kicad_pcb"

# 顶视图 3D（看元件有没有摆歪/压边/撞在一起）
"$CLI" pcb render -o "$OUT/top.png" --side top --zoom 0.85 -w 2400 -h 1500 "$PCB"
"$CLI" pcb render -o "$OUT/bottom.png" --side bottom --zoom 0.85 -w 2400 -h 1500 "$PCB"
# 分层 SVG（看铜箔/丝印/天线，3D 渲染看不出走线）
"$CLI" pcb export svg -o "$OUT/fcu.svg" --layers F.Cu,Edge.Cuts --page-size-mode 2 "$PCB"
"$CLI" pcb export svg -o "$OUT/bcu.svg" --layers B.Cu,Edge.Cuts --page-size-mode 2 "$PCB"
"$CLI" pcb export svg -o "$OUT/silk.svg" --layers F.SilkS,Edge.Cuts --page-size-mode 2 "$PCB"
# 装配图：密板做不到 100% 丝印覆盖，找件靠这张（F.Fab 有全部 156 个位号）
"$CLI" pcb export pdf -o "$OUT/assembly-top.pdf" --layers F.Fab,F.SilkS,Edge.Cuts \
    --mode-single --include-border-title "$PCB"
"$CLI" pcb export pdf -o "$OUT/assembly-bottom.pdf" --layers B.Fab,B.SilkS,Edge.Cuts \
    --mode-single --include-border-title "$PCB"
echo "出图完成 → $OUT（含装配图 assembly-*.pdf）"
