#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path


PRODUCT_NAME = "Pilot Kit Box"
PRODUCT_SLUG = "pilot-kit-box"
BOARD_ID = "esp32p4"
CHIP_FAMILY = "ESP32-P4"
DEFAULT_VERSION_FILE = Path(__file__).resolve().parents[2] / "firmware" / "version.txt"

# 扩展板**板系**（v3 / v4，将来还有 v5），不是修订号——V3.9 / V4.3 只是各自
# 当前的修订。一次发布必须每个板系各产出一套：两个板系把 IMU 贴在相差 90° 的
# 角度上，固件按构建时选定的板型换算姿态，刷错了地平仪会整体横滚偏 90°——
# 照样有姿态、照样跟着动，地面上看不出来。顺序与 firmware/main/Kconfig.projbuild
# 的 choice 一致，`tools/firmware_release/test_prepare_esp32p4_release.py` 会对拍。
BOARD_PROFILES = ("v3", "v4")
BOARD_PROFILE_LABELS = {
    "v3": "v3 board (expansion-board-v3)",
    "v4": "v4 board (expansion-board-v4)",
}

FLASH_MODE = "dio"
FLASH_FREQ = "80m"
FLASH_SIZE = "32MB"

BUILD_PARTS = {
    "bootloader": Path("bootloader/bootloader.bin"),
    "partition_table": Path("partition_table/partition-table.bin"),
    "app": Path("pilot_kit_box.bin"),
}

MERGE_OFFSETS = {
    "bootloader": "0x2000",
    "partition_table": "0x8000",
    "app": "0x10000",
}


def normalize_version(version: str) -> str:
    value = version.strip()
    if value.startswith("refs/tags/"):
        value = value.removeprefix("refs/tags/")
    board_tag_prefix = f"{BOARD_ID}-"
    if value.startswith(board_tag_prefix):
        value = value.removeprefix(board_tag_prefix)
    if not value:
        raise ValueError("version must not be empty")
    return value


def default_version(version_file: Path = DEFAULT_VERSION_FILE) -> str:
    if not version_file.is_file():
        raise FileNotFoundError(f"default firmware version file not found: {version_file}")
    return normalize_version(version_file.read_text("utf-8"))


def check_profile(profile: str) -> str:
    if profile not in BOARD_PROFILES:
        raise ValueError(
            f"unknown board profile {profile!r}; expected one of {BOARD_PROFILES}"
        )
    return profile


def artifact_names(version: str, profile: str) -> dict[str, str]:
    """两套产物必须**每一个文件名**都带板型。

    只给 factory bin 加后缀是不够的：bootloader / partition table / app /
    manifest / 校验和 / README 都会落在同一个 dist/release 目录里，任何一个
    重名都会被后打包的那一版静默覆盖，而覆盖后的包看起来完全正常。
    """
    version = normalize_version(version)
    profile = check_profile(profile)
    prefix = f"{PRODUCT_SLUG}-{BOARD_ID}-{profile}-{version}"
    return {
        "factory": f"{prefix}-factory.bin",
        "bootloader": f"{prefix}-bootloader.bin",
        "partition_table": f"{prefix}-partition-table.bin",
        "app": f"{prefix}-app.bin",
        "manifest": f"manifest-{BOARD_ID}-{profile}.json",
        "checksums": f"SHA256SUMS-{BOARD_ID}-{profile}.txt",
        "readme": f"README-{BOARD_ID}-{profile}.md",
        "archive": f"{prefix}.zip",
    }


