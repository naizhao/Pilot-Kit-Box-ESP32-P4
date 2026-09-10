#!/usr/bin/env python3
"""cc13_flash.py — CC1312R 代理烧录工具（经 RP2040 cJTAG 位脉冲）.

用法:
    python3 cc13_flash.py /dev/ttyACM0 firmware.bin

前置:
    - 扩展板已上电、RP2040 已刷入含 cJTAG 代刷功能的固件
    - 用 USB-C 数据线连接扩展板的 USB-C 口（J4）到电脑
    - 1090 解码会暂停；烧录完成后自动恢复

流程:
    1. 打开 RP2040 USB CDC 串口
    2. 发 'F' → RP2040 进代刷模式（读 IDCODE 作为通信证明）
    3. 发 4 字节小端长度（方案 A），再流式发送固件镜像
    4. 收满长度即自动擦写校验 → RESET CC1312R → 打印 FLASH-DONE

注意:
    - 需要 pyserial: pip3 install pyserial
    - 串口波特率 115200（CDC 虚拟串口，实际不限速）
"""
import struct
import sys
import time
import serial


def flash(port: str, fw_path: str) -> bool:
    try:
        ser = serial.Serial(port, 115200, timeout=1)
    except serial.SerialException as e:
        print(f"ERROR: cannot open {port}: {e}")
        return False

    fw = open(fw_path, "rb").read()
    print(f"Firmware: {fw_path} ({len(fw)} bytes)")

    # 进入代刷模式
    print("Entering flash mode...")
    ser.reset_input_buffer()
    ser.write(b"F")
    time.sleep(0.5)
    resp = ser.read(4096).decode("utf-8", errors="replace")
    if "FLASH-MODE READY" not in resp:
        print(f"ERROR: flash mode not entered. Response:\n{resp}")
        ser.close()
        return False
    print(f"  {resp.strip()}")

    # 方案 A：先发 4 字节小端长度（镜像字节数），再发镜像本体。
    ser.write(struct.pack("<I", len(fw)))

    # 流式发送镜像
    CHUNK = 4096
    for i in range(0, len(fw), CHUNK):
        chunk = fw[i:i + CHUNK]
        ser.write(chunk)
        print(f"\r  Sent {min(i + CHUNK, len(fw))}/{len(fw)} bytes", end="", flush=True)
        time.sleep(0.01)  # 给 RP2040 时间处理

    print()

    # 收满长度即自动擦写校验 + RESET（无需 'Q'）；等待 FLASH-DONE。
    # 352KB 按 250kHz 位脉冲要分钟级，不能用固定短超时。
    print("Waiting for erase + program + verify + reset (may take minutes)...")
    resp = ""
    deadline = time.time() + 900
    while time.time() < deadline:
        d = ser.read(4096)
        if d:
            resp += d.decode("utf-8", errors="replace")
            if any(k in resp for k in ("FLASH-DONE", "FLASH-FAIL", "FLASH-ABORT")):
                break
    if "FLASH-DONE" in resp:
        print(f"SUCCESS: {resp.strip()}")
        ser.close()
        return True
    else:
        print(f"WARNING: no FLASH-DONE confirmation. Response:\n{resp}")
        ser.close()
        return False


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <serial_port> <firmware.bin>")
        print(f"Example: {sys.argv[0]} /dev/ttyACM0 adsb978_cc13.bin")
        sys.exit(1)

    port, fw = sys.argv[1], sys.argv[2]
    ok = flash(port, fw)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
