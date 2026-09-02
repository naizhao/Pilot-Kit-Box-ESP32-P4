#!/usr/bin/env python3
"""阶段 A：从 V3.8 母版建立 V3.9 候选板。

为什么不用工作区那份中间态当母版：它虽然补齐了 14 件、清空了 B 面，但把 24 条板级
丝印和 4 个 RF zone 弄丢了（RG-V3-01/02）。凶手是 `tools/rebuild.sh` 第一步的
`gen_pcb.py:216 pcbnew.NewBoard()` —— 从空板重建，而板级 `gr_text` 和命名 zone 既不在
`PLACEMENT.py` 也不在 `ROUTES.json` 里，**跑一次丢一次**。母版反过来是干净的
（29 条 gr_text / 11 个 zone 齐全），缺的只是元件。

所以方向是"从母版加元件"，不是"从中间态补丝印"——加元件是可枚举可核对的（14 个位号），
恢复丝印要证明 24 条的位置、层、字号、镜像全对，后者贵得多也脆得多。

坐标不是重新摆的，是从中间态**移植**：那 14 件在中间态已经摆好且全在 F.Cu。罩哥选
"从 V3.8 重来"要的是干净母版，不是重复劳动。

幂等：重复跑结果一致（先按位号删掉已存在的同名件再加）。

用法：KiCad 自带 python3 tools/eco_v39_seed_candidate.py
"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pcbnew

ROOT = Path(__file__).resolve().parents[1]
BOARD = "expansion-board-v3"
MASTER_REV = "V3.8"          # 母版取自哪个 commit 的板
WORK = ROOT / "internal" / "work" / "v3.9"
CANDIDATE = WORK / f"{BOARD}-v39-candidate.kicad_pcb"
DONOR = ROOT / "kicad" / f"{BOARD}.kicad_pcb"        # 中间态，只当坐标供体

# 14 个审计修复件：母版里没有，中间态里已摆好
AUDIT_REFS = ("R47", "R50", "R51", "R52", "R53", "R54",
              "C81", "C82", "C83", "C84", "C85", "C86", "D4", "D5")
# 母版里趴在 B.Cu 的四件，中间态已移到 F.Cu —— 单面装配是硬规则
BACKSIDE_REFS = ("C21", "C36", "C48", "R11")

REQUIRED_TEXTS = ("USB", "1090 IFA", "1090 EXT", "978 UAT",
                  "GNSS INT", "GNSS EXT", "1090MHz IFA ANT")
REQUIRED_ZONES = ("ANT1090_open_end_keepout", "ANT1090_short_end_keepout",
                  "ifa_rf50_corridor_ANT1090_IFA", "ifa_rf50_corridor_IFA_MATCH")


def extract_master() -> Path:
    """从 Git 取 V3.8 母版。只读 `git show`，不碰工作区。"""
    WORK.mkdir(parents=True, exist_ok=True)
    out = WORK / f"{BOARD}-{MASTER_REV}-master.kicad_pcb"
    blob = subprocess.run(
        ["git", "show", f"HEAD:hardware/{BOARD}/kicad/{BOARD}.kicad_pcb"],
        cwd=ROOT.parents[1], capture_output=True, check=True,
    ).stdout
    out.write_bytes(blob)
    return out


def board_refs(board) -> dict:
    return {fp.GetReference(): fp for fp in board.GetFootprints()}


def transplant(candidate, donor, reference: str):
    """把 donor 上某个封装整体搬到 candidate。

    用 `pcbnew.Cast_to_FOOTPRINT(fp.Duplicate())` 而不是手工重建：封装里除了焊盘还有
    丝印、courtyard、3D 模型路径、属性位，手工重建必漏。网络在这里**故意不绑**——
    候选板的 net 表来自 V3.8 网表，而阶段 B/C 还会改网络，绑早了要绑两遍。
    统一留到 `eco_v39_bind_nets.py` 按最新网表一次性绑。
    """
    source = board_refs(donor)[reference]
    try:
        clone = source.Duplicate(False)          # KiCad 10：addToParentGroup 参数
    except TypeError:
        clone = source.Duplicate()
    clone = pcbnew.Cast_to_FOOTPRINT(clone)
    clone.SetParent(candidate)
    candidate.Add(clone)
    clone.SetReference(reference)
    return clone


def main() -> int:
    master = extract_master()
    shutil.copyfile(master, CANDIDATE)
    candidate = pcbnew.LoadBoard(str(CANDIDATE))
    donor = pcbnew.LoadBoard(str(DONOR))
    donor_refs = board_refs(donor)

    # ① 补 14 个审计件（幂等：先删同名）
    existing = board_refs(candidate)
    for ref in AUDIT_REFS:
        if ref in existing:
            candidate.Remove(existing[ref])
        if ref not in donor_refs:
            raise SystemExit(f"中间态里没有 {ref}，无法移植坐标")
        transplant(candidate, donor, ref)

    # ② 四件 B.Cu → F.Cu，位置也用中间态的（母版位置是背面布局，翻过来会压到别的件）
    for ref in BACKSIDE_REFS:
        target = board_refs(candidate)[ref]
        src = donor_refs[ref]
        if target.GetLayer() != pcbnew.F_Cu:
            target.Flip(target.GetPosition(), False)
        target.SetPosition(src.GetPosition())
        target.SetOrientation(src.GetOrientation())
        if target.GetLayer() != pcbnew.F_Cu:
            raise SystemExit(f"{ref} 翻面后仍不在 F.Cu")

    candidate.Save(str(CANDIDATE))

    # ③ 验收——阶段 A 的五条成功标准，全部实测
    board = pcbnew.LoadBoard(str(CANDIDATE))
    refs = board_refs(board)
    text = CANDIDATE.read_text(encoding="utf-8")
    texts = re.findall(r'\n\t\(gr_text "([^"]*)"', text)
    zones = re.findall(r'\(name "([^"]+)"\)', text)
    backside = sorted(r for r, fp in refs.items() if fp.GetLayer() != pcbnew.F_Cu)
    missing_audit = [r for r in AUDIT_REFS if r not in refs]
    missing_text = [t for t in REQUIRED_TEXTS if t not in texts]
    missing_zone = [z for z in REQUIRED_ZONES if z not in zones]

    print(f"候选板 → {CANDIDATE.relative_to(ROOT)}")
    print(f"  封装        {len(refs)}")
    print(f"  gr_text     {len(texts)}  缺功能丝印 {missing_text or '无'}")
    print(f"  命名 zone   {len(zones)}  缺 RF zone {missing_zone or '无'}")
    print(f"  B 面元件    {backside or '空集'}")
    print(f"  14 审计件   缺 {missing_audit or '无'}")
    ok = not (missing_text or missing_zone or backside or missing_audit)
    print("✅ 阶段 A 结构判据全过" if ok else "❌ 阶段 A 未达标")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
