#!/usr/bin/env python3
"""cc13_flash.py — CC1312R 代刷工具（经 RP2040）。

    python3 tools/cc13_flash.py /dev/cu.usbmodemXXXX firmware/cc1312r/build/adsb978_cc13.bin

两条通道
--------
  bsl （默认，**正式通道**）— 走 CC1312R 的 ROM 串行 bootloader (SSI0)。
        前提：目标 flash 里已有一版镜像，且其 CCFG 开了 bootloader+backdoor。
  cjtag（--channel cjtag）— 位脉冲 cJTAG。为空片首刷而写，**实测未打通**，
        留着只为不丢代码，正常不要用（见 HANDOVER-CC13-CJTAG.md）。

⚠ 空片烧不了
------------
空片的 CCFG 是擦除态全 0xFF，BOOTLOADER_ENABLE≠0xC5 → ROM bootloader 被禁用。
串行 bootloader 是**现场升级**机制，不是**首次烧录**机制。第一次必须用 cJTAG
仿真器（J-Link -if cJTAG / XDS110），之后才轮到本工具。

防砖闸门
--------
烧录前解析镜像自带的 CCFG，**拒绝会把升级通道关掉的镜像**。理由见
tools/cc13_ccfg.py：这类镜像烧完不是"这次没成功"，而是"以后再也烧不了"，
而本板没有调试座、也没有那三根网络的测试点，只能拆机焊 QFN 引脚。
确实要烧（比如手上有仿真器）用 --force。

需要 pyserial：pip3 install pyserial
"""
import argparse
import pathlib
import struct
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import cc13_ccfg  # noqa: E402

# stdout 走管道时是块缓冲，而报错走 stderr 不缓冲——不统一的话排错时会看到
# "已拒绝烧录"出现在"镜像 CCFG: ..."之前，上下文和结论是倒着的。
sys.stdout.reconfigure(line_buffering=True)

try:
    import serial
except ImportError:
    print("需要 pyserial：pip3 install pyserial", file=sys.stderr)
    sys.exit(2)

# 各通道的：进入命令、就绪标志、完成标志、失败标志
CHANNELS = {
    "bsl": {
        "cmd": b"U",
        "ready": "BSL-MODE",
        "done": ("BSL-DONE",),
        "fail": ("BSL-FAIL", "BSL-ABORT"),
    },
    "cjtag": {
        "cmd": b"F",
        "ready": "FLASH-MODE READY",
        "done": ("FLASH-DONE",),
        "fail": ("FLASH-FAIL", "FLASH-ABORT", "FLASH-MODE FAIL"),
    },
}


def read_for(ser, seconds, until=()):
    """读 seconds 秒，或直到出现 until 里的任一标志。"""
    out = ""
    deadline = time.time() + seconds
    while time.time() < deadline:
        d = ser.read(4096)
        if d:
            out += d.decode("utf-8", errors="replace")
            if any(k in out for k in until):
                break
    return out


def gate(fw: bytes, force: bool) -> bool:
    """防砖闸门。返回 True 表示可以继续。"""
    probs = cc13_ccfg.problems(fw)
    print(f"镜像 CCFG: {cc13_ccfg.describe(fw)}")
    if not probs:
        return True

    print("\n⚠ 这份镜像会让 CC1312R 失去升级通道：", file=sys.stderr)
    for p in probs:
        print(f"    · {p}", file=sys.stderr)
    if not force:
        print("\n  已拒绝烧录。烧进去之后要恢复就只能：拆机 → 焊 U10 的 24/25 脚"
              "\n  和 R47 → 接 cJTAG 仿真器。确认要烧请加 --force。", file=sys.stderr)
        return False
    print("\n  --force：明知后果仍继续。", file=sys.stderr)
    return True


def flash(port: str, fw_path: str, channel: str, force: bool) -> bool:
    ch = CHANNELS[channel]
    fw = pathlib.Path(fw_path).read_bytes()
    print(f"镜像: {fw_path}（{len(fw)} 字节）  通道: {channel}")

    if not gate(fw, force):
        return False

    try:
        ser = serial.Serial(port, 115200, timeout=1)
    except serial.SerialException as e:
        print(f"打不开 {port}: {e}", file=sys.stderr)
        return False

    try:
        time.sleep(0.3)
        ser.reset_input_buffer()
        ser.write(ch["cmd"])
        resp = read_for(ser, 3.0, until=(ch["ready"],) + ch["fail"])
        if ch["ready"] not in resp:
            print(f"没进烧录模式。设备回应：\n{resp.strip()}", file=sys.stderr)
            return False
        print(f"  {resp.strip()}")

        # 先 4 字节小端长度，再流式发镜像（两条通道同款约定）
        ser.write(struct.pack("<I", len(fw)))
        # 长度头之后设备会立刻回报是否进得去（BSL 要 PING 目标）
        early = read_for(ser, 2.0, until=ch["fail"] + ("RECV",))
        if early.strip():
            print(f"  {early.strip()}")
        if any(k in early for k in ch["fail"]):
            return False

        CHUNK = 4096
        for i in range(0, len(fw), CHUNK):
            ser.write(fw[i:i + CHUNK])
            sent = min(i + CHUNK, len(fw))
            print(f"\r  已发送 {sent}/{len(fw)} 字节", end="", flush=True)
            # 设备端是逐字节状态机 + 每 252 字节一次 flash 写，必须给它时间
            time.sleep(0.02)
        print()

        print("等待 擦除 → 写入 → 校验 → 复位（可能要几分钟）...")
        resp = read_for(ser, 900, until=ch["done"] + ch["fail"])
        if any(k in resp for k in ch["done"]):
            print(f"成功：{resp.strip()}")
            return True
        print(f"未收到完成确认。设备回应：\n{resp.strip()}", file=sys.stderr)
        return False
    finally:
        ser.close()


def main():
    ap = argparse.ArgumentParser(
        description="CC1312R 代刷（经 RP2040）",
        formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("port", help="RP2040 的 USB CDC 串口")
    ap.add_argument("firmware", help="CC1312R 镜像 .bin")
    ap.add_argument("--channel", choices=sorted(CHANNELS), default="bsl",
                    help="默认 bsl（正式通道）；cjtag 实测未打通")
    ap.add_argument("--force", action="store_true",
                    help="绕过防砖闸门（明知会失去升级通道仍要烧）")
    a = ap.parse_args()
    sys.exit(0 if flash(a.port, a.firmware, a.channel, a.force) else 1)


if __name__ == "__main__":
    main()
