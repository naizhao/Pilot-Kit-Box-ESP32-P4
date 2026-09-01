#!/usr/bin/env python3
"""v3/v4 PCB revision、公开文档和制造包的一致性回归测试。"""

from __future__ import annotations

from pathlib import Path
import os
import re
import unittest
import zipfile


ROOT = Path(__file__).resolve().parents[1]
EXPECTED = {
    "expansion-board-v3": "V3.9",
    "expansion-board-v4": "V4.3",
}
AVAILABLE_BOARDS = tuple(
    board for board in EXPECTED
    if (ROOT / "hardware" / board / "kicad" / f"{board}.kicad_pcb").is_file()
)
REQUESTED_BOARDS = tuple(filter(None, os.environ.get("PK_TEST_BOARDS", "").split(",")))
BOARDS = REQUESTED_BOARDS or AVAILABLE_BOARDS
VERSION = re.compile(r"\bV[34]\.\d+\b")


def board_root(board: str) -> Path:
    return ROOT / "hardware" / board


def metadata_file(board: str) -> Path | None:
    base = board_root(board)
    for candidate in (base / "tools" / "board_meta.py", base / "internal" / "tools" / "board_meta.py"):
        if candidate.is_file():
            return candidate
    return None


def current_doc_titles(board: str) -> dict[str, str]:
    """只检查把本板 revision 写在 H1 的当前交付文档，正文历史版本不动。"""
    base = board_root(board)
    result = {}
    for path in sorted(base.glob("*.md")):
        title = path.read_text(encoding="utf-8").splitlines()[0]
        if VERSION.search(title):
            result[path.name] = title
    return result


class BoardRevisionContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        unknown = set(BOARDS) - set(EXPECTED)
        missing = set(BOARDS) - set(AVAILABLE_BOARDS)
        if unknown or missing:
            raise AssertionError(
                f"PK_TEST_BOARDS 无效：unknown={sorted(unknown)}, missing={sorted(missing)}"
            )

    def test_metadata_and_pcb_silkscreen_match_target_revision(self):
        for board in BOARDS:
            expected = EXPECTED[board]
            with self.subTest(board=board):
                metadata = metadata_file(board)
                if metadata is not None:
                    match = re.search(
                        r'^BOARD_REV\s*=\s*"([^"]+)"',
                        metadata.read_text(encoding="utf-8"),
                        re.MULTILINE,
                    )
                    self.assertIsNotNone(match, metadata)
                    self.assertEqual(match.group(1), expected, metadata)

                pcb = board_root(board) / "kicad" / f"{board}.kicad_pcb"
                silk_revisions = set(
                    re.findall(r'\(gr_text "(V[34]\.\d+)\s+(?:\(|\d{4}-)', pcb.read_text(encoding="utf-8"))
                )
                self.assertEqual(silk_revisions, {expected}, pcb)

    def test_current_public_document_titles_match_target_revision(self):
        for board in BOARDS:
            expected = EXPECTED[board]
            base = board_root(board)
            with self.subTest(board=board, document="README.md"):
                readme_title = (base / "README.md").read_text(encoding="utf-8").splitlines()[0]
                self.assertIn(expected, readme_title)

            titles = current_doc_titles(board)
            self.assertTrue(titles, board)
            for filename, title in titles.items():
                with self.subTest(board=board, document=filename):
                    self.assertEqual(VERSION.findall(title), [expected], title)

    def test_v4_release_package_is_revisioned_and_matches_source(self):
        board = "expansion-board-v4"
        if board not in BOARDS:
            self.skipTest("当前分支不维护 v4")

        expected = EXPECTED[board]
        release = board_root(board) / "release"
        # 日期戳不写死：发布日期是发布那天才知道的，写死会让「按时发布」变成
        # 合同的一部分。改成按版本号找，但要求**恰好一个**——同一版本堆着两个
        # 日期的包，下单时挑错哪个都不会有人发现。
        candidates = sorted(
            release.glob(f"expansion-board-v4-gerber-JLC-{expected}-*.zip")
        )
        self.assertEqual(
            len(candidates), 1,
            f"{expected} 的 Gerber 包应当恰好一个，实际 {[p.name for p in candidates]}",
        )
        gerber = candidates[0]
        stamp = gerber.stem.rsplit("-", 1)[1]
        source = release / f"expansion-board-v4-kicad-{expected}-{stamp}.zip"
        self.assertTrue(source.is_file(), source)

        readme = (board_root(board) / "README.md").read_text(encoding="utf-8")
        self.assertIn(gerber.name, readme)
        self.assertIn(source.name, readme)

        with zipfile.ZipFile(gerber) as archive:
            self.assertEqual(len(archive.namelist()), 15)
            self.assertFalse(any(name.endswith(".gbrjob") for name in archive.namelist()))

        pcb = board_root(board) / "kicad" / "expansion-board-v4.kicad_pcb"
        with zipfile.ZipFile(source) as archive:
            self.assertEqual(
                archive.read("kicad/expansion-board-v4.kicad_pcb"),
                pcb.read_bytes(),
            )


if __name__ == "__main__":
    unittest.main()
