#!/usr/bin/env python3
"""统一的 host 测试入口 —— 不需要实板、不需要 ESP-IDF、不需要串口。

为什么需要它
------------
`firmware/test/` 下每个 .c 的文件头里都写着自己的编译命令，这套约定很好用
（判据和用法长在一起），但它意味着**没有一条命令能跑全部测试**：改一个共享
头文件后，要不要一个个手抄 36 条 cc 命令去复验，取决于当时记不记得。漏跑的
那一个，就是下次上板才发现的那一个。

这个脚本不发明新约定，只是把既有约定自动化：从每个测试的文件头注释里**解析
出它自己声明的第一条编译命令**，在仓库根目录执行，再跑出来的二进制。所以
测试文件仍然是自解释的，改编译命令也只需要改那一处。

用法
----
    python3 firmware/test/run_host_tests.py            # 全部
    python3 firmware/test/run_host_tests.py --list     # 只列出会跑什么
    python3 firmware/test/run_host_tests.py -k board   # 只跑名字含 board 的
    python3 firmware/test/run_host_tests.py --c-only   # 跳过 Python 测试

退出码：有任何 FAIL 就非 0。SKIP（缺第三方依赖）不算通过，会单独计数并在
汇总里点名，不会被混进 "全部通过"。

不在本入口范围内
----------------
`hardware/` 下的 KiCad 合同测试（test_component_contract.py 等）需要 kicad-cli
和 PCB 源，属于硬件侧，用
    /usr/bin/python3 -m pytest hardware/test_*.py
单独跑。其中 V3 的 `RP_1V1/C82/C83` 4 条失败是 2026-09-03 的产品豁免，
不要为了让某个入口"全绿"去删它们。
"""

from __future__ import annotations

import argparse
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
TEST_DIR = ROOT / "firmware" / "test"
PY_TEST_DIR = ROOT / "firmware" / "scripts"

# 文件头注释里第一条以 cc/gcc/clang 开头的命令就是"标准编译方式"。
# 后面若还有 ASan/UBSan 变体，是可选的加强跑法，不在默认入口里。
_CMD_START = re.compile(r"^\s*\*?\s*((?:cc|gcc|clang)\s.*)$")


def extract_compile_command(src: Path) -> str | None:
    """从 .c 的文件头注释里抠出第一条完整编译命令（处理反斜杠续行）。"""
    lines = src.read_text(encoding="utf-8").splitlines()
    parts: list[str] = []
    for line in lines[:80]:
        if not parts:
            m = _CMD_START.match(line)
            if not m:
                continue
            frag = m.group(1)
        else:
            # 续行：剥掉注释前缀 " * "
            frag = re.sub(r"^\s*\*?\s?", "", line)
        cont = frag.rstrip().endswith("\\")
        parts.append(frag.rstrip().rstrip("\\").strip())
        if not cont:
            break
    if not parts:
        return None
    return " ".join(p for p in parts if p)


def split_build_and_run(cmd: str) -> tuple[str, str | None]:
    """把 'cc ... -o /tmp/x a.c && /tmp/x' 拆成编译和运行两段。

    有的测试头里写了 `&& /tmp/x`，有的没写。统一拆开自己跑，这样编译失败和
    断言失败能分别报出来，而不是混成一个退出码。
    """
    build = cmd
    if "&&" in cmd:
        build = cmd.split("&&", 1)[0].strip()
    try:
        tokens = shlex.split(build)
    except ValueError:
        return build, None
    binary = None
    for i, tok in enumerate(tokens):
        if tok == "-o" and i + 1 < len(tokens):
            binary = tokens[i + 1]
    return build, binary


class Result:
    def __init__(self, name: str, status: str, secs: float, detail: str = ""):
        self.name, self.status, self.secs, self.detail = name, status, secs, detail


