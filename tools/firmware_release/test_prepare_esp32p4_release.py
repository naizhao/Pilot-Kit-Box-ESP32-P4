import json
import unittest
from pathlib import Path

from . import prepare_esp32p4_release as release


REPO_ROOT = Path(__file__).resolve().parents[2]


class Esp32p4ReleasePlanTest(unittest.TestCase):
    def test_default_version_reads_firmware_version_file(self):
        self.assertEqual(release.default_version(), "v0.5.0")

    def test_normalize_version_strips_board_tag_prefix(self):
        self.assertEqual(release.normalize_version("refs/tags/esp32p4-v0.5.0"), "v0.5.0")

    def test_artifact_names_include_board_id(self):
        names = release.artifact_names("v1.2.3")

        self.assertEqual(
            names["factory"],
            "pilot-kit-box-esp32p4-v1.2.3-factory.bin",
        )
        self.assertEqual(
            names["archive"],
            "pilot-kit-box-esp32p4-v1.2.3.zip",
        )
        self.assertTrue(all("esp32p4" in value for value in names.values()))

    def test_manifest_targets_esp32p4_merged_binary_at_offset_zero(self):
        manifest = release.build_manifest(
            version="v1.2.3",
            factory_path="pilot-kit-box-esp32p4-v1.2.3-factory.bin",
        )

        build = manifest["builds"][0]
        self.assertEqual(build["chipFamily"], "ESP32-P4")
        self.assertFalse(build["improv"])
        self.assertEqual(
            build["parts"],
            [
                {
                    "path": "pilot-kit-box-esp32p4-v1.2.3-factory.bin",
                    "offset": 0,
                }
            ],
        )

    def test_manifest_is_serializable_json(self):
        manifest = release.build_manifest(
            version="v1.2.3",
            factory_path="pilot-kit-box-esp32p4-v1.2.3-factory.bin",
        )

        encoded = json.dumps(manifest, sort_keys=True)

        self.assertIn('"ESP32-P4"', encoded)
        self.assertIn("pilot-kit-box-esp32p4-v1.2.3-factory.bin", encoded)

    def test_merge_command_accepts_explicit_esptool_prefix(self):
        command = release.build_merge_bin_command(
            esptool_cmd=["/idf/python", "-m", "esptool"],
            build_outputs={
                "bootloader": "/build/bootloader.bin",
                "partition_table": "/build/partition-table.bin",
                "app": "/build/app.bin",
            },
            factory_path="/dist/pilot-kit-box-esp32p4-v1.2.3-factory.bin",
        )

        self.assertEqual(command[:5], ["/idf/python", "-m", "esptool", "--chip", "ESP32-P4"])
        self.assertIn("merge-bin", command)
        self.assertIn("0x2000", command)
        self.assertIn("0x8000", command)
        self.assertIn("0x10000", command)

    def test_release_workflow_injects_version_into_firmware_build(self):
        workflow = REPO_ROOT / ".github" / "workflows" / "release-esp32p4-firmware.yml"

        text = workflow.read_text("utf-8")

        self.assertIn('RELEASE_VERSION="${{ env.RELEASE_VERSION }}"', text)
        self.assertIn('RELEASE_VERSION="${RELEASE_VERSION#esp32p4-}"', text)
        self.assertLess(
            text.index('RELEASE_VERSION="${{ env.RELEASE_VERSION }}"'),
            text.index('RELEASE_VERSION="${RELEASE_VERSION#esp32p4-}"'),
        )
        self.assertIn('idf.py -DPROJECT_VER="$RELEASE_VERSION" build', text)
        self.assertNotIn("bash -lc", text)

    def test_release_workflow_skips_pages_when_pages_is_not_enabled(self):
        workflow = REPO_ROOT / ".github" / "workflows" / "release-esp32p4-firmware.yml"

        text = workflow.read_text("utf-8")

        self.assertIn('gh api "repos/${GITHUB_REPOSITORY}/pages"', text)
        self.assertIn('echo "enabled=false" >> "$GITHUB_OUTPUT"', text)
        self.assertIn("if: steps.pages.outputs.enabled == 'true'", text)
        self.assertIn("if: github.event_name == 'workflow_dispatch'", text)
        self.assertIn(
            "if: github.event_name == 'workflow_dispatch' && needs.build.outputs.pages-enabled == 'true'",
            text,
        )

    def test_release_workflow_writes_notes_outside_docker_owned_dist(self):
        workflow = REPO_ROOT / ".github" / "workflows" / "release-esp32p4-firmware.yml"

        text = workflow.read_text("utf-8")

        self.assertIn('notes="${RUNNER_TEMP}/RELEASE_NOTES-esp32p4.md"', text)
        self.assertNotIn('notes="dist/release/RELEASE_NOTES-esp32p4.md"', text)

    def test_firmware_update_docs_explain_version_source(self):
        docs = REPO_ROOT / "docs" / "firmware_update.md"

        text = docs.read_text("utf-8")

        self.assertIn("firmware/version.txt", text)
        self.assertIn("PROJECT_VER", text)


if __name__ == "__main__":
    unittest.main()