def build_manifest(version: str, factory_path: str, profile: str | None = None) -> dict:
    # 名字里带板型：esp-web-tools 会把 manifest 的 name 显示在刷写对话框上，
    # 这是用户在按下"开始"之前最后一次能发现自己选错板型的机会。
    label = f" {BOARD_PROFILE_LABELS[check_profile(profile)]}" if profile else ""
    return {
        "name": f"{PRODUCT_NAME} {CHIP_FAMILY}{label}",
        "version": normalize_version(version),
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 0,
        "builds": [
            {
                "chipFamily": CHIP_FAMILY,
                "improv": False,
                "parts": [
                    {
                        "path": factory_path,
                        "offset": 0,
                    }
                ],
            }
        ],
    }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_build_outputs(build_dir: Path) -> dict[str, Path]:
    outputs = {
        key: build_dir / rel_path for key, rel_path in BUILD_PARTS.items()
    }
    missing = [str(path) for path in outputs.values() if not path.is_file()]
    if missing:
        raise FileNotFoundError(
            "missing firmware build output(s): " + ", ".join(missing)
        )
    return outputs


def default_esptool_command() -> list[str]:
    env_cmd = os.environ.get("ESPTOOL_CMD")
    if env_cmd:
        return shlex.split(env_cmd)

    path_cmd = shutil.which("esptool.py")
    if path_cmd:
        return [path_cmd]

    idf_tools = Path(os.environ.get("IDF_TOOLS_PATH", Path.home() / ".espressif" / "tools"))
    idf_python = idf_tools / "python" / "v6.0.1" / "venv" / "bin" / "python"
    if idf_python.is_file():
        return [str(idf_python), "-m", "esptool"]

    return [sys.executable, "-m", "esptool"]


def build_merge_bin_command(
    esptool_cmd: list[str],
    build_outputs: dict[str, Path | str],
    factory_path: Path | str,
) -> list[str]:
    cmd = [
        *esptool_cmd,
        "--chip",
        CHIP_FAMILY,
        "merge-bin",
        "-o",
        str(factory_path),
        "--flash-mode",
        FLASH_MODE,
        "--flash-freq",
        FLASH_FREQ,
        "--flash-size",
        FLASH_SIZE,
    ]
    for key in ("bootloader", "partition_table", "app"):
        cmd.extend([MERGE_OFFSETS[key], str(build_outputs[key])])
    return cmd


def run_merge_bin(
    build_outputs: dict[str, Path],
    factory_path: Path,
    esptool_cmd: list[str] | None = None,
) -> None:
    cmd = build_merge_bin_command(
        esptool_cmd=esptool_cmd or default_esptool_command(),
        build_outputs=build_outputs,
        factory_path=factory_path,
    )
    subprocess.run(cmd, check=True)


def write_json(path: Path, data: dict) -> None:
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", "utf-8")


def write_release_readme(
    path: Path, version: str, names: dict[str, str], profile: str
) -> None:
    label = BOARD_PROFILE_LABELS[check_profile(profile)]
    other = [p for p in BOARD_PROFILES if p != profile][0]
    path.write_text(
        "\n".join(
            [
                f"# {PRODUCT_NAME} {CHIP_FAMILY} {normalize_version(version)} — {label}",
                "",
                f"这个压缩包**只适用于 {label} 扩展板**，用于给已经出厂预刷 C6 协处理器的",
                "Pilot Kit Box 更新 P4 主固件。",
                "",
                "## ⚠️ 先确认板型",
                "",
                "v3 与 v4 板系把 IMU 贴在相差 90° 的角度上，固件按板系换算姿态。",
                "**刷错板型不会报错**：地平仪照样有姿态、照样跟着动，只是横滚整体偏 90°，",
                "桌上不容易看出来。",
                "",
                f"另一版在同一个 release 里，文件名带 `{other}`。",
                "刷完开机后，串口日志里 `imu: board profile ...` 那一行会显示实际生效的板型。",
                "",
                "## 推荐给普通用户",
                "",
                f"- 使用网页刷写：打开 GitHub Pages 刷机页，选 **{label}** 那个按钮，",
                f"  页面会读取 `{names['manifest']}`。",
                f"- 网页刷写使用 `{names['factory']}`，刷写地址为 `0x0`。",
                "",
                "## 给维护者排障",
                "",
                f"- bootloader: `{names['bootloader']}` @ `0x2000`",
                f"- partition table: `{names['partition_table']}` @ `0x8000`",
                f"- app: `{names['app']}` @ `0x10000`",
                "",
                "注意：这只更新 ESP32-P4 主固件，不更新 ESP32-C6 hosted slave 固件。",
                "",
            ]
        ),
        "utf-8",
    )


