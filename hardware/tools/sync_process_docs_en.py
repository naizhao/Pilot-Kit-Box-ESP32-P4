#!/usr/bin/env python3
"""从中文权威生成表重建精简英文 CHECKLIST/ASSEMBLY，避免两套表漂移。"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]

STAGES = {
    "A": "Power",
    "B": "MCU + Flash",
    "C": "Sensors + GNSS",
    "D": "978 Transceiver",
    "E": "1090 Receive Chain",
    "F": "External Interface",
    "G": "Airband",          # V4.6 新增：BK4819 + 26MHz TCXO + PCM1808
}

SKIP_REASONS = {
    "DNP": "DNP",
    "安装孔": "mounting hole",
    "测试点焊盘": "test-point pad",
    "短接焊盘": "solder-bridge pad",
    "板载天线": "on-board antenna; per-cut measured 50.0mm outer envelope / 48.5mm centerline span",
    "备用检波位（默认贴 U13）": "alternate detector footprint (U13 is default)",
    # V4.10：J11 从 2x8 排针改成 TestPad 裸焊盘阵列、不再是要贴的件。
    # CHECKLIST（短文案）与 ASSEMBLY（长文案）两处产物各有一段，缺一条
    # 就会把中文原样抄进英文版——J11 曾经因此整行留在英文 Stage B 里。
    "调试焊盘阵列，不贴件": "debug pad array; fit nothing",
    "调试焊盘阵列（2x10 裸焊盘，配 2x10p 测试夹压接），不贴任何件。短接 5-6=复位、7-8=BOOTSEL、19-20=自检":
        "debug pad array (2x10 bare pads, mates with a 2x10p test clip); fit nothing. "
        "Bridge 5-6=reset, 7-8=BOOTSEL, 19-20=self-test",
    "M2.5 安装孔（⌀2.7mm NPTH），不是元件": "M2.5 mounting hole (⌀2.7mm NPTH), not a component",
}


def fields(line):
    return [part.strip() for part in line.strip().strip("|").split("|")]


def unknown_stage(code, context):
    """未登记的阶段必须炸，不能静默。

    2026-09-12 实证：V4.6 在中文表里加了「阶段 G 航空波段」，而这里只认 A–F。
    后果不是"漏掉"，是**错配**——`sync_assembly` 的 `stage` 变量保持上一轮的 "F" 不变，
    19 个 airband 件被整段并进 "Stage F: External Interface"（1 件 → 20 件）；
    `sync_checklist` 这边则把 "G 航空波段 AIRBAND" 原样抄进英文表。
    两边都不报错，产物看着完整。加阶段的人当时是手改英文产物绕过去的，
    于是横幅写着"Do not hand-edit"的文件反而成了唯一正确的那份。
    """
    raise RuntimeError(
        f"{context} 出现未登记的阶段 {code!r}。新增阶段必须先加进本文件的 STAGES，"
        f"否则英文版会漏译或把整段件错配到上一个阶段。"
    )


def translate_stage(stage):
    match = re.match(r"([A-Z])\s", stage)
    if match:
        code = match.group(1)
        if code not in STAGES:
            unknown_stage(code, "CHECKLIST-zh_CN.md")
        return f"{code} {STAGES[code]}"
    if "不贴" in stage:
        reason = re.search(r"（(.+)）", stage)
        text = SKIP_REASONS.get(reason.group(1), reason.group(1)) if reason else "DNP"
        return f"**Not placed** ({text})"
    return stage


def checklist_note(ref, note):
    if ref == "ANT1":
        return "Per-cut measured 50.0mm outer envelope / 48.5mm centerline span"
    if ref in {"D4", "D5"}:
        return "Bidirectional low-capacitance USB ESD protector; no polarity"
    if ref in {"D1", "D2", "D3"}:
        return "Polarized; follow the cathode marking"
    if not note:
        return ""
    if "无源晶体" in note:
        return "Passive crystal; 180-degree rotation is electrically equivalent"
    if "板上印着" in note:
        printed = re.search(r"`([^`]+)`", note)
        return f"Board silkscreen reads {printed.group(1) if printed else 'the wrong reference'}; use coordinates"
    if "DNP" in note:
        return "DNP by default; see the Chinese companion for tuning details"
    # 有源 TCXO 装反会短路烧件——这条必须整句译出来，不能落到下面那条泛泛的
    # "Observe pin 1"。按中文原文的特征词匹配而不是按位号，位号改了也还能命中。
    # （它之前是人肉写进 CHECKLIST.md 的，重跑同步器会被抹掉，2026-09-12 实证。）
    if "上电即烧" in note:
        return (
            "ACTIVE TCXO: pin2=GND / pin4=Vcc — rotating it 180 degrees shorts them and "
            "destroys the part on power-up. There is NO marking on the top face; the 0.2mm "
            "chamfer is on the BOTTOM at corner #1 (datasheet C0.2) — check it before placing, "
            "or use one unit to establish the bottom-chamfer / top-marking relationship first. "
            "The filled silkscreen dot on the board points at pin 1"
        )
    if "pin1" in note.lower() or "1脚" in note or "方向" in note:
        return "Observe pin 1 / signal direction"
    return "See the Chinese companion for the detailed engineering note"


def sync_checklist(board_root, revision):
    source = (board_root / "CHECKLIST-zh_CN.md").read_text(encoding="utf-8").splitlines()
    rows = []
    for line in source:
        if not line.startswith("|") or "**" not in line:
            continue
        parts = fields(line)
        if len(parts) != 7:
            continue
        match = re.fullmatch(r"\*\*([A-Z]+\d+)\*\*", parts[1])
        if not match:
            continue
        ref = match.group(1)
        rows.append([
            parts[0], parts[1], parts[2], parts[3], parts[4],
            translate_stage(parts[5]), checklist_note(ref, parts[6]),
        ])

    out = [
        f"# {revision} BOM Verification Checklist (reference-designator order)",
        "",
        "Chinese companion: [`CHECKLIST-zh_CN.md`](CHECKLIST-zh_CN.md)",
        "",
        "> Generated from the authoritative schematic and PCB coordinates. Do not hand-edit.",
        "> `Not placed` is authoritative: DNP parts, test pads, solder bridges, mounting holes, and PCB antennas do not receive components.",
        "",
        f"{len(rows)} designators in total.",
        "",
        "| ✓ | Designator | Value / Model | Footprint | Position (x, y) | Stage | Notes |",
        "|---|---|---|---|---|---|---|",
    ]
    out.extend("| " + " | ".join(row) + " |" for row in rows)
    (board_root / "CHECKLIST.md").write_text("\n".join(out) + "\n", encoding="utf-8")


def difficulty(text):
    if "底部焊盘" in text:
        return "★★★ Exposed pad; reflow/hot air required"
    if "0402" in text:
        return "★★ 0402; use low airflow"
    if "引脚在外" in text:
        return "★ Exposed leads; hand touch-up possible"
    return "★ Routine"


def sync_assembly(board_root, revision, board):
    source = (board_root / "ASSEMBLY-zh_CN.md").read_text(encoding="utf-8").splitlines()
    grouped = {code: [] for code in STAGES}
    skipped = []
    stage = None
    in_skip = False
    for line in source:
        heading = re.match(r"## 阶段 ([A-Z])", line)
        if heading:
            stage = heading.group(1)
            if stage not in STAGES:
                unknown_stage(stage, "ASSEMBLY-zh_CN.md")
            in_skip = False
            continue
        if line.startswith("## 不贴的"):
            in_skip = True
            stage = None
            continue
        if not line.startswith("|") or "**" not in line:
            continue
        parts = fields(line)
        if in_skip and len(parts) == 2 and re.fullmatch(r"\*\*[A-Z]+\d+\*\*", parts[0]):
            reason = SKIP_REASONS.get(parts[1], parts[1])
            skipped.append((parts[0], reason))
        elif stage and len(parts) == 6 and re.fullmatch(r"\*\*[A-Z]+\d+\*\*", parts[0]):
            grouped[stage].append(parts[:5] + [difficulty(parts[5])])

    out = [
        f"# {revision} Manual SMT Placement List",
        "",
        "Chinese companion: [`ASSEMBLY-zh_CN.md`](ASSEMBLY-zh_CN.md)",
        "",
        "> Generated from the authoritative schematic sheets and PCB coordinates. Do not hand-edit.",
        "> Place by designator and coordinate; the PCB side alone is not a variant rule.",
        "",
    ]
    if board == "v4":
        out.extend([
            "## Assembly variant rule",
            "",
            "- Powered variant: populate the power section, but leave R7/R8 DNP.",
            "- Unpowered variant: omit the complete power section and populate both R7/R8.",
            "- Never combine CH224K with R7/R8. See [VARIANTS.md](VARIANTS.md).",
            "",
        ])
    for code, name in STAGES.items():
        rows = grouped[code]
        # 本板没有的阶段就不出小节。v3 没有航空波段，否则会多出一个空的
        # "Stage G: Airband (0 parts)"。注意这**不能**用来掩盖漏配：
        # 未登记的阶段在上面已经抛错了，走不到这里。
        if not rows:
            continue
        out.extend([
            f"## Stage {code}: {name} ({len(rows)} parts)",
            "",
            "| Designator | Value / Model | Footprint | Board position | Rotation | Manual difficulty |",
            "|---|---|---|---|---|---|",
        ])
        out.extend("| " + " | ".join(row) + " |" for row in rows)
        out.append("")
    out.extend([
        f"## Not placed ({len(skipped)} positions)",
        "",
        "| Designator | Reason |",
        "|---|---|",
    ])
    out.extend(f"| {ref} | {reason} |" for ref, reason in skipped)
    (board_root / "ASSEMBLY.md").write_text("\n".join(out) + "\n", encoding="utf-8")


def main():
    # 版本号只认各板的board_meta.py，禁止在双语同步器里再维护一份写死副本。
    boards = (
        ("v3", ROOT / "hardware" / "expansion-board-v3" / "tools" / "board_meta.py"),
        ("v4", ROOT / "hardware" / "expansion-board-v4" / "internal" / "tools" / "board_meta.py"),
    )
    for board, metadata in boards:
        board_root = ROOT / "hardware" / f"expansion-board-{board}"
        match = re.search(r'^BOARD_REV\s*=\s*"([^"]+)"',
                          metadata.read_text(encoding="utf-8"), re.MULTILINE)
        if not match:
            raise RuntimeError(f"{metadata} 缺少BOARD_REV")
        revision = match.group(1)
        sync_checklist(board_root, revision)
        sync_assembly(board_root, revision, board)
        print(f"synced expansion-board-{board}: {revision}")


if __name__ == "__main__":
    main()
