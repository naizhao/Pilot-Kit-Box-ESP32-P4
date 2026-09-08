#!/usr/bin/env python3
"""stack_budget_gate.py — UAT 链路栈预算门禁（WP-E-2 P1 修复配套）。

读构建产物目录里的 .su（-fstack-usage）文件，把 UAT 实链路上的
关键函数栈帧钉在预算内。背景（审计 WP-E-2）：core0 栈 2048 B，
静态化前最深链 main→spim_poll→digest→on_uat_frame→p4_link_send_uat
≈3448 B（确定性越界）；修复 = 大缓冲静态化（mosi/miso/msg/chunk/pl），
本门禁防止回归。

用法（build.sh 调用，位于本目录）：
    python3 stack_budget_gate.py <build_dir> <elf>
"""
import re
import subprocess
import sys
from pathlib import Path

# 函数名 → 栈帧预算（字节）。预算依据：静态化后实测 + 余量；
# 回归即红（改函数/编译器版本会翻新预算——更新时注明依据）。
BUDGET = {
    "main": 256,
    "spim_poll": 128,           # 静态化前 1064
    "spi_master_digest": 128,   # 静态化前 1040
    "on_uat_frame": 64,
    "p4_link_send_uat": 128,    # 静态化前 1168
    "spi_master_next_txn": 256,
    "tx_frame": 64,
    "adsb_link_uat_uplink_encode": 128,
}


def su_frames(build_dir: Path):
    frames = {}
    for su in build_dir.rglob("*.su"):
        for line in su.read_text().splitlines():
            # file:line:col:funcname\tbytes\tqualifier
            m = re.match(r".*?:\d+:\d+:(\S+)\s+(\d+)\s+\S+", line)
            if m:
                frames[m.group(1)] = int(m.group(2))
    return frames


def elf_frames(elf: Path):
    """从 ELF 符号表取 BUDGET 关注的函数是否实际链接（gc-sections
    可能裁掉未调用者——被裁函数不检查）。"""
    out = subprocess.run(
        ["arm-none-eabi-nm", str(elf)],
        capture_output=True, text=True, check=True).stdout
    syms = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in ("t", "T"):
            syms.add(parts[2])
    return syms


def main():
    build_dir, elf = Path(sys.argv[1]), Path(sys.argv[2])
    frames = su_frames(build_dir)
    linked = elf_frames(elf)
    fail = 0
    for fn, budget in BUDGET.items():
        if fn not in linked:
            continue
        got = frames.get(fn)
        if got is None:
            print(f"GATE-FAIL {fn}: linked but no .su entry "
                  f"(build config drift?)")
            fail += 1
        elif got > budget:
            print(f"GATE-FAIL {fn}: {got} B > budget {budget} B")
            fail += 1
        else:
            print(f"ok  {fn}: {got} B <= {budget} B")
    sys.exit(1 if fail else 0)


if __name__ == "__main__":
    main()
