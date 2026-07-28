#!/usr/bin/env python3
"""从模拟器批量生成 UI 截图，落到仓库根的 images/。

为什么要脚本而不是手工截
------------------------
这批图要进 README 和官网。UI 每改一次它们就全部过期，而手工截图的代价高到
让人不想重截——于是网站上挂着半年前的界面。写成脚本后重出一遍只要一条命令，
过期成本降到接近零。

同时它也是**回归基线**：改完布局跑一次，用 git diff 看哪些图变了，就知道
改动波及了哪些场景。没变的图 git 不会记录，变了的一眼可见。

每张图的「场景」由环境变量拼出来（模拟器一路加过来的那些开关），
所以这里的场景表同时也是一份「模拟器支持哪些状态」的清单。
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

SIM_DIR = Path(__file__).resolve().parent
REPO = SIM_DIR.parent
BUILD = SIM_DIR / "build-capture"
OUT = REPO / "images"

# 定格在动画的哪一秒。13.7 s 时 pitch≈0、roll≈-24°，地平线倾斜但不极端，
# 俯仰正负刻度同时可见——是最能说明问题的一帧。换值会让所有图一起变。
AT_SEC = "13.7"

# name → 环境变量。空 dict = 默认态（PFD 主页，中文，dock 收起）。
SCENES: list[tuple[str, dict[str, str], str]] = [
    # PFD 主页**没有**中英两版：这一屏全是国际通用的符号、数字与固定缩写
    # （HDG / KM/H / ALT / VS），一个 i18n 词条都没有，两种语言渲染逐字节相同。
    # 这与 ICAO 标准仪表不做本地化是一致的——语言只影响导航与设置这类文字界面。
    ("ui-4.3-pfd",          {},                                   "PFD 主页"),
    ("ui-4.3-dock",         {"PK_SIM_DOCK": "1"},                 "dock 展开：六页签 + 调平"),
    ("ui-4.3-dock-en",      {"PK_SIM_DOCK": "1",
                             "PK_SIM_LANG": "en"},                "dock 展开（英文，页签最宽的一版）"),
    ("ui-4.3-dock-left",    {"PK_SIM_DOCK": "1",
                             "PK_SIM_FAB": "left"},               "FAB 吸左缘，dock 反向铺开"),
    ("ui-4.3-subpage",      {"PK_SIM_SUB": "1"},                  "二级页面：返回栏 + FAB 变 ←"),
    ("ui-4.3-toast",        {"PK_SIM_TOAST": "1"},                "Toast 提示压在最上层"),
    ("ui-4.3-battery-low",  {"PK_SIM_BATT": "3"},                 "低电量：电池转 alert 图标并变红"),
    ("ui-4.3-charging",     {"PK_SIM_BATT": "45",
                             "PK_SIM_CHARGING": "1"},             "充电中：电池播放逐帧动画"),
]


def run(cmd: list[str], **kw) -> None:
    subprocess.run(cmd, check=True, capture_output=True, **kw)


def ensure_sim() -> Path:
    """配置并构建模拟器，返回可执行文件路径。

    用独立的 build-capture 目录：开发时常在 sim/build* 里切分辨率做实验，
    截图必须固定在 800×480，不能受那些实验状态影响。
    """
    run(["cmake", "-S", str(SIM_DIR), "-B", str(BUILD), "-DPANEL=800x480"])
    run(["cmake", "--build", str(BUILD), "-j8"])
    exe = BUILD / "pkbox_sim"
    if not exe.exists():
        sys.exit(f"构建后仍找不到 {exe}")
    return exe


def capture(exe: Path, name: str, env_extra: dict[str, str], tmp: Path) -> Path:
    bmp = tmp / f"{name}.bmp"
    env = {**os.environ, **env_extra}
    run([str(exe), "--shot", AT_SEC, str(bmp)], env=env)

    png = OUT / f"{name}.png"
    # -strip 去掉时间戳等元数据：否则内容没变的图每次也会产生 git diff。
    run(["magick", str(bmp), "-strip", str(png)])
    return png


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", help="只出匹配该子串的场景")
    args = ap.parse_args()

    if shutil.which("magick") is None:
        sys.exit("需要 ImageMagick（magick）——本项目的字体/图标生成也依赖它")

    exe = ensure_sim()
    OUT.mkdir(exist_ok=True)
    tmp = BUILD / "shots"
    tmp.mkdir(exist_ok=True)

    scenes = [s for s in SCENES if not args.only or args.only in s[0]]
    if not scenes:
        sys.exit(f"没有场景匹配 --only {args.only!r}")

    print(f"定格于 t={AT_SEC}s，共 {len(scenes)} 张 → {OUT.relative_to(REPO)}/\n")
    for name, env_extra, desc in scenes:
        png = capture(exe, name, env_extra, tmp)
        kb = png.stat().st_size / 1024
        print(f"  {name:24s} {kb:6.1f} KB   {desc}")

    print("\n完成。UI 改动后重跑本脚本，再用 git diff 看哪些场景受影响。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
