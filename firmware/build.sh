#!/usr/bin/env bash
# build.sh — Pilot-Kit ESP32-P4 build/flash wrapper.
#
# Why this exists
# ---------------
# ESP-IDF v6.0.1 does NOT export the legacy `ESP_IDF_VERSION`
# environment variable into the Kconfig build env (only `IDF_VERSION`).
# But `managed_components/espressif__esp_wifi_remote/Kconfig` still
# does:
#
#     orsource "./Kconfig.idf_v$ESP_IDF_VERSION.in"
#
# With an empty `$ESP_IDF_VERSION`, the orsource silently fails →
# `Kconfig.idf_v6.0.in` is never loaded → the `SLAVE_IDF_TARGET_*`
# choice chain doesn't exist → `ESP_HOSTED_CP_TARGET_ESP32C6` is
# hidden (it `depends on SLAVE_IDF_TARGET_ESP32C6`) → the Kconfig
# choice falls back to `ESP_HOSTED_CP_TARGET_ESP32H2` (the only
# unconditional option) → H2 defaults are SPI transport + GPIO 12
# reset → `spi_mempool_create` fails in `__libc_init_array()` →
# entire P4 panics in C-startup before `app_main()` can run.
#
# Symptom in serial log:
#     E (1534) HS_MP: mempool create failed: no mem
#     assert failed: spi_mempool_create spi_drv.c:141 (buf_mp_g)
#     [stack dump]
#     ESP-Hosted is trying to bring up SPI transport instead of SDIO.
#
# Fix: export `ESP_IDF_VERSION=6.0` before invoking idf.py, every
# time. This wrapper does that.
#
# Usage
# -----
# Drop-in replacement for `idf.py`. Examples:
#
#     ./build.sh                       # equivalent to `idf.py build`
#     ./build.sh build
#     ./build.sh -p /dev/cu.usbserial-XXX flash monitor
#     ./build.sh reconfigure
#     ./build.sh menuconfig
#
# If you've already `source`-d ESP-IDF's `export.sh`, this wrapper
# just adds the missing `ESP_IDF_VERSION` and forwards. If you
# haven't, it auto-discovers the install at the expected paths.

set -euo pipefail

# --- 1. The thing this script exists for --------------------------------
#
# Make Kconfig's `$ESP_IDF_VERSION` substitution resolve to the right
# version even when the IDF build system forgot to set it.
: "${ESP_IDF_VERSION:=6.0}"
export ESP_IDF_VERSION

# --- 2. Auto-discover ESP-IDF if env isn't already set up ---------------
#
# Honour the user's environment first; only paper over missing vars if
# they haven't sourced export.sh. The fallback paths match this
# machine's known install (`/Users/samwu/.espressif/...`); adapt
# IDF_PATH and IDF_TOOLS_PATH if you move the install.

DEFAULT_IDF_PATH="${HOME}/.espressif/v6.0.1/esp-idf"
DEFAULT_IDF_TOOLS_PATH="${HOME}/.espressif/tools"

: "${IDF_PATH:=$DEFAULT_IDF_PATH}"
export IDF_PATH

: "${IDF_TOOLS_PATH:=$DEFAULT_IDF_TOOLS_PATH}"
export IDF_TOOLS_PATH

if [ ! -d "$IDF_PATH" ]; then
    echo "build.sh: IDF_PATH does not exist: $IDF_PATH" >&2
    echo "          Source your esp-idf/export.sh first or edit DEFAULT_IDF_PATH in this script." >&2
    exit 1
fi

# --- 3. Python venv (also a non-standard layout on this machine) --------
#
# Stock IDF expects `$IDF_TOOLS_PATH/python_env/idf6.0_py3.13_env/`,
# but the installer on this machine put the venv at
# `$IDF_TOOLS_PATH/python/v6.0.1/venv/`. We symlink the expected
# path on first run so `idf.py` doesn't bail with
# "ESP-IDF Python virtual environment ... not found".

ACTUAL_VENV="${IDF_TOOLS_PATH}/python/v6.0.1/venv"
EXPECTED_VENV="${IDF_TOOLS_PATH}/python_env/idf6.0_py3.13_env"

if [ -d "$ACTUAL_VENV" ] && [ ! -e "$EXPECTED_VENV" ]; then
    mkdir -p "${IDF_TOOLS_PATH}/python_env"
    ln -sfn "$ACTUAL_VENV" "$EXPECTED_VENV"
fi

# idf.py itself looks at both `$HOME/.espressif/python_env/...` and
# `$IDF_TOOLS_PATH/python_env/...` depending on code path, so mirror
# the symlink under $HOME too.
HOME_EXPECTED_VENV="${HOME}/.espressif/python_env/idf6.0_py3.13_env"
if [ -d "$ACTUAL_VENV" ] && [ ! -e "$HOME_EXPECTED_VENV" ]; then
    mkdir -p "${HOME}/.espressif/python_env"
    ln -sfn "$ACTUAL_VENV" "$HOME_EXPECTED_VENV"
fi

: "${IDF_PYTHON_ENV_PATH:=$ACTUAL_VENV}"
export IDF_PYTHON_ENV_PATH

# --- 4. Ninja + RISC-V toolchain on PATH --------------------------------
NINJA_BIN="$(find "$IDF_TOOLS_PATH/ninja" -name ninja -type f -maxdepth 3 2>/dev/null | head -1 || true)"
RV_GCC="$(find "$IDF_TOOLS_PATH/riscv32-esp-elf" -name riscv32-esp-elf-gcc -type f -maxdepth 5 2>/dev/null | head -1 || true)"

export PATH="${IDF_PYTHON_ENV_PATH}/bin:${PATH}"
[ -n "$NINJA_BIN" ] && export PATH="$(dirname "$NINJA_BIN"):${PATH}"
[ -n "$RV_GCC" ]    && export PATH="$(dirname "$RV_GCC"):${PATH}"

# --- 5. Forward to idf.py -----------------------------------------------
if [ "$#" -eq 0 ]; then
    set -- build
fi
exec python "${IDF_PATH}/tools/idf.py" "$@"
