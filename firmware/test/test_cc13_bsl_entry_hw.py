#!/usr/bin/env python3
"""CC1312R ROM-BSL 代写入口的无写入实机回归测试。

向 RP2040 发送 ``U`` 后必须收到 ``BSL-MODE``。随后发送长度 0，验证状态机
以 ``BSL-ABORT`` 安全退出；长度校验发生在接管引脚和 Flash 操作之前，因此本
测试不会擦除或写入 CC1312R。
"""

import argparse
import os
import select
import struct
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


def read_until(fd: int, marker: bytes, timeout: float) -> bytearray:
    deadline = time.monotonic() + timeout
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
        if marker in received:
            break
    return received


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=True)
    parser.add_argument("--timeout", type=float, default=3.0)
    args = parser.parse_args()

    fd = os.open(args.port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        configure(fd)
        termios.tcflush(fd, termios.TCIFLUSH)
        os.write(fd, b"U")
        entered = read_until(fd, b"BSL-MODE", args.timeout)
        if b"BSL-MODE" not in entered:
            print(entered.decode("utf-8", errors="backslashreplace"))
            print("FAIL: 'U' 未进入 CC1312R ROM-BSL 代写模式")
            return 1

        os.write(fd, struct.pack("<I", 0))
        aborted = read_until(fd, b"BSL-ABORT", args.timeout)
    finally:
        os.close(fd)

    text = (entered + aborted).decode("utf-8", errors="backslashreplace")
    print(text)
    if "BSL-FAIL: 镜像长度非法" not in text or "BSL-ABORT" not in text:
        print("FAIL: 零长度镜像没有安全退出代写模式")
        return 1
    print("PASS: 'U' 已进入代写模式，零长度镜像已安全退出")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
