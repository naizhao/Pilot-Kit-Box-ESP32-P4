#!/usr/bin/env python3
"""
gen_demo_track.py —— 真实飞行 GPX → 演示模式的轨迹表（C 源码常量表）。

为什么要有这个脚本
------------------
演示模式（config_demo.c / demo_data.c）此前把本机**钉在北京上空一点不动**，
只有 ADS-B 假目标绕着它转。产品负责人的原话是「演示模式就是要移动才行」；
工程上还有一条更硬的理由：以本机位置为中心的滚动窗口（pk_win.c）与它的让路
规则 R1–R4，在本机静止时**永远走不到让路分支**——窗口一次填满之后再没有新格
要加载，`pk_win: status` 里的 loads/evicts/yields 就恒为 0，等于没测。本机一动、
跨过 1°×1° 网格边界，那条路径立刻可复现。

数据来源
--------
内部飞行记录导出的 GPX（1 Hz，含 lat/lon/ele/time）。**只取数据不取代码**。

输出格式（小端，ESP32-P4 native；直接生成 C 源码而不是二进制 blob）
------------------------------------------------------------------
生成 C 数组而不是 EMBED_FILES 的 .bin，理由和 demo_data.c 放在 firmware/main
是同一条：**模拟器要原样编译它**。EMBED_FILES 产出的 `_binary_*_start` 符号
只有 ESP-IDF 链接脚本才给，sim 那边链不上；而演示模式的截图回归恰恰跑在 sim 上。

每条记录 20 B（4 字节对齐，RISC-V 上不会踩非对齐访问）：

    int32_t  lat_e7      纬度 × 1e7
    int32_t  lon_e7      经度 × 1e7
    uint32_t t_s         距轨迹起点的**真实**秒数（不是相邻点增量：
                         回放要按时间二分查找，存增量就得先做前缀和，
                         而 demo_data 那条「无状态」硬约束不允许有 init）
    int16_t  alt_m       GPX <ele>，米（GPX 的高度是几何高，ft 换算在运行时做）
    int16_t  roll_ddeg   坡度 0.1°，由转弯率反算（见 derive_roll）
    uint16_t trk_ddeg    航迹真北 0.1°，0..3599
    int16_t  gs_kt       地速，节

**地速与航向在这里算好存进去，而不是运行时从相邻点现算**：抽稀之后相邻两点
可能隔几十秒，弦长/弦向与飞机当时的真实速度/航迹差得很远（转弯段尤其），
而这两个数是直接画在 PFD 上的。用原始 1 Hz 数据算完再抽稀，才是真值。
升降率反过来是运行时算的——它由高度差决定，抽稀的高度容差就是它的误差上限。

用法
----
    python3 firmware/scripts/gen_demo_track.py <input.gpx> \
        -o firmware/main/demo_track_data.c

    python3 -m unittest firmware.scripts.test_gen_demo_track   # 单测
"""

from __future__ import annotations

import argparse
import datetime as _dt
import math
import os
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from typing import Iterable, Sequence

# ── 抽稀容差 ────────────────────────────────────────────────────────────
# 原始 1 Hz 轨迹 6000+ 点 × 20 B = 120 KB，对一个演示功能来说太贵。
# 这几个容差取的是「屏上看不出差别」的量级：25 m 横向误差在最常用的 10 NM
# 量程上不到两个像素，10 m 高度误差换算到 VS 上不足 ±20 fpm。
# 实测（ZGGG-Full-Flight）6084 → 651 点 = 13 KB，离几十 KB 的预算还有余量，
# 所以宁可把容差压紧也不去省这点 flash——抽稀是不可逆的。
POS_TOL_M = 25.0        # 位置横向偏差
ALT_TOL_M = 10.0        # 高度偏差（也就是运行时算 VS 的误差上限）
GS_TOL_KT = 3.0         # 地速偏差
TRK_TOL_DEG = 1.0       # 航迹偏差
ROLL_TOL_DEG = 1.5      # 坡度偏差
MAX_SEG_S = 30.0        # 单段最长跨度：再长就算各项都在容差内也要插点，
                        # 否则一条几百秒的直线段会让"进出格"发生在插值中途，
                        # 而窗口是按 1 Hz 采本机位置的，跨度太大会跳格。

