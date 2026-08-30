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
        self.assertIn('"$T/tools/fix_eco_silk_v38.py"', text)

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

    def test_ses_import_ignores_rule_areas_when_counting_filled_zones(self):
        text = (SCRIPT.parent / "import_ses.py").read_text(encoding="utf-8")
        self.assertIn("GetIsRuleArea", text)

    def test_frozen_placement_can_put_eco_passives_on_the_back(self):
        text = (SCRIPT.parent / "gen_pcb.py").read_text(encoding="utf-8")
        self.assertIn("IsFlipped", text)
        self.assertIn("Flip(", text)

    def test_v38_local_eco_routes_are_replayed_after_frozen_routes(self):
        text = SCRIPT.read_text(encoding="utf-8")
        eco_position = text.index('"$T/tools/route_eco_v38.py"')
        import_position = text.index('"$T/tools/import_routes.py"')
        self.assertLess(import_position, eco_position)

    def test_v38_eco_covers_qpl9547_without_rerouting_bno085(self):
        text = (SCRIPT.parent / "route_eco_v38.py").read_text(encoding="utf-8")
        self.assertIn('pad(board, "U11", 1)', text)
        self.assertNotIn('pad(board, "U4", 4)', text)

    def test_rf_layer_check_handles_no_in2_routes(self):
        text = (SCRIPT.parent / "check_route.py").read_text(encoding="utf-8")
        self.assertIn('rf_lay_netnames.get("In2.Cu", set())', text)

    def test_document_generators_do_not_reuse_global_tmp_netlist(self):
        for filename in ("gen_assembly.py", "gen_checklist.py", "gen_bom.py"):
            text = (SCRIPT.parent / filename).read_text(encoding="utf-8")
            self.assertNotIn('NET = "/tmp/expansion.net.xml"', text, filename)

    def test_silk_swap_audit_ignores_fabrication_reference_layers(self):
        text = (SCRIPT.parent / "gen_assembly.py").read_text(encoding="utf-8")
        self.assertIn('"tlayer": board.GetLayerName', text)
        self.assertIn('a.get("tlayer") in ("F.Silkscreen", "B.Silkscreen")', text)

    def test_frozen_silk_snapshot_uses_current_board_revision_on_import(self):
        text = (SCRIPT.parent / "silk_texts.py").read_text(encoding="utf-8")
        self.assertIn('REVISION.sub(BOARD_REV, it["text"])', text)


if __name__ == "__main__":
    unittest.main()