def run_c_tests(selector: str | None) -> list[Result]:
    results: list[Result] = []
    for src in sorted(TEST_DIR.glob("test_*.c")):
        if selector and selector not in src.name:
            continue
        t0 = time.monotonic()
        cmd = extract_compile_command(src)
        if cmd is None:
            results.append(Result(src.name, "SKIP", 0.0,
                                  "文件头里没有 cc/gcc/clang 编译命令"))
            continue
        build, binary = split_build_and_run(cmd)
        if binary is None:
            results.append(Result(src.name, "SKIP", 0.0,
                                  "编译命令里没有 -o <目标>，无法确定要跑哪个二进制"))
            continue
        cp = subprocess.run(build, shell=True, cwd=ROOT,
                            capture_output=True, text=True)
        if cp.returncode != 0:
            results.append(Result(src.name, "FAIL", time.monotonic() - t0,
                                  "编译失败：\n" + (cp.stderr or cp.stdout)[-2000:]))
            continue
        rp = subprocess.run([binary], cwd=ROOT, capture_output=True, text=True)
        dt = time.monotonic() - t0
        if rp.returncode == 0:
            results.append(Result(src.name, "PASS", dt))
        else:
            results.append(Result(src.name, "FAIL", dt,
                                  (rp.stdout or "")[-3000:] + (rp.stderr or "")[-1000:]))
    return results


def run_python_tests(selector: str | None) -> list[Result]:
    results: list[Result] = []
    for src in sorted(PY_TEST_DIR.glob("test_*.py")):
        if selector and selector not in src.name:
            continue
        t0 = time.monotonic()
        cp = subprocess.run([sys.executable, str(src)], cwd=ROOT,
                            capture_output=True, text=True)
        dt = time.monotonic() - t0
        out = (cp.stdout or "") + (cp.stderr or "")
        if cp.returncode == 0:
            results.append(Result(src.name, "PASS", dt))
        elif "ModuleNotFoundError" in out or "ImportError" in out:
            miss = re.search(r"No module named '([^']+)'", out)
            results.append(Result(src.name, "SKIP", dt,
                                  f"缺依赖 {miss.group(1) if miss else '(见输出)'}"))
        else:
            results.append(Result(src.name, "FAIL", dt, out[-3000:]))
    return results


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-k", "--only", metavar="SUBSTR",
                    help="只跑文件名含该子串的测试")
    ap.add_argument("--list", action="store_true", help="只列出会跑什么，不执行")
    ap.add_argument("--c-only", action="store_true", help="跳过 Python 测试")
    args = ap.parse_args()

    if args.list:
        for src in sorted(TEST_DIR.glob("test_*.c")):
            if args.only and args.only not in src.name:
                continue
            cmd = extract_compile_command(src)
            print(f"[C ] {src.name}\n     {cmd or '(无编译命令)'}")
        if not args.c_only:
            for src in sorted(PY_TEST_DIR.glob("test_*.py")):
                if args.only and args.only not in src.name:
                    continue
                print(f"[PY] {src.name}")
        return 0

    results = run_c_tests(args.only)
    if not args.c_only:
        results += run_python_tests(args.only)

    print()
    for r in results:
        mark = {"PASS": "PASS", "FAIL": "FAIL", "SKIP": "SKIP"}[r.status]
        line = f"  {mark}  {r.name:<38s} {r.secs:6.2f}s"
        if r.status == "SKIP" and r.detail:
            line += f"   ({r.detail})"
        print(line)

    failed = [r for r in results if r.status == "FAIL"]
    skipped = [r for r in results if r.status == "SKIP"]
    passed = [r for r in results if r.status == "PASS"]

    for r in failed:
        print(f"\n{'=' * 72}\nFAIL: {r.name}\n{'=' * 72}\n{r.detail}")

    print(f"\n合计 {len(results)}：通过 {len(passed)}，失败 {len(failed)}，"
          f"跳过 {len(skipped)}")
    if skipped:
        print("跳过的不算通过：" + "、".join(r.name for r in skipped))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