def write_checksums(path: Path, files: list[Path]) -> None:
    lines = [f"{sha256_file(file)}  {file.name}" for file in files]
    path.write_text("\n".join(lines) + "\n", "utf-8")


def create_archive(path: Path, files: list[Path]) -> None:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for file in files:
            archive.write(file, arcname=file.name)


def copy_web_assets(web_dir: Path, site_dir: Path) -> None:
    if not web_dir.is_dir():
        raise FileNotFoundError(f"web asset directory not found: {web_dir}")
    site_dir.mkdir(parents=True, exist_ok=True)
    for item in web_dir.iterdir():
        target = site_dir / item.name
        if item.is_file():
            shutil.copy2(item, target)


def prepare_release(
    build_dir: Path,
    dist_dir: Path,
    web_dir: Path,
    version: str,
    profile: str,
    esptool_cmd: list[str] | None = None,
) -> None:
    version = normalize_version(version)
    profile = check_profile(profile)
    names = artifact_names(version, profile)
    build_outputs = require_build_outputs(build_dir)

    release_dir = dist_dir / "release"
    site_dir = dist_dir / "site"
    # 每个板型一个 latest 目录。故意**不**再生成不带板型的
    # firmware/esp32p4/latest/：那条路径等于给刷机页留一个"默认刷 V4.3"的
    # 大按钮，V3.9 用户点下去不会有任何提示。
    latest_dir = site_dir / "firmware" / BOARD_ID / profile / "latest"
    release_dir.mkdir(parents=True, exist_ok=True)
    latest_dir.mkdir(parents=True, exist_ok=True)

    copied_bins = {
        "bootloader": release_dir / names["bootloader"],
        "partition_table": release_dir / names["partition_table"],
        "app": release_dir / names["app"],
    }
    for key, target in copied_bins.items():
        shutil.copy2(build_outputs[key], target)

    factory_release = release_dir / names["factory"]
    run_merge_bin(build_outputs, factory_release, esptool_cmd=esptool_cmd)

    manifest = build_manifest(version, names["factory"], profile)
    manifest_release = release_dir / names["manifest"]
    write_json(manifest_release, manifest)

    readme = release_dir / names["readme"]
    write_release_readme(readme, version, names, profile)

    checksum = release_dir / names["checksums"]
    checksum_inputs = [
        factory_release,
        copied_bins["bootloader"],
        copied_bins["partition_table"],
        copied_bins["app"],
        manifest_release,
    ]
    write_checksums(checksum, checksum_inputs)

    archive_path = release_dir / names["archive"]
    create_archive(
        archive_path,
        [factory_release, *copied_bins.values(), manifest_release, checksum, readme],
    )

    copy_web_assets(web_dir, site_dir)
    shutil.copy2(factory_release, latest_dir / names["factory"])
    write_json(latest_dir / names["manifest"], manifest)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Package Pilot Kit Box ESP32-P4 firmware release assets."
    )
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--dist-dir", type=Path, required=True)
    parser.add_argument("--web-dir", type=Path, default=Path("web/flasher"))
    parser.add_argument(
        "--board-profile",
        required=True,
        choices=list(BOARD_PROFILES),
        help="扩展板板型。必须显式给出：两版产物同名会互相覆盖，"
             "而且刷错板型只表现为姿态偏 90°，不会报错。",
    )
    parser.add_argument(
        "--version",
        help="Version label used for release assets; defaults to firmware/version.txt.",
    )
    parser.add_argument(
        "--esptool-cmd",
        help="Optional esptool command prefix, for example: 'python -m esptool'.",
    )
    args = parser.parse_args()

    prepare_release(
        build_dir=args.build_dir,
        dist_dir=args.dist_dir,
        web_dir=args.web_dir,
        version=args.version or default_version(),
        profile=args.board_profile,
        esptool_cmd=shlex.split(args.esptool_cmd) if args.esptool_cmd else None,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
