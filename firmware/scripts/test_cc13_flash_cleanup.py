#!/usr/bin/env python3
"""主机代写工具在目标早期失败后必须把 RP2040 恢复到命令模式。"""

from __future__ import annotations

import importlib.util
import struct
import sys
import types
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "tools"
FIRMWARE = ROOT / "firmware" / "cc1312r" / "release" / "adsb978_cc13.bin"


class ProtocolSerial:
    """只实现本测试涉及的 RP2040 CDC 协议边界。"""

    def __init__(self) -> None:
        self.rx = bytearray()
        self.phase = "idle"
        self.remaining = 0

    def reset_input_buffer(self) -> None:
        self.rx.clear()

    def write(self, data: bytes) -> int:
        if self.phase == "idle" and data == b"U":
            self.phase = "length"
            self.rx.extend(b"BSL-MODE\n")
        elif self.phase == "length":
            assert len(data) == 4
            self.remaining = struct.unpack("<I", data)[0]
            self.phase = "discard"
            self.rx.extend(b"BSL-FAIL: no target ACK\n")
        elif self.phase == "discard":
            consumed = min(len(data), self.remaining)
            self.remaining -= consumed
            if self.remaining == 0:
                self.phase = "idle"
                self.rx.extend(b"BSL-ABORT\n")
        return len(data)

    def read(self, size: int) -> bytes:
        data = bytes(self.rx[:size])
        del self.rx[:size]
        return data

    def close(self) -> None:
        pass


def load_tool(device: ProtocolSerial):
    serial_stub = types.ModuleType("serial")
    serial_stub.__path__ = []
    serial_stub.SerialException = OSError
    serial_stub.Serial = lambda *_args, **_kwargs: device
    tools_stub = types.ModuleType("serial.tools")
    list_ports_stub = types.ModuleType("serial.tools.list_ports")
    port = types.SimpleNamespace(device="fake-port", vid=0x2E8A, pid=0x000A,
                                 serial_number="TEST-RP2040")
    list_ports_stub.comports = lambda: [port]
    tools_stub.list_ports = list_ports_stub
    serial_stub.tools = tools_stub
    sys.modules["serial"] = serial_stub
    sys.modules["serial.tools"] = tools_stub
    sys.modules["serial.tools.list_ports"] = list_ports_stub
    sys.path.insert(0, str(TOOLS))
    spec = importlib.util.spec_from_file_location("cc13_flash_under_test",
                                                  TOOLS / "cc13_flash.py")
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    device = ProtocolSerial()
    tool = load_tool(device)

    assert not tool.flash("fake-port", str(FIRMWARE), "bsl", False)
    assert device.phase == "idle", (
        "早期 BSL-FAIL 后 RP2040 仍在吞载荷，后续控制命令会全部失效"
    )
    assert device.remaining == 0
    print("PASS: 早期 BSL-FAIL 后主机已恢复 RP2040 命令模式")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
