#!/usr/bin/env python3
"""
check_demo_track_parity.py —— Python 与 C 两份轨迹处理实现的对拍。

为什么要有它
------------
演示轨迹现在有两条生产路径：

    PC 侧   gen_demo_track.py            → demo_track_data.c（编进固件的兜底轨迹）
    盒子侧  firmware/main/demo_gpx.c     → SD 卡上 GPX 现算的轨迹

同一份清洗逻辑写了两遍，这类"双实现"最典型的下场是**慢慢漂移**：有人在
Python 侧调了一个容差、修了一个边界，C 那边没跟上，于是"卡上放的轨迹"和
"内置的轨迹"对同一个 GPX 给出不同的读数，而且只在真机演示时才看得出来。

所以这里要求的不是"差不多"，是**逐点逐字段完全相等**。两处刻意的实现选择
就是为了让这个要求成立：
  - C 侧中间量全用 double（不是 float）；float 会让抽稀的容差判断在边界上
    分歧，保留点数直接对不上。
  - C 侧取整用 round-half-to-even，照抄 Python 内建 round() 的 banker's
    rounding；C 标准库的 round() 是 half-away-from-zero，x.5 上差 1。

容差
----
默认 --tol 0（要求零差异）。留这个开关是为了在**真出现**平台浮点差异时能量化
它有多大，而不是给"反正就差一点"开后门：一旦点数不同就直接判失败，容差只作用
在字段值上。经度 1e-7° ≈ 1.1 cm、航迹 0.1°、地速 1 kt —— 任何一个字段差 1 LSB
在屏上都看不出来，但差 1 LSB 也说明两边的浮点路径已经不同了，值得查。

用法
----
    python3 firmware/scripts/check_demo_track_parity.py <input.gpx>
    python3 firmware/scripts/check_demo_track_parity.py <input.gpx> --keep-cc

源 GPX 不随本仓库分发：需要对拍时把它的路径作为位置参数传进来，不传就跳过
并返回 0——本仓库单独 clone 时不该因此红掉。
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_demo_track as G  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
FW_MAIN = os.path.normpath(os.path.join(HERE, "..", "main"))
# 源 GPX 不随本仓库分发，默认留空；用位置参数传入路径即可对拍。
DEFAULT_GPX = ""

FIELDS = ("lat_e7", "lon_e7", "t_s", "alt_m", "roll_ddeg", "trk_ddeg", "gs_kt")

# C 侧的 dumper。刻意不放进 firmware/main：它只在对拍时存在，进了 main 就得
# 跟着固件一起编、一起维护一个永远不会被固件调用的 main()。
DUMP_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include "demo_gpx.h"
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    pk_demo_gpx_result_t r; const char *err = NULL;
    if (!pk_demo_gpx_load_file(argv[1], 0, NULL, &r, &err)) {
        fprintf(stderr, "%s\n", err ? err : "?"); return 1;
    }
    fprintf(stderr, "raw=%u kept=%u dur=%u trunc=%d\n",
            (unsigned)r.raw_n, (unsigned)r.n, (unsigned)r.dur_s, (int)r.truncated);
    for (unsigned i = 0; i < r.n; ++i)
        printf("%d,%d,%u,%d,%d,%u,%d\n", (int)r.pts[i].lat_e7, (int)r.pts[i].lon_e7,
               (unsigned)r.pts[i].t_s, (int)r.pts[i].alt_m, (int)r.pts[i].roll_ddeg,
               (unsigned)r.pts[i].trk_ddeg, (int)r.pts[i].gs_kt);
    free(r.pts);
    return 0;
}
"""


def python_rows(gpx: str) -> list[tuple[int, ...]]:
    """跑 Python 侧的完整管线，得到与 C 侧同构的整数行。

    刻意复用 gen_demo_track 里的函数而不是解析它生成的 .c 文本：解析文本会把
    "生成器的输出格式"也绑进对拍，格式一改对拍就假红。
    """
    raw = G.parse_gpx(gpx)
    G.derive(raw)
    kept = G.simplify(raw)
    t_col = G.quantize_time(kept)
    out = []
    for s, t_s in zip(kept, t_col):
        out.append((
            G._clampi(s.lat * 1e7, -2147483648, 2147483647),
            G._clampi(s.lon * 1e7, -2147483648, 2147483647),
            t_s,
            G._clampi(s.alt_m, -32768, 32767),
            G._clampi(s.roll_deg * 10.0, -32768, 32767),
            G._clampi(s.trk_deg * 10.0, 0, 3599) % 3600,
            G._clampi(s.gs_kt, -32768, 32767),
        ))
    return out


def c_rows(gpx: str, workdir: str) -> list[tuple[int, ...]]:
    src = os.path.join(workdir, "dump_demo_gpx.c")
    exe = os.path.join(workdir, "dump_demo_gpx")
    with open(src, "w", encoding="utf-8") as f:
        f.write(DUMP_C)
    cc = os.environ.get("CC", "cc")
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                    "-I", FW_MAIN, "-o", exe, src,
                    os.path.join(FW_MAIN, "demo_gpx.c"), "-lm"], check=True)
    r = subprocess.run([exe, gpx], check=True, capture_output=True, text=True)
    sys.stderr.write("  C : " + r.stderr.strip() + "\n")
    return [tuple(int(x) for x in line.split(","))
            for line in r.stdout.splitlines() if line.strip()]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("gpx", nargs="?", default=DEFAULT_GPX)
    ap.add_argument("--tol", type=int, default=0,
                    help="每字段允许的整数偏差（默认 0 = 要求逐字段相等）")
    ap.add_argument("--keep-cc", action="store_true", help="保留临时编译目录")
    args = ap.parse_args(argv)

    if not os.path.exists(args.gpx):
        print(f"SKIP: 找不到源 GPX {args.gpx!r}（不随本仓库分发，请用位置参数传入）")
        return 0

    work = tempfile.mkdtemp(prefix="demo_parity_")
    try:
        print(f"== demo_track parity: {os.path.basename(args.gpx)} ==")
        cr = c_rows(args.gpx, work)
        pr = python_rows(args.gpx)
        print(f"  py: kept={len(pr)}")

        if len(pr) != len(cr):
            print(f"\nFAILED: 点数不同 python={len(pr)} c={len(cr)}")
            print("  → 抽稀判据分歧（八成是某侧用了 float，或容差常量没同步）")
            return 1

        bad = 0
        for i, (p, c) in enumerate(zip(pr, cr)):
            for k, (pv, cv) in enumerate(zip(p, c)):
                if abs(pv - cv) > args.tol:
                    if bad < 20:
                        print(f"  [DIFF] #{i} {FIELDS[k]}: py={pv} c={cv}")
                    bad += 1
        if bad:
            print(f"\nFAILED: {bad} 处字段不一致（tol={args.tol}）")
            return 1
        print(f"\nALL PASS — {len(pr)} 点 × {len(FIELDS)} 字段逐一相等"
              f"（tol={args.tol}）")
        return 0
    finally:
        if args.keep_cc:
            print(f"  (临时目录保留在 {work})")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
