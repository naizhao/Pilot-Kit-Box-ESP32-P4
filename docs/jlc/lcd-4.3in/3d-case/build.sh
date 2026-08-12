#!/usr/bin/env bash
# 4.3" 盒体 STL/OBJ 构建流程
# 用法：./build.sh
#
# 三步：
#   1. OpenSCAD CLI 从 scad 源文件导出 STL（ASCII）
#   2. admesh 修复法线方向与法线值（JLC 同款校验引擎）
#      —— OpenSCAD CGAL 导出 STL 时，共面区域的预计算 normal 偏差，
#         会被 JLC 切片器报「反向三角面」。admesh --normal-directions
#         --normal-values 把它们修正。每次重导出 STL 都必须跑这一步，
#         否则「反向三角面」会复发（实测凸字/凹字版都是 93 个 normal 错误）。
#   3. stl2obj.py 转 OBJ（JLC 下单只收 STEP/OBJ）
set -euo pipefail

SCAD="/Applications/OpenSCAD-2021.01.app/Contents/MacOS/OpenSCAD"
DIR="$(cd "$(dirname "$0")" && pwd)"
SCAD_FILE="$DIR/pilot-kit-box-43-base.scad"
STL="$DIR/pilot-kit-box-43-base_v2.stl"
OBJ="$DIR/pilot-kit-box-43-base_v2.obj"

echo "==> [1/3] OpenSCAD 导出 STL（约 2 分钟）…"
"$SCAD" -o "$STL" "$SCAD_FILE" 2>&1 | grep -E "WARNING|ERROR|Simple:|Volumes:" || true
WARN=$(grep -cE "WARNING|ERROR" "$SCAD_FILE" 2>/dev/null || true)

echo "==> [2/3] admesh 修法线（normal-directions + normal-values）…"
admesh --normal-directions --normal-values --write-ascii-stl="$STL" "$STL" 2>&1 \
    | grep -E "Normals fixed|Degenerate|Volume"

echo "==> [3/3] stl2obj 转 OBJ …"
python3 "$DIR/stl2obj.py" "$STL" "$OBJ"

echo "==> 完成。"
echo "    $STL"
echo "    $OBJ"
echo "    （提交前用 mesh_topo.py 复核：绕序冲突=0 / 非流形=0 / 破口=0）"
