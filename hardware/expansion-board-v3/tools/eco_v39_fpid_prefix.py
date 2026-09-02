#!/usr/bin/env python3
"""给 PCB 上的 fpid 补库前缀，让 schematic parity 检查真正跑起来。

板上 178 个封装的 fpid 全是裸名（`C_0603_1608Metric`），而原理图侧带库
（`Capacitor_SMD:C_0603_1608Metric`）。后果不是"报一堆不匹配"，而是更糟的
**静默失效**：KiCad 定位不到库，footprint_symbol_mismatch 这项压根不跑，
于是 parity 报 0，看起来一切正常。

V4 在 2026-09-01 踩过同一个坑——补上前缀后 parity 从 0 变成 212，再修到 22。
V3 这边一开始也报 0，那是因为候选板放在 internal/work/v3.9/，那个目录没有
fp-lib-table，检查同样被跳过；板子挪回 kicad/ 之后才暴露 196 条。

前缀取自网表（原理图是权威），不猜。只改 fpid 这一个 token，几何、位置、
网络、丝印一概不碰——改完用逐项对比验证这一点。

用法：KiCad 自带 python3 tools/eco_v39_fpid_prefix.py [--dry-run]
"""
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PCB = ROOT / "kicad" / "expansion-board-v3.kicad_pcb"
NETLIST = ROOT / "build" / "netlist.xml"


def main() -> int:
    dry = "--dry-run" in sys.argv
    want = {c.attrib["ref"]: (c.findtext("footprint") or "")
            for c in ET.parse(NETLIST).getroot().findall("./components/comp")}
    text = PCB.read_text(encoding="utf-8")

    # 逐个 footprint 块处理：块头的 fpid 与块内的 Reference 属性要对上，
    # 不能按位号全局替换——同一个封装名会出现在很多块里。
    out = []
    cursor = 0
    changed, skipped, unknown = [], [], []
    for match in re.finditer(r'\n\t\(footprint "([^"]+)"', text):
        head_start, head_end = match.start(1), match.end(1)
        current = match.group(1)
        # 往后找这个块的 Reference
        ref_match = re.search(r'\(property "Reference" "([^"]*)"', text[head_end:head_end + 4000])
        if not ref_match:
            continue
        reference = ref_match.group(1)
        target = want.get(reference)
        if not target:
            unknown.append(reference)
            continue
        if current == target:
            skipped.append(reference)
            continue
        if ":" in current and current != target:
            # 已有前缀但和网表不一致——那是真差异，不在本脚本职责内
            skipped.append(f"{reference}(已有前缀 {current})")
            continue
        if target.split(":")[-1] != current:
            unknown.append(f"{reference}: 板上 {current} vs 网表 {target}")
            continue
        out.append(text[cursor:head_start])
        out.append(target)
        cursor = head_end
        changed.append(reference)
    out.append(text[cursor:])
    result = "".join(out)

    print(f"补前缀 {len(changed)} 个 / 已正确 {len(skipped)} 个 / 需人工看 {len(unknown)} 个")
    if unknown:
        for item in unknown[:10]:
            print(f"  ⚠️ {item}")

    # 只允许 fpid 这一个 token 变化：行数必须不变，且差异行全部是 footprint 块头
    before_lines = text.splitlines()
    after_lines = result.splitlines()
    if len(before_lines) != len(after_lines):
        raise SystemExit(f"行数变了 {len(before_lines)} → {len(after_lines)}，中止")
    diff = [i for i, (a, b) in enumerate(zip(before_lines, after_lines)) if a != b]
    bad = [i for i in diff if not after_lines[i].lstrip().startswith('(footprint "')]
    if bad:
        raise SystemExit(f"有非 fpid 行被改动：{[after_lines[i][:60] for i in bad[:3]]}")
    print(f"改动行数 {len(diff)}，全部是 footprint 块头 ✅")

    if dry:
        print("(--dry-run，不落盘)")
        return 0
    PCB.write_text(result, encoding="utf-8")
    print(f"已落盘 → {PCB.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
