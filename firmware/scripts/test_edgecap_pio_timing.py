#!/usr/bin/env python3
"""逐周期执行实际 edge_cap.pio，验证去毛刺后的时间守恒与合法脉冲通过。

这不是复制生产常量的数学测试：解释器直接读取 PIO 源码、解析标签并执行
``set/mov/jmp/push``。因此拒绝路径少计一次 ``x--``、修改确认窗口，或高相位
换算常量与 PIO 分离，都会改变观测结果并使本测试失败。
"""

from __future__ import annotations

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PIO = ROOT / "firmware" / "rp2040" / "edge_cap.pio"
HEADER = ROOT / "firmware" / "rp2040" / "edge_cap.h"
PRELOAD = 0xFFFFFFE0


def parse_program() -> tuple[list[str], dict[str, int], int, int]:
    text = re.sub(r"/\*.*?\*/", "", PIO.read_text(encoding="utf-8"), flags=re.S)
    instructions: list[str] = []
    labels: dict[str, int] = {}
    wrap_target = 0
    wrap = -1
    for raw in text.splitlines():
        line = raw.split(";", 1)[0].strip()
        if line.startswith("% c-sdk"):
            break
        if not line or line.startswith("%") or line.startswith(".program"):
            continue
        if line == ".wrap_target":
            wrap_target = len(instructions)
            continue
        if line == ".wrap":
            wrap = len(instructions) - 1
            continue
        if ":" in line:
            label, line = line.split(":", 1)
            labels[label.strip()] = len(instructions)
            line = line.strip()
            if not line:
                continue
        instructions.append(re.sub(r"\s+", " ", line))
    assert wrap >= wrap_target >= 0
    assert len(instructions) <= 32, f"PIO program has {len(instructions)} instructions"
    return instructions, labels, wrap_target, wrap


def high_correction() -> int:
    text = HEADER.read_text(encoding="utf-8")
    match = re.search(r"high \? (\d+)u : 3u", text)
    assert match, "cannot find high-phase correction in edge_cap.h"
    return int(match.group(1))


class Sm:
    def __init__(self) -> None:
        self.ins, self.labels, self.wrap_target, self.wrap = parse_program()
        self.pc = self.wrap_target
        self.x = self.y = self.isr = 0
        self.cycle = 0
        self.pushes: list[tuple[int, int]] = []

    def step(self, pin: int) -> None:
        op = self.ins[self.pc]
        next_pc = self.pc + 1
        if op.startswith("set "):
            reg, value = [v.strip() for v in op[4:].split(",")]
            setattr(self, reg, int(value, 0))
        elif op.startswith("mov "):
            dst, src = [v.strip() for v in op[4:].split(",")]
            invert = src.startswith("~")
            src = src.lstrip("~")
            value = getattr(self, src)
            setattr(self, dst, (~value if invert else value) & 0xFFFFFFFF)
        elif op.startswith("jmp pin "):
            if pin:
                next_pc = self.labels[op.split()[-1]]
        elif op.startswith("jmp x-- "):
            if self.x != 0:
                self.x = (self.x - 1) & 0xFFFFFFFF
                next_pc = self.labels[op.split()[-1]]
        elif op.startswith("jmp y-- "):
            if self.y != 0:
                self.y = (self.y - 1) & 0xFFFFFFFF
                next_pc = self.labels[op.split()[-1]]
        elif op.startswith("jmp "):
            next_pc = self.labels[op.split()[-1]]
        elif op.startswith("push"):
            self.pushes.append((self.cycle, self.isr))
        else:
            raise AssertionError(f"unsupported PIO instruction: {op}")
        if self.pc == self.wrap:
            next_pc = self.wrap_target
        self.pc = next_pc
        self.cycle += 1


def run(intervals: list[tuple[int, int]], end_cycle: int) -> list[tuple[int, int]]:
    """intervals 是半开区间 [start,end)，其中输入为高。"""
    sm = Sm()
    for cycle in range(end_cycle):
        pin = int(any(start <= cycle < end for start, end in intervals))
        sm.step(pin)
    return sm.pushes


def raw_cycles(raw: int, high: bool) -> int:
    correction = high_correction() if high else 3
    return ((PRELOAD - raw) + correction) * 2


def accepted_durations(intervals: list[tuple[int, int]]) -> list[int]:
    pushes = run(intervals, intervals[-1][1] + 200)
    # 首次合法上升先 push low，之后严格 high/low 成对。
    return [raw_cycles(raw, bool(i & 1)) for i, (_, raw) in enumerate(pushes)]


def assert_close(got: int, want: int, tolerance: int = 4) -> None:
    assert abs(got - want) <= tolerance, f"got {got} cycles, want {want}±{tolerance}"


def test_valid_half_microsecond_pulse_passes() -> None:
    durations = accepted_durations([(100, 163)])  # 63 cycles ≈ 0.504 us
    assert len(durations) == 2, durations
    assert_close(durations[1], 63)


def test_rejected_glitches_preserve_following_low_time() -> None:
    base = [(100, 163), (500, 563)]
    one = [(100, 163), (260, 266), (500, 563)]       # 48 ns 毛刺
    many = [(100, 163), (220, 226), (300, 310), (380, 386), (500, 563)]
    base_low = accepted_durations(base)[2]
    one_low = accepted_durations(one)[2]
    many_low = accepted_durations(many)[2]
    assert_close(base_low, 500 - 163)
    assert_close(one_low, base_low)
    assert_close(many_low, base_low)


def test_glitches_inside_modes_intervals_preserve_total_time() -> None:
    for gap_cycles in (63, 125, 313):  # 约 0.5/1.0/2.5 us
        clean = [(100, 163), (163 + gap_cycles, 226 + gap_cycles)]
        glitch_at = 163 + gap_cycles // 2
        noisy = [(100, 163), (glitch_at, glitch_at + 6),
                 (163 + gap_cycles, 226 + gap_cycles)]
        clean_low = accepted_durations(clean)[2]
        noisy_low = accepted_durations(noisy)[2]
        assert_close(clean_low, gap_cycles)
        assert_close(noisy_low, clean_low)


def make_modes_frame(bits: int) -> list[tuple[int, int]]:
    """生成 Mode-S preamble + payload 的合法高电平包络（0.5 us=63 cycles）。"""
    half = 63
    starts = [0, half, 7 * half // 2, 9 * half // 2]
    t0 = 200
    raw_pulses = [(t0 + s, t0 + s + half) for s in starts]
    data0 = t0 + 8 * half
    # 固定但非退化的数据图案；相邻 bit 的脉冲可能自然合并成 1 us 高电平。
    for i in range(bits):
        one = ((i * 29 + 7) ^ (i >> 1)) & 1
        start = data0 + i * 2 * half + (0 if one else half)
        raw_pulses.append((start, start + half))
    pulses: list[tuple[int, int]] = []
    for start, end in sorted(raw_pulses):
        if pulses and pulses[-1][1] == start:
            pulses[-1] = (pulses[-1][0], end)
        else:
            pulses.append((start, end))
    return pulses


def test_complete_56_and_112_bit_envelopes_pass() -> None:
    for bits in (56, 112):
        pulses = make_modes_frame(bits)
        durations = accepted_durations(pulses)
        assert len(durations) == 2 * len(pulses), (bits, len(pulses), len(durations))
        highs = durations[1::2]
        for got, (start, end) in zip(highs, pulses):
            assert_close(got, end - start)


if __name__ == "__main__":
    tests = [value for name, value in sorted(globals().items())
             if name.startswith("test_") and callable(value)]
    for test in tests:
        test()
        print(f"PASS {test.__name__}")