EARTH_R_M = 6371008.8
G_MS2 = 9.80665
KT_PER_MS = 1.943844
M_PER_FT = 0.3048

# 航迹角失效门槛：地速低于它就沿用上一点的航迹。
# 取 3 kt 而不是 0，与固件里 pk_own_heading_resolve() 的 2 kt 同一条道理——
# 停机坪上 GPS 位置噪声几十米就能让方位角在整圈里乱转，抽稀会忠实地把这堆
# 垃圾保下来，回放时 HSI 在开场十几秒里疯转。
TRK_MIN_GS_KT = 3.0

# 高度中值滤波窗口（原始采样点数，取奇数）。
# 对付**孤立**的单点高度毛刺。中值而不是均值：均值会把尖刺抹平成一段两倍宽
# 的假爬升，中值直接丢弃它。
ALT_MEDIAN_N = 5

# 高度变化率上限，m/s（4000 fpm）。
#
# 这条源轨迹里有两处**台阶**而不是尖刺：t≈925 s（起飞后）一次 2 s 内 +439 m，
# t≈7431 s（下降中）一次 1 s 内 −84 m，而且台阶之后的高度就一直停在新的
# 基准上——是记录跨段拼接时换了高度基准，不是毛刺。中值滤波对台阶无能为力
# （中值本来就是保边缘的）。
#
# 处理方式是**限速跟随**：高度每秒最多变 20.3 m，超出的部分留到后面几秒补。
# 439 m 的台阶因此摊成约 22 s 的 4000 fpm 爬升——喷气机刚离地时本来就是这个
# 量级，看上去毫无破绽；限速器在追平原始曲线之后自动不再起作用，所以航段
# 其余部分与落地高度一个字节都没改。
#
# 不选"把台阶后面整段减去 439 m"：那会让终点落在比 ZBAA 停机坪低 439 m 的
# 地方，等于用一个正确的开头换一个错误的结尾。
ALT_MAX_RATE_MPS = 20.32


@dataclass
class Sample:
    """原始采样点 + 派生量。角度单位一律为度。"""
    t_s: float
    lat: float
    lon: float
    alt_m: float
    trk_deg: float = 0.0
    gs_kt: float = 0.0
    roll_deg: float = 0.0


# ── GPX 解析 ────────────────────────────────────────────────────────────
def _parse_time(text: str) -> _dt.datetime:
    """GPX 的时间写法不止一种。

    flyGarmin 导出的是 `2020-12-16T05:24:34+0000`（无冒号偏移），
    别的工具给 `...Z`。fromisoformat 在 3.11 之前两种都不吃，这里统一成
    `+00:00` 再交给它，免得为一个日期字串引入 dateutil 依赖。
    """
    s = text.strip()
    if s.endswith("Z"):
        s = s[:-1] + "+00:00"
    elif len(s) >= 5 and s[-5] in "+-" and s[-3] != ":":
        s = s[:-2] + ":" + s[-2:]
    return _dt.datetime.fromisoformat(s)


def parse_gpx(path_or_text: str, *, is_text: bool = False) -> list[Sample]:
    """读 GPX 轨迹点。命名空间用 `{*}` 通配——GPX 1.0 / 1.1 的 xmlns 不同，
    写死 1.1 会在 1.0 的文件上静默返回 0 个点。"""
    root = ET.fromstring(path_or_text) if is_text else ET.parse(path_or_text).getroot()
    pts = root.findall(".//{*}trkpt")
    out: list[Sample] = []
    t0: _dt.datetime | None = None
    for p in pts:
        lat_a, lon_a = p.get("lat"), p.get("lon")
        if lat_a is None or lon_a is None:
            continue
        ele_e = p.find("{*}ele")
        time_e = p.find("{*}time")
        if time_e is None or not (time_e.text or "").strip():
            # 没有时间戳就没法定义回放速度，直接放弃这个点而不是猜一个 dt。
            continue
        t = _parse_time(time_e.text)
        if t0 is None:
            t0 = t
        alt = float(ele_e.text) if (ele_e is not None and ele_e.text) else 0.0
        out.append(Sample(t_s=(t - t0).total_seconds(),
                          lat=float(lat_a), lon=float(lon_a), alt_m=alt))
    # 时间必须单调递增：有的记录器在跨段拼接处会回跳一两秒，回放的二分查找
    # 建立在单调之上，不筛掉的话查出来的段是乱的。
    mono: list[Sample] = []
    for s in out:
        if mono and s.t_s <= mono[-1].t_s:
            continue
        mono.append(s)
    return mono


