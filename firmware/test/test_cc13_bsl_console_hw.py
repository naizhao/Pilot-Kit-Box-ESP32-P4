#!/usr/bin/env python3
"""CC1312R ROM-BSL 控制台入口的实机回归测试。

只发送只读诊断命令 ``L``；诊断会复位 CC1312R 并发送 PING 矩阵，但不会
执行 BANK_ERASE、DOWNLOAD 或 SEND_DATA。
"""

import argparse
import os
import re
import select
import termios
import time


def configure(fd: int) -> None:
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()

    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        configure(fd)
        termios.tcflush(fd, termios.TCIFLUSH)
        os.write(fd, b"L")

        deadline = time.monotonic() + args.timeout
        received = bytearray()
        while time.monotonic() < deadline:
            ready, _, _ = select.select([fd], [], [], 0.25)
            if not ready:
                continue
            try:
                chunk = os.read(fd, 8192)
            except BlockingIOError:
                continue
            received.extend(chunk)
            if b"cc13-bsl: DIAG-DONE" in received:
                break
    finally:
        os.close(fd)

    text = received.decode("utf-8", errors="backslashreplace")
    print(text)
    if "cc13-bsl: ROM 串行 bootloader (SSI0)" not in text:
        print("FAIL: 'L' 未进入 CC1312R SSI ROM-BSL 诊断")
        return 1
    mode_lines = [line for line in text.splitlines() if line.lstrip().startswith("mode")]
    if len(mode_lines) != 16:
        print(f"FAIL: 应有 16 组 PING 变体，实收 {len(mode_lines)} 组")
        return 1
    incomplete = [line for line in mode_lines if len(re.findall(r" [0-9A-F]{2}", line)) != 15]
    if incomplete:
        print(f"FAIL: {len(incomplete)} 组 PING 回读没有完整收到 15 字节")
        return 1
    wait_lines = [line for line in text.splitlines() if line.lstrip().startswith("wait=")]
    if len(wait_lines) != 16:
        print(f"FAIL: 应有 16 组复位等待扫描，实收 {len(wait_lines)} 组")
        return 1
    if any("DIO8=" not in line for line in wait_lines):
        print("FAIL: 复位等待扫描缺少首字节后的 DIO8 上拉/下拉探针")
        return 1
    if "cc13-bsl: DIAG-DONE" not in text:
        print("FAIL: 诊断没有完整结束")
        return 1
    if "ACK(0xCC)!" not in text:
        print("FAIL: TI mode3/FSS 建立时间诊断仍未收到 ROM PING ACK")
        return 1
    print("PASS: 'L' 已完成 PING 变体与复位等待/DIO8 高阻诊断")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
