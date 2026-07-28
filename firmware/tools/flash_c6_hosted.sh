#!/usr/bin/env bash
#
# 批量烧录 ESP32-C6 的 ESP-Hosted slave 固件。
# 串口未出现时每秒探测一次；发现后只启动一次 esptool 连接，避免反复打开
# macOS USB-UART 设备引发 termios EINVAL。

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIR
FIRMWARE_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
readonly FIRMWARE_DIR
readonly DEFAULT_IMAGE="${FIRMWARE_DIR}/network_adapter_esp32c6.bin"
readonly DEFAULT_IDF_PATH="/Users/samwu/.espressif/v6.0.1/esp-idf"
readonly DEFAULT_IDF_TOOLS_PATH="/Users/samwu/.espressif"
readonly PORT_POLL_SECONDS=1

image_path="${DEFAULT_IMAGE}"
serial_port=""
baud_rate=115200
p4_delay=30
batch_mode=false
check_only=false

usage() {
    cat <<'EOF'
用法：
  firmware/tools/flash_c6_hosted.sh [选项]

选项：
  --image PATH       ESP-Hosted app 镜像
                     默认：firmware/network_adapter_esp32c6.bin
  --port DEVICE      固定串口；默认自动等待 /dev/cu.usbserial-*
  --baud RATE        烧录波特率，默认 115200
  --p4-delay SEC     检出串口后留给 P4 进入 ROM 的倒计时，默认 30 秒
  --batch            批量模式；每块完成后等待串口拔出，再等下一块
  --check-only       只校验环境和镜像，不连接硬件
  -h, --help         显示帮助

H4 接线（不要连接 USB-UART 的 VCC）：
  H4-1 C6_IO9 -> 板上 GND（烧录期间保持短接）
  H4-2 GND    -> USB-UART GND
  H4-3 C6_RXD <- USB-UART TX
  H4-4 C6_TXD -> USB-UART RX

脚本发现串口后会开始倒计时。倒计时期间：
  1. 保持 C6_IO9 接地；
  2. 按住 P4 BOOT；
  3. 点按并松开 P4 RESET；
  4. 再保持 BOOT 约 2 秒后松开。

这样 P4 停在 ROM，不会通过 GPIO54 在写入中途复位 C6。
EOF
}

die() {
    printf '错误：%s\n' "$*" >&2
    exit 1
}

require_value() {
    [[ $# -ge 2 && -n "$2" ]] || die "$1 缺少参数"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --image)
            require_value "$@"
            image_path="$2"
            shift 2
            ;;
        --port)
            require_value "$@"
            serial_port="$2"
            shift 2
            ;;
        --baud)
            require_value "$@"
            baud_rate="$2"
            shift 2
            ;;
        --p4-delay)
            require_value "$@"
            p4_delay="$2"
            shift 2
            ;;
        --batch)
            batch_mode=true
            shift
            ;;
        --check-only)
            check_only=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "未知选项：$1（使用 --help 查看帮助）"
            ;;
    esac
done

[[ "${baud_rate}" =~ ^[0-9]+$ ]] && [[ "${baud_rate}" -gt 0 ]] ||
    die "--baud 必须是正整数"
[[ "${p4_delay}" =~ ^[0-9]+$ ]] || die "--p4-delay 必须是非负整数"
[[ -f "${image_path}" ]] || die "找不到镜像：${image_path}"

export ESP_IDF_VERSION=6.0
export IDF_PATH="${PK_IDF_PATH:-${DEFAULT_IDF_PATH}}"
export IDF_TOOLS_PATH="${PK_IDF_TOOLS_PATH:-${DEFAULT_IDF_TOOLS_PATH}}"

[[ -f "${IDF_PATH}/export.sh" ]] || die "找不到 ESP-IDF 环境脚本：${IDF_PATH}/export.sh"

printf '载入 ESP-IDF：%s\n' "${IDF_PATH}"
# ESP-IDF 的环境脚本会输出较长的激活信息，这里保留错误、隐藏正常输出。
# shellcheck disable=SC1091
. "${IDF_PATH}/export.sh" >/dev/null

python -m esptool version

printf '\n校验 C6 镜像：%s\n' "${image_path}"
image_info="$(python -m esptool --chip esp32c6 image-info "${image_path}")"
printf '%s\n' "${image_info}"

grep -q 'ESP32-C6' <<<"${image_info}" ||
    die "镜像目标不是 ESP32-C6"
grep -q 'Project name:.*network_adapter' <<<"${image_info}" ||
    die "镜像不是 network_adapter"
grep -q 'App version:.*2\.12\.7' <<<"${image_info}" ||
    die "镜像版本不是锁定的 ESP-Hosted 2.12.7"
grep -q 'Flash size:.*4MB' <<<"${image_info}" ||
    die "镜像 flash size 不是 4MB"