# ── 几何 ────────────────────────────────────────────────────────────────
def bearing_deg(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """真北航迹角，0..360。"""
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dl = math.radians(lon2 - lon1)
    y = math.sin(dl) * math.cos(p2)
    x = math.cos(p1) * math.sin(p2) - math.sin(p1) * math.cos(p2) * math.cos(dl)
    return math.degrees(math.atan2(y, x)) % 360.0


def dist_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """等距圆柱近似。本轨迹相邻点最远也就几十公里，误差远小于 POS_TOL_M。"""
    dlat = math.radians(lat2 - lat1)
    dlon = math.radians(lon2 - lon1) * math.cos(math.radians((lat1 + lat2) * 0.5))
    return math.hypot(dlat, dlon) * EARTH_R_M


def wrap180(deg: float) -> float:
    return (deg + 180.0) % 360.0 - 180.0


def lerp_angle(a: float, b: float, u: float) -> float:
    """沿短弧插值，结果归一到 0..360。359° 与 1° 之间必须走 2° 那条路，
    线性插值会绕 358°，抽稀判据就会把一条直线判成剧烈转弯。"""
    return (a + wrap180(b - a) * u) % 360.0


# ── 派生量 ──────────────────────────────────────────────────────────────
def derive(samples: list[Sample],
           *, trk_span_s: float = 4.0, roll_smooth_s: float = 6.0) -> list[Sample]:
    """在**原始**采样率上算航迹/地速/坡度，之后才抽稀。

    航迹用一个 ±trk_span_s 的跨度算而不是相邻两点：1 Hz 下相邻点相距不到
    250 m，GPS 位置噪声几十米就能让方位角抖上好几度，直接拿去反算坡度会得到
    一串 ±30° 的假横滚。
    """
    n = len(samples)
    if n == 0:
        return samples
    if n == 1:
        return samples

    despike_alt(samples)

    for i, s in enumerate(samples):
        j = i
        while j > 0 and samples[i].t_s - samples[j].t_s < trk_span_s:
            j -= 1
        k = i
        while k < n - 1 and samples[k].t_s - samples[i].t_s < trk_span_s:
            k += 1
        if j == k:
            j, k = max(0, i - 1), min(n - 1, i + 1)
        a, b = samples[j], samples[k]
        dt = b.t_s - a.t_s
        d = dist_m(a.lat, a.lon, b.lat, b.lon)
        if dt > 0:
            s.gs_kt = d / dt * KT_PER_MS
        if s.gs_kt >= TRK_MIN_GS_KT and d > 1.0:
            s.trk_deg = bearing_deg(a.lat, a.lon, b.lat, b.lon)
        elif i > 0:
            # 几乎没动时方位角是纯噪声，沿用上一点，别让它乱转。
            s.trk_deg = samples[i - 1].trk_deg

    # 开场那段还停在廊桥上，上面的循环让它们全沿用了初值 0°，于是飞机一动
    # 就有一次 0° → 跑道方向的跳变。用第一个有效航迹回填，把跳变消掉。
    first = next((s.trk_deg for s in samples if s.gs_kt >= TRK_MIN_GS_KT), None)
    if first is not None:
        for s in samples:
            if s.gs_kt >= TRK_MIN_GS_KT:
                break
            s.trk_deg = first

    _derive_roll(samples, roll_smooth_s)
    return samples


def despike_alt(samples: list[Sample]) -> None:
    """中值去毛刺 + 限速跟随去台阶，就地改写 Sample.alt_m。
    见 ALT_MEDIAN_N / ALT_MAX_RATE_MPS 的说明。"""
    n = len(samples)
    if n < 2:
        return
    if n >= ALT_MEDIAN_N:
        half = ALT_MEDIAN_N // 2
        src = [s.alt_m for s in samples]
        for i in range(n):
            # 端点处用**对称收缩**的窗口（末点 half=0，原值直通）。
            # 若像常见写法那样只把窗口截断成 [n-3, n-1]，在爬升段上末点会被
            # 换成倒数第二点的值——首末两点恰好是轨迹的起降高度基准，
            # 不能因为滤波的边界处理而被改掉。
            h = min(half, i, n - 1 - i)
            w = sorted(src[i - h:i + h + 1])
            samples[i].alt_m = w[len(w) // 2]

    for i in range(1, n):
        dt = samples[i].t_s - samples[i - 1].t_s
        if dt <= 0:
            continue
        lim = ALT_MAX_RATE_MPS * dt
        prev = samples[i - 1].alt_m
        samples[i].alt_m = max(prev - lim, min(prev + lim, samples[i].alt_m))


def _derive_roll(samples: list[Sample], smooth_s: float) -> None:
    """坡度由协调转弯反算：tan(φ) = V·ω / g。

    直接拿它当 PFD 的横滚是合理的——真实航线飞行里除了阵风修正，坡度基本
    就是这个值。上限钳在 ±30°：民航自动驾驶的常用限制就是 25–30°，超出的
    一定是位置噪声放大出来的，不是真的。
    """
    n = len(samples)
    raw = [0.0] * n
    for i in range(n):
        j, k = max(0, i - 1), min(n - 1, i + 1)
        dt = samples[k].t_s - samples[j].t_s
        if dt <= 0:
            continue
        omega = math.radians(wrap180(samples[k].trk_deg - samples[j].trk_deg)) / dt
        v = samples[i].gs_kt / KT_PER_MS
        raw[i] = math.degrees(math.atan2(v * omega, G_MS2))

    for i in range(n):
        j = i
        while j > 0 and samples[i].t_s - samples[j].t_s < smooth_s:
            j -= 1
        k = i
        while k < n - 1 and samples[k].t_s - samples[i].t_s < smooth_s:
            k += 1
        avg = sum(raw[j:k + 1]) / float(k - j + 1)
        samples[i].roll_deg = max(-30.0, min(30.0, avg))


# ── 抽稀 ────────────────────────────────────────────────────────────────
def simplify(samples: Sequence[Sample]) -> list[Sample]:
    """贪心保点：从上一个保留点起向前伸，只要**中间任意一点**的线性插值误差
    超过任一容差，就把前一点定下来。

    比 Douglas-Peucker 笨，但它天然能把位置/高度/速度/航迹/坡度五个量一起
    管住（DP 只管一个量，五个量就要跑五遍再求并集），而且是一遍 O(n·k)。
    """
    n = len(samples)
    if n <= 2:
        return list(samples)
    keep = [samples[0]]
    anchor = 0
    i = 1
    while i < n - 1:
        if _within_tol(samples, anchor, i + 1):
            i += 1
            continue
        keep.append(samples[i])
        anchor = i
        i += 1
    keep.append(samples[n - 1])
    return keep


def _within_tol(s: Sequence[Sample], a: int, b: int) -> bool:
    """a→b 直接连一段，中间每个点的插值误差是否都在容差内。"""
    sa, sb = s[a], s[b]
    span = sb.t_s - sa.t_s
    if span > MAX_SEG_S:
        return False
    if span <= 0:
        return True
    for m in range(a + 1, b):
        p = s[m]
        u = (p.t_s - sa.t_s) / span
        lat = sa.lat + (sb.lat - sa.lat) * u
        lon = sa.lon + (sb.lon - sa.lon) * u
        if dist_m(lat, lon, p.lat, p.lon) > POS_TOL_M:
            return False
        if abs(sa.alt_m + (sb.alt_m - sa.alt_m) * u - p.alt_m) > ALT_TOL_M:
            return False
        if abs(sa.gs_kt + (sb.gs_kt - sa.gs_kt) * u - p.gs_kt) > GS_TOL_KT:
            return False
        if abs(wrap180(lerp_angle(sa.trk_deg, sb.trk_deg, u) - p.trk_deg)) > TRK_TOL_DEG:
            return False
        if abs(sa.roll_deg + (sb.roll_deg - sa.roll_deg) * u - p.roll_deg) > ROLL_TOL_DEG:
            return False
    return True


# ── C 源码输出 ──────────────────────────────────────────────────────────
REC_BYTES = 20


def _clampi(v: float, lo: int, hi: int) -> int:
    return max(lo, min(hi, int(round(v))))


def quantize_time(samples: Sequence[Sample]) -> list[int]:
    """把 t_s 落成**严格递增**的整秒。

    表里的时间是 uint32 秒，而原始 GPX 有 1.8 s 这种非整秒采样，四舍五入之后
    相邻两点可能撞成同一个整数。回放的 seek() 是二分查找，前提就是严格递增；
    撞了之后那一段的插值分母为 0，屏上表现为某一瞬间位置/姿态定住不动。
    冲突时把后一点顶到 prev+1 而不是丢掉它：丢点会丢掉一次真实的机动，
    而顶 1 秒引入的时间误差上限就是 1 s（10 倍速下是 0.1 墙钟秒）。
    """
    out: list[int] = []
    prev = -1
    for s in samples:
        t = int(round(s.t_s))
        if t <= prev:
            t = prev + 1
        out.append(t)
        prev = t
    return out


def emit_c(samples: Sequence[Sample], src_name: str, raw_n: int) -> str:
    n = len(samples)
    t_col = quantize_time(samples)
    dur = t_col[-1] if n else 0
    lat0, lon0 = (samples[0].lat, samples[0].lon) if n else (0.0, 0.0)
    latN, lonN = (samples[-1].lat, samples[-1].lon) if n else (0.0, 0.0)
    lines = [
        "/*",
        " * demo_track_data.c —— 演示模式的真实飞行轨迹表。",
        " *",
        " * **本文件由 firmware/scripts/gen_demo_track.py 生成，不要手改。**",
        " *",
        f" * 源：内部飞行记录导出的 GPX（{src_name}）",
        f" * 原始点 {raw_n} → 抽稀后 {n}（容差：位置 {POS_TOL_M:.0f} m / 高度 "
        f"{ALT_TOL_M:.0f} m / 地速 {GS_TOL_KT:.0f} kt / 航迹 {TRK_TOL_DEG:.0f}° "
        f"/ 坡度 {ROLL_TOL_DEG:.1f}°）",
        f" * 真实时长 {dur} s，表体 {n * REC_BYTES} B",
        f" * 起点 {lat0:.4f},{lon0:.4f} → 终点 {latN:.4f},{lonN:.4f}",
        " */",
        '#include "demo_track.h"',
        "",
        "const pk_demo_track_pt_t pk_demo_track[] = {",
    ]
    for s, t_s in zip(samples, t_col):
        lines.append(
            "    {%d,%d,%u,%d,%d,%u,%d}," % (
                _clampi(s.lat * 1e7, -2147483648, 2147483647),
                _clampi(s.lon * 1e7, -2147483648, 2147483647),
                t_s,
                _clampi(s.alt_m, -32768, 32767),
                _clampi(s.roll_deg * 10.0, -32768, 32767),
                _clampi(s.trk_deg * 10.0, 0, 3599) % 3600,
                _clampi(s.gs_kt, -32768, 32767),
            ))
    lines += [
        "};",
        "",
        f"const uint32_t pk_demo_track_n        = {n};",
        f"const uint32_t pk_demo_track_dur_s    = {dur};",
        "",
    ]
    return "\n".join(lines)


def main(argv: Sequence[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("gpx")
    ap.add_argument("-o", "--out", required=True)
    args = ap.parse_args(argv)

    raw = parse_gpx(args.gpx)
    if len(raw) < 2:
        print(f"error: {args.gpx} 里可用轨迹点不足（{len(raw)}）", file=sys.stderr)
        return 1
    derive(raw)
    kept = simplify(raw)
    text = emit_c(kept, os.path.basename(args.gpx), len(raw))
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(text)

    span = kept[-1].t_s
    print(f"{args.gpx}: {len(raw)} → {len(kept)} 点 "
          f"({len(kept) * REC_BYTES} B = {len(kept) * REC_BYTES / 1024.0:.1f} KB), "
          f"时长 {span:.0f} s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
