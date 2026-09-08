#!/usr/bin/env bash
# build.sh — RP2040 扩展板固件（adsb1090）一键构建。本地与 CI 共用同一份。
#
# 用法
# ----
#     ./build.sh        # 无参数全量构建；产物 build/adsb1090.uf2，末尾打印 sha256
#
# 依赖
# ----
# - cmake >= 3.16、宿主 C/C++ 编译器（编译 picotool、pioasm 等宿主工具）；
# - arm-none-eabi-gcc 在 PATH，**必须含 newlib**（pico-sdk 硬编码
#   --specs=nosys.specs，缺 newlib 必编不过）：
#   - macOS：brew install --cask gcc-arm-embedded（formula 版无 newlib，不可用）；
#   - Ubuntu：apt-get install gcc-arm-none-eabi libnewlib-arm-none-eabi。
# - 首次构建需联网：pico-sdk / tinyusb / picotool 全部按 CMakeLists.txt 里
#   带 URL_HASH 的 tarball pin 经 FetchContent 取回并缓存在 build/ 下。
#
# 关于 PICO_SDK_PATH
# ------------------
# 本工程**不读** PICO_SDK_PATH：SDK 版本与完整性都钉死在 CMakeLists.txt
# （2.1.1 tarball + URL_HASH；tinyusb 钉在 SDK 官方 submodule pin
# 86ad6e56，因为 SDK tarball 不含 submodule）。若环境残留该变量，这里
# 主动 unset，避免它漏进 SDK/picotool 的查找逻辑造成"本地能用 CI 不能用"。
set -euo pipefail

cd "$(dirname "$0")"

if [ -n "${PICO_SDK_PATH:-}" ]; then
    echo "build.sh: ignoring PICO_SDK_PATH='${PICO_SDK_PATH}'" \
        "(SDK comes from the pinned FetchContent tarball in CMakeLists.txt)" >&2
    unset PICO_SDK_PATH
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico
cmake --build build

# 产物断言：UF2 由 picotool 在 POST_BUILD 生成，缺了说明后处理没跑完。
test -f build/adsb1090.uf2

# 栈预算门禁（WP-E-2 P1）：UAT 链路关键函数帧 ≤ 预算（.su 来自
# -fstack-usage，见 CMakeLists 注释）。任一 GATE-FAIL 即退出非零。
python3 stack_budget_gate.py build build/adsb1090.elf

# sha256：Linux 有 sha256sum，macOS 有 shasum，二者取其一。
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum build/adsb1090.uf2
else
    shasum -a 256 build/adsb1090.uf2
fi