grep -q 'Flash freq:.*80m' <<<"${image_info}" ||
    die "镜像 flash 频率不是 80MHz"
grep -q 'Flash mode:.*DIO' <<<"${image_info}" ||
    die "镜像 flash mode 不是 DIO"
grep -q 'Validation hash:.*(valid)' <<<"${image_info}" ||
    die "镜像校验哈希无效"

image_size="$(wc -c <"${image_path}" | tr -d ' ')"
printf '\n镜像大小：%s bytes\nSHA-256：' "${image_size}"
shasum -a 256 "${image_path}" | awk '{print $1}'

if "${check_only}"; then
    printf '\n环境与镜像校验通过；--check-only 未连接或写入硬件。\n'
    exit 0
fi

discover_auto_port() {
    local ports=()
    local candidate

    while IFS= read -r candidate; do
        [[ -n "${candidate}" ]] && ports+=("${candidate}")
    done < <(find /dev -maxdepth 1 -type c -name 'cu.usbserial-*' 2>/dev/null | sort)

    if [[ ${#ports[@]} -eq 0 ]]; then
        return 1
    fi
    if [[ ${#ports[@]} -gt 1 ]]; then
        printf '发现多个 USB-UART 串口：\n' >&2
        printf '  %s\n' "${ports[@]}" >&2
        return 2
    fi

    printf '%s\n' "${ports[0]}"
}

wait_for_port() {
    local waited=0
    local selected=""

    if [[ -n "${serial_port}" ]]; then
        printf '\n每 %d 秒等待串口：%s\n' "${PORT_POLL_SECONDS}" "${serial_port}" >&2
        until [[ -c "${serial_port}" ]]; do
            sleep "${PORT_POLL_SECONDS}"
            waited=$((waited + PORT_POLL_SECONDS))
            if (( waited % 10 == 0 )); then
                printf '仍在等待串口（%d 秒）……\n' "${waited}" >&2
            fi
        done
        printf '%s\n' "${serial_port}"
        return
    fi

    printf '\n每 %d 秒自动探测 /dev/cu.usbserial-*；无需按回车。\n' \
        "${PORT_POLL_SECONDS}" >&2
    while true; do
        if selected="$(discover_auto_port)"; then
            break
        else
            result=$?
        fi
        if [[ ${result} -eq 2 ]]; then
            die "请用 --port 明确指定本次烧录设备"
        fi
        sleep "${PORT_POLL_SECONDS}"
        waited=$((waited + PORT_POLL_SECONDS))
        if (( waited % 10 == 0 )); then
            printf '仍在等待 USB-UART（%d 秒）……\n' "${waited}" >&2
        fi
    done
    printf '%s\n' "${selected}"
}

wait_for_port_removal() {
    local port="$1"
    printf '\n本块已完成。拔出 USB-UART，脚本会每秒检测并继续下一块。\n'
    while [[ -e "${port}" ]]; do
        sleep "${PORT_POLL_SECONDS}"
    done
    printf '已检测到串口移除。\n'
}

prepare_p4_rom() {
    local remaining="${p4_delay}"

    printf '\n请保持 C6_IO9 接地，并让 P4 停在 ROM：\n'
    printf '  按住 P4 BOOT -> 点按 P4 RESET -> 2 秒后松开 BOOT。\n'
    while (( remaining > 0 )); do
        printf '\r%2d 秒后自动识别并烧录 C6……' "${remaining}"
        sleep 1
        remaining=$((remaining - 1))
    done
    printf '\r开始识别并烧录 C6。                   \n'
}

flash_c6() {
    local port="$1"
    python -m esptool \
        --chip esp32c6 \
        --port "${port}" \
        --baud "${baud_rate}" \
        --before no-reset \
        --after no-reset \
        --connect-attempts 0 \
        write-flash \
        --flash-mode dio \
        --flash-freq 80m \
        --flash-size 4MB \
        0x10000 "${image_path}"
}

board_index=1
while true; do
    printf '\n========== 准备第 %d 块板 ==========\n' "${board_index}"
    active_port="$(wait_for_port)"
    printf '检测到串口：%s\n' "${active_port}"

    prepare_p4_rom
    printf '\n开始以 %s baud 写入 0x10000……\n' "${baud_rate}"
    flash_c6 "${active_port}" ||
        die "无法识别或写入 C6。检查 IO9、RX/TX、GND 和 P4 ROM 步骤；脚本不会自动重复擦写"

    printf '\n第 %d 块烧录成功。请断电，移除 C6_IO9-GND 短接，再正常启动。\n' \
        "${board_index}"

    if ! "${batch_mode}"; then
        break
    fi

    wait_for_port_removal "${active_port}"
    board_index=$((board_index + 1))
done
