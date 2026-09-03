import json
import unittest
from pathlib import Path

from . import prepare_esp32p4_release as release


REPO_ROOT = Path(__file__).resolve().parents[2]


class Esp32p4ReleasePlanTest(unittest.TestCase):
    def test_default_version_reads_firmware_version_file(self):
        self.assertEqual(release.default_version(), "v0.8.0")

    def test_normalize_version_strips_board_tag_prefix(self):
        self.assertEqual(release.normalize_version("refs/tags/esp32p4-v0.8.0"), "v0.8.0")

    def test_artifact_names_include_board_id(self):
        # 2026-09-03：产物按扩展板板型拆成两套，profile 成为必填参数。
        names = release.artifact_names("v1.2.3", profile="v4")

        self.assertEqual(
            names["factory"],
            "pilot-kit-box-esp32p4-v4-v1.2.3-factory.bin",
        )
        self.assertEqual(
            names["archive"],
            "pilot-kit-box-esp32p4-v4-v1.2.3.zip",
        )
        self.assertTrue(all("esp32p4" in value for value in names.values()))

    def test_artifact_names_reject_an_unknown_profile(self):
        with self.assertRaises(ValueError):
            release.artifact_names("v1.2.3", profile="v9_9")

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
        # 判语义不判措辞：构建命令现在在两个板型的循环里，前缀是
        # `idf.py -B "build_${profile}" ... \` 续行，绑死整行会误杀正确实现。
        self.assertIn('-DPROJECT_VER="$RELEASE_VERSION" build', text)
        self.assertNotIn("bash -lc", text)

    def test_cmake_preserves_explicit_release_version(self):
        cmake = REPO_ROOT / "firmware" / "CMakeLists.txt"
        text = cmake.read_text("utf-8")

        self.assertIn(
            'if(NOT DEFINED PROJECT_VER OR PROJECT_VER STREQUAL "")',
            text,
        )

    def test_release_workflow_exports_pages_site_without_deploying_pages(self):
        workflow = REPO_ROOT / ".github" / "workflows" / "release-esp32p4-firmware.yml"

        text = workflow.read_text("utf-8")

        self.assertIn("name: pilot-kit-box-esp32p4-pages-site", text)
        self.assertIn("path: dist/site", text)
        self.assertNotIn("actions/configure-pages", text)
        self.assertNotIn("actions/upload-pages-artifact", text)
        self.assertNotIn("actions/deploy-pages", text)
        self.assertNotIn("environment:\n      name: github-pages", text)

    def test_pages_workflow_deploys_site_artifact_from_release_run(self):
        workflow = REPO_ROOT / ".github" / "workflows" / "deploy-esp32p4-pages.yml"

        text = workflow.read_text("utf-8")

        self.assertIn("workflow_run:", text)
        self.assertIn("Release ESP32-P4 firmware", text)
        self.assertIn("actions: read", text)
        self.assertIn("actions/download-artifact@v4", text)
        self.assertIn("name: pilot-kit-box-esp32p4-pages-site", text)
        self.assertIn(
            "run-id: ${{ github.event_name == 'workflow_run' && github.event.workflow_run.id || github.event.inputs.run_id }}",
            text,
        )
        self.assertIn("actions/upload-pages-artifact@v4", text)
        self.assertIn("actions/deploy-pages@v4", text)
        self.assertIn("environment:\n      name: github-pages", text)

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

    def test_readme_points_to_public_firmware_updater(self):
        readme = REPO_ROOT / "README.md"

        text = readme.read_text("utf-8")

        self.assertIn("https://updater.pilotkit.app", text)

    # --- 扩展板板型：一次发布必须同时产出 V3.9 与 V4.3 两套 -------------
    #
    # 为什么必须两套都发：V3.9 与 V4.3 把 IMU 贴在相差 90° 的角度上，固件按
    # 构建时选定的板型换算姿态。只发一套等于让另一半用户刷进去一个「照样有
    # 姿态、只是横滚偏 90°」的固件——地面上看不出来。

    def test_artifact_names_are_namespaced_per_board_profile(self):
        v39 = release.artifact_names("v1.2.3", profile="v3")
        v43 = release.artifact_names("v1.2.3", profile="v4")

        # 每个产物名都必须带板型，否则两套会互相覆盖。
        for key in v39:
            with self.subTest(key=key):
                self.assertIn("v3", v39[key], f"{key} 没带板型")
                self.assertIn("v4", v43[key], f"{key} 没带板型")
                self.assertNotEqual(v39[key], v43[key])

        self.assertEqual(
            v43["factory"], "pilot-kit-box-esp32p4-v4-v1.2.3-factory.bin"
        )
        self.assertEqual(v39["manifest"], "manifest-esp32p4-v3.json")

    def test_board_profiles_match_the_firmware_kconfig_choice(self):
        # 发布侧的板型清单必须和固件 Kconfig 的 choice 一一对应，不能各写各的。
        kconfig = (
            REPO_ROOT / "firmware" / "main" / "Kconfig.projbuild"
        ).read_text("utf-8")
        for profile in release.BOARD_PROFILES:
            with self.subTest(profile=profile):
                self.assertIn(
                    f"config PK_BOARD_PROFILE_{profile.upper()}", kconfig
                )
                fragment = REPO_ROOT / "firmware" / f"sdkconfig.defaults.{profile}"
                self.assertTrue(fragment.is_file(), f"缺少 {fragment.name}")
        self.assertEqual(list(release.BOARD_PROFILES), ["v3", "v4"])

    def test_release_workflow_builds_both_board_profiles(self):
        workflow = REPO_ROOT / ".github" / "workflows" / "release-esp32p4-firmware.yml"
        text = workflow.read_text("utf-8")

        # 两个板型都必须进构建循环。列表来自 BOARD_PROFILES，tag 推送时它
        # 恒为两版；判的是这个默认值，不是循环那一行的字面写法。
        self.assertIn("for profile in ${BOARD_PROFILES}", text)
        self.assertIn("'v3 v4'", text)

        # 独立 build 目录：共用一个会留下上一版的 .o，症状是"改了没生效"。
        self.assertIn('-B "build_${profile}"', text)
        self.assertIn('-DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.${profile}"', text)
        self.assertIn('-DSDKCONFIG="build_${profile}/sdkconfig"', text)
        self.assertIn('--build-dir "build_${profile}"', text)
        self.assertIn('--board-profile "${profile}"', text)

        # 构建产物必须被验证真的带上了选定板型，而不是悄悄回落到 Kconfig 默认。
        self.assertIn("CONFIG_PK_BOARD_PROFILE_${upper}", text)
        self.assertIn("config/sdkconfig.h", text)

        # 两套同名会互相静默覆盖，必须有个数量兜底。
        self.assertIn("-factory.bin | wc -l", text)

        # 旧的单板型构建路径不能留着，否则会悄悄多产出一份没板型标识的固件。
        self.assertNotIn("idf.py set-target esp32p4\n", text)
        self.assertNotIn("--build-dir build \\", text)

    def test_release_workflow_can_build_one_profile_on_demand(self):
        workflow = REPO_ROOT / ".github" / "workflows" / "release-esp32p4-firmware.yml"
        text = workflow.read_text("utf-8")

        # 手动触发时可以只编一版，用于验证/排障。choice 型输入由 GitHub 限定
        # 取值，不会把任意字符串带进 shell。
        self.assertIn("board_profile:", text)
        self.assertIn("type: choice", text)
        for option in ("both", "v3", "v4"):
            with self.subTest(option=option):
                self.assertIn(f"- {option}", text)
        # tag 推送必须仍然两版都编——发布不允许只发一半。
        self.assertIn("BOARD_PROFILES", text)

    def test_partial_build_does_not_publish_site_or_release(self):
        """只编一版时不能发布 Pages 站点，也不能发 Release 资产。

        Pages 站点上两个板型各占一个 manifest 目录，刷机页两个按钮各指一个。
        单板型构建的 dist/site 里只有一半，一旦发布出去，另一版用户点按钮会 404。
        """
        workflow = REPO_ROOT / ".github" / "workflows" / "release-esp32p4-firmware.yml"
        text = workflow.read_text("utf-8")

        site_step = text.index("name: pilot-kit-box-esp32p4-pages-site")
        release_step = text.index("name: Publish GitHub Release assets")
        self.assertIn("BUILD_IS_COMPLETE", text[:site_step])
        self.assertIn("BUILD_IS_COMPLETE", text[:release_step])

    def test_pages_workflow_skips_runs_without_a_site_artifact(self):
        """单板型验证构建不产出站点产物，Pages 部署要安静跳过而不是报红。

        每次单板型构建都让 Pages 工作流失败一次，会把人训练成无视红叉。
        """
        workflow = REPO_ROOT / ".github" / "workflows" / "deploy-esp32p4-pages.yml"
        text = workflow.read_text("utf-8")

        self.assertIn("actions/runs/", text)
        self.assertIn("GITHUB_OUTPUT", text)
        self.assertIn("steps.site_artifact.outputs.found == 'true'", text)

    def test_flasher_page_makes_the_user_pick_a_board_profile(self):
        page = (REPO_ROOT / "web" / "flasher" / "index.html").read_text("utf-8")

        for profile in ("v3", "v4"):
            with self.subTest(profile=profile):
                self.assertIn(
                    f"firmware/esp32p4/{profile}/latest/manifest-esp32p4-{profile}.json",
                    page,
                )
        # 不能再有一个不带板型的 manifest —— 那就是"点大按钮默认刷 V4.3"，
        # V3.9 用户会静默刷错。
        self.assertNotIn('manifest="firmware/esp32p4/latest/', page)
        self.assertNotIn("manifest-esp32p4.json", page)
        # 页面必须写清楚选错的后果，不能只摆两个按钮。
        self.assertIn("90", page)

    def test_flasher_page_uses_shared_svg_icon(self):
        page = REPO_ROOT / "web" / "flasher" / "index.html"
        icon = REPO_ROOT / "web" / "flasher" / "favicon.svg"

        text = page.read_text("utf-8")

        self.assertTrue(icon.is_file())
        self.assertIn('<link rel="icon" type="image/svg+xml" href="favicon.svg" />', text)
        self.assertIn('<img src="favicon.svg" alt="" />', text)


if __name__ == "__main__":
    unittest.main()
