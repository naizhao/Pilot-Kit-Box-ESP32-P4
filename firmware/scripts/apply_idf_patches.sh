#!/usr/bin/env bash
# apply_idf_patches.sh — 把 firmware/patches/ 下的 IDF 组件 patch 应用到本地 IDF 安装。
#
# 背景：P4 rev<v3 上 esp_driver_jpeg 的 rxlink/txlink 要 MALLOC_CAP_DEFAULT，
# 但 P4 最大的 DMA|INTERNAL 内存块(0x4ff62140, 131KB)没有 DEFAULT cap →
# jpeg_new_decoder_engine 永久 NO_MEM。patch 去掉 rxlink/txlink 的 DEFAULT，
# 让它们命中那块大内存。详见 memory: p4-jpeg-dma-no-default-cap。
#
# 幂等：已应用的 patch 会跳过。在 idf.py build 前手动跑，或加到 CI。
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PATCHES_DIR="$SCRIPT_DIR/../patches"

: "${IDF_PATH:?需要先 source IDF 环境（export.sh），IDF_PATH 未设}"

applied=0
skipped=0
failed=0

for patch in "$PATCHES_DIR"/*.patch; do
    [ -f "$patch" ] || continue
    name=$(basename "$patch")

    # patch 文件名约定：COMPONENT__描述.patch，组件名 = 第一段（双下划线前）
    comp="${name%%__*}"
    # 单下划线分隔的组件名 esp_driver_jpeg_rxlink... → 取到 .patch 前去掉后缀
    # 简化：文件名就是 esp_driver_jpeg_rxlink_no_default.patch，组件 = esp_driver_jpeg
    case "$name" in
        esp_driver_jpeg_*) comp="$IDF_PATH/components/esp_driver_jpeg" ;;
        *) echo "[skip] $name: 未知组件前缀"; continue ;;
    esac

    if [ ! -d "$comp" ]; then
        echo "[FAIL] $name: 组件目录不存在 $comp"
        failed=$((failed+1))
        continue
    fi

    # 幂等检测：先测正向 dry-run(--forward 不交互、不容忍已应用)。
    #   正向成功 = 未应用 → 应用它
    #   正向失败 = 已应用 或 冲突 → 再测反向 dry-run 区分
    if (cd "$comp" && patch --forward -p1 --dry-run --silent < "$patch" >/dev/null 2>&1); then
        (cd "$comp" && patch --forward -p1 --silent --no-backup-if-mismatch < "$patch")
        echo "[apply] $name → $comp"
        applied=$((applied+1))
    elif (cd "$comp" && patch -R --forward -p1 --dry-run --silent < "$patch" >/dev/null 2>&1); then
        echo "[ok]   $name 已应用，跳过"
        skipped=$((skipped+1))
    else
        echo "[FAIL] $name: patch 不匹配（IDF 版本变了？文件已改？）"
        failed=$((failed+1))
    fi
done

echo "---"
echo "applied=$applied skipped=$skipped failed=$failed"
[ "$failed" -eq 0 ] || exit 1
