import json
import unittest

from . import prepare_esp32p4_release as release


class Esp32p4ReleasePlanTest(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()
