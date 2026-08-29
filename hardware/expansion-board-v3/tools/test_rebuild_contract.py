#!/usr/bin/env python3
"""冻结重建脚本的布线快照契约。"""

from pathlib import Path
import unittest


SCRIPT = Path(__file__).resolve().parent / "rebuild.sh"


class RebuildContractTest(unittest.TestCase):
    def test_snapshot_is_the_last_command_allowed_to_write_tracks(self):
        text = SCRIPT.read_text(encoding="utf-8")
        import_position = text.index('"$T/tools/import_routes.py"')
        ifa_position = text.index('"$T/tools/route_ifa_feed.py"')

        self.assertLess(ifa_position, import_position)
        self.assertNotIn('"$T/tools/route_gnss_sw.py"', text)

    def test_board_and_build_directories_can_be_isolated(self):
        text = SCRIPT.read_text(encoding="utf-8")
        self.assertIn('BDIR="${PK_BOARD_DIR:-$T/kicad}"', text)
        self.assertIn('B="${PK_BUILD_DIR:-$T/build}"', text)

    def test_silk_drc_fix_is_part_of_rebuild(self):
        text = SCRIPT.read_text(encoding="utf-8")
        self.assertIn('"$T/tools/fix_silk_drc.py"', text)

    def test_rebuild_preserves_frozen_manual_reference_positions(self):
        text = SCRIPT.read_text(encoding="utf-8")
        self.assertIn('PK_PRESERVE_SILK_REF=1', text)

    def test_polarity_audit_output_is_not_cut_with_head_under_pipefail(self):
        text = SCRIPT.read_text(encoding="utf-8")
        line = next(item for item in text.splitlines() if "gen_polarity_marks.py" in item)
        self.assertNotIn("head", line)

    def test_full_reroute_requires_explicit_opt_in(self):
        text = (SCRIPT.parent / "run_route.sh").read_text(encoding="utf-8")
        self.assertIn('PK_ALLOW_REROUTE', text)


if __name__ == "__main__":
    unittest.main()
