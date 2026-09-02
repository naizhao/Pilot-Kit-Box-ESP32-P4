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
}

SKIP_REASONS = {
    "DNP": "DNP",
    "安装孔": "mounting hole",
    "测试点焊盘": "test-point pad",
    "短接焊盘": "solder-bridge pad",
    "板载天线": "on-board antenna",
    "备用检波位（默认贴 U13）": "alternate detector footprint (U13 is default)",
}


def fields(line):
    return [part.strip() for part in line.strip().strip("|").split("|")]


def translate_stage(stage):
    match = re.match(r"([A-F])\s", stage)
    if match:
        code = match.group(1)
        return f"{code} {STAGES[code]}"
    if "不贴" in stage:
        reason = re.search(r"（(.+)）", stage)
        text = SKIP_REASONS.get(reason.group(1), reason.group(1)) if reason else "DNP"
        return f"**Not placed** ({text})"
    return stage


def checklist_note(ref, note):
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
        heading = re.match(r"## 阶段 ([A-F])", line)
        if heading:
            stage = heading.group(1)
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
    for board, revision in (("v3", "V3.9"), ("v4", "V4.3")):
        board_root = ROOT / "hardware" / f"expansion-board-{board}"
        sync_checklist(board_root, revision)
        sync_assembly(board_root, revision, board)
        print(f"synced expansion-board-{board}: {revision}")


if __name__ == "__main__":
    main()
