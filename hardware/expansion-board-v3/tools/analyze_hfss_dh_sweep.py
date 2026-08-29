#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""分析 IFA D/H HFSS 扫参导出的复数 S11 CSV。

输入数据必须包含频率、S11 实部和 S11 虚部。脚本在 50Ω 参考面换算
阻抗与 SWR，并提取：

* 指定频率（默认 1090MHz）的复数 S11、阻抗和 SWR；
* CSV 采样点中的 S11 最低点；
* 0.98–1.20GHz 内、插值电阻低于 100Ω 的主模态 X=0 交叉。

文件名中的 ``ifa_direct_<L>..._d<D>_h<H>_...`` 会解析成毫米参数；
历史 CSV 没有 D/H token 时，相应字段输出 null/空值。
"""

import argparse
import csv
import json
import math
import pathlib
import re
import sys
from typing import NamedTuple


REFERENCE_IMPEDANCE_OHM = 50.0
DEFAULT_TARGET_FREQUENCY_MHZ = 1090.0
MAIN_MODE_MIN_MHZ = 980.0
MAIN_MODE_MAX_MHZ = 1200.0
MAIN_MODE_MAX_RESISTANCE_OHM = 100.0


class S11Sample(NamedTuple):
    frequency_mhz: float
    s11: complex
    impedance: complex
    swr: float
    s11_db: float


def _decode_filename_number(token):
    """把 HFSS 文件名中的 50p20 还原成 50.20。"""
    return float(token.replace("p", "."))


def parse_case_parameters(filename):
    """从 HFSS 文件名解析主臂 L、两腿间距 D 和脚长 H（单位 mm）。"""
    name = pathlib.Path(filename).name
    length_match = re.search(r"ifa_direct_(\d+(?:p\d+)?)(?:_|$)", name)
    d_match = re.search(r"(?:^|_)d(\d+(?:p\d+)?)(?:_|$)", name)
    h_match = re.search(r"(?:^|_)h(\d+(?:p\d+)?)(?:_|$)", name)
    return {
        "length_mm": _decode_filename_number(length_match.group(1)) if length_match else None,
        "d_mm": _decode_filename_number(d_match.group(1)) if d_match else None,
        "h_mm": _decode_filename_number(h_match.group(1)) if h_match else None,
    }


def s11_to_impedance(s11, reference_impedance_ohm=REFERENCE_IMPEDANCE_OHM):
    """按 Z=Z0(1+Γ)/(1-Γ) 把复数反射系数换算为阻抗。"""
    denominator = 1.0 - s11
    if abs(denominator) < 1e-15:
        return complex(math.inf, math.inf)
    return reference_impedance_ohm * (1.0 + s11) / denominator


def s11_to_swr(s11):
    """由复数反射系数计算 SWR；|Γ|>=1 时返回无穷大。"""
    magnitude = abs(s11)
    if magnitude >= 1.0:
        return math.inf
    return (1.0 + magnitude) / (1.0 - magnitude)


def s11_to_db(s11):
    """把复数反射系数换算为 dB。"""
    magnitude = abs(s11)
    if magnitude == 0.0:
        return -math.inf
    return 20.0 * math.log10(magnitude)


def _find_column(fieldnames, predicate, description):
    for fieldname in fieldnames or ():
        normalized = fieldname.strip().lower()
        if predicate(normalized):
            return fieldname
    raise ValueError("CSV 缺少%s列；实际列为：%s" % (description, ", ".join(fieldnames or ())))


def _frequency_scale_to_mhz(header):
    normalized = header.lower()
    if "ghz" in normalized:
        return 1000.0
    if "mhz" in normalized:
        return 1.0
    if "khz" in normalized:
        return 0.001
    if "hz" in normalized:
        return 0.000001
    raise ValueError("频率列必须在表头标明 GHz、MHz、kHz 或 Hz：%s" % header)


def read_s11_csv(source):
    """读取 HFSS 复数 S11 CSV；source 可为路径或已打开的文本流。"""
    should_close = False
    if hasattr(source, "read"):
        stream = source
    else:
        stream = pathlib.Path(source).open("r", encoding="utf-8-sig", newline="")
        should_close = True

    try:
        reader = csv.DictReader(stream)
        frequency_column = _find_column(
            reader.fieldnames,
            lambda value: value.startswith("freq"),
            "频率",
        )
        real_column = _find_column(
            reader.fieldnames,
            lambda value: value.startswith("re(") and "port1" in value,
            "S11 实部",
        )
        imag_column = _find_column(
            reader.fieldnames,
            lambda value: value.startswith("im(") and "port1" in value,
            "S11 虚部",
        )
        frequency_scale = _frequency_scale_to_mhz(frequency_column)
        samples = []
        for line_number, row in enumerate(reader, start=2):
            try:
                frequency_mhz = float(row[frequency_column]) * frequency_scale
                s11 = complex(float(row[real_column]), float(row[imag_column]))
            except (TypeError, ValueError) as exc:
                raise ValueError("CSV 第%d行不是有效数值：%s" % (line_number, exc)) from exc
            samples.append(
                S11Sample(
                    frequency_mhz=frequency_mhz,
                    s11=s11,
                    impedance=s11_to_impedance(s11),
                    swr=s11_to_swr(s11),
                    s11_db=s11_to_db(s11),
                )
            )
    finally:
        if should_close:
            stream.close()

    if len(samples) < 2:
        raise ValueError("CSV 至少需要两个频率采样点")
    samples.sort(key=lambda sample: sample.frequency_mhz)
    for left, right in zip(samples, samples[1:]):
        if left.frequency_mhz == right.frequency_mhz:
            raise ValueError("CSV 含重复频率 %.9gMHz" % left.frequency_mhz)
    return samples


def _linear_fraction(left_frequency, right_frequency, target_frequency):
    return (target_frequency - left_frequency) / (right_frequency - left_frequency)


def _interpolate_at_frequency(samples, target_frequency_mhz):
    if not samples[0].frequency_mhz <= target_frequency_mhz <= samples[-1].frequency_mhz:
        raise ValueError(
            "目标频率 %.3fMHz 超出 CSV 范围 %.3f–%.3fMHz"
            % (target_frequency_mhz, samples[0].frequency_mhz, samples[-1].frequency_mhz)
        )
    for sample in samples:
        if sample.frequency_mhz == target_frequency_mhz:
            return sample
    for left, right in zip(samples, samples[1:]):
        if left.frequency_mhz < target_frequency_mhz < right.frequency_mhz:
            fraction = _linear_fraction(
                left.frequency_mhz, right.frequency_mhz, target_frequency_mhz
            )
            s11 = left.s11 + fraction * (right.s11 - left.s11)
            return S11Sample(
                frequency_mhz=target_frequency_mhz,
                s11=s11,
                impedance=s11_to_impedance(s11),
                swr=s11_to_swr(s11),
                s11_db=s11_to_db(s11),
            )
    raise AssertionError("排序后的频率区间未覆盖目标频率")


def _zero_crossing(left, right):
    left_x = left.impedance.imag
    right_x = right.impedance.imag
    if left_x == 0.0:
        fraction = 0.0
    elif right_x == 0.0:
        fraction = 1.0
    elif left_x * right_x > 0.0:
        return None
    else:
        fraction = -left_x / (right_x - left_x)

    frequency_mhz = left.frequency_mhz + fraction * (
        right.frequency_mhz - left.frequency_mhz
    )
    resistance_ohm = left.impedance.real + fraction * (
        right.impedance.real - left.impedance.real
    )
    return frequency_mhz, resistance_ohm


def _find_main_mode(samples):
    candidates = []
    seen_frequencies = set()
    for left, right in zip(samples, samples[1:]):
        crossing = _zero_crossing(left, right)
        if crossing is None:
            continue
        frequency_mhz, resistance_ohm = crossing
        rounded_frequency = round(frequency_mhz, 9)
        if rounded_frequency in seen_frequencies:
            continue
        seen_frequencies.add(rounded_frequency)
        if not MAIN_MODE_MIN_MHZ <= frequency_mhz <= MAIN_MODE_MAX_MHZ:
            continue
        if not 0.0 < resistance_ohm < MAIN_MODE_MAX_RESISTANCE_OHM:
            continue
        candidates.append((frequency_mhz, resistance_ohm))

    if not candidates:
        raise ValueError(
            "在 %.0f–%.0fMHz 内没有找到 R<%.0fΩ 的 X=0 主模态"
            % (MAIN_MODE_MIN_MHZ, MAIN_MODE_MAX_MHZ, MAIN_MODE_MAX_RESISTANCE_OHM)
        )

    # D/H 扫参的目标是 1090MHz；若出现多个低阻交叉，取最接近目标的一个。
    return min(
        candidates,
        key=lambda candidate: abs(candidate[0] - DEFAULT_TARGET_FREQUENCY_MHZ),
    )


def _sample_dict(sample):
    return {
        "frequency_mhz": sample.frequency_mhz,
        "s11_db": sample.s11_db,
        "s11_real": sample.s11.real,
        "s11_imag": sample.s11.imag,
        "swr": sample.swr,
        "z_real_ohm": sample.impedance.real,
        "z_imag_ohm": sample.impedance.imag,
    }


def analyze_samples(samples, target_frequency_mhz=DEFAULT_TARGET_FREQUENCY_MHZ):
    """从一组 S11 采样中提取目标频点、S11 最低点和目标主模态。"""
    target_sample = _interpolate_at_frequency(samples, target_frequency_mhz)
    minimum_sample = min(samples, key=lambda sample: abs(sample.s11))
    resonance_frequency_mhz, resonance_resistance_ohm = _find_main_mode(samples)
    return {
        "at_target": _sample_dict(target_sample),
        "s11_minimum": _sample_dict(minimum_sample),
        "series_resonance": {
            "frequency_mhz": resonance_frequency_mhz,
            "resistance_ohm": resonance_resistance_ohm,
            "reactance_ohm": 0.0,
        },
    }


def analyze_file(path, target_frequency_mhz=DEFAULT_TARGET_FREQUENCY_MHZ):
    """分析单个 HFSS CSV，并附加源文件和文件名参数。"""
    input_path = pathlib.Path(path)
    result = analyze_samples(read_s11_csv(input_path), target_frequency_mhz)
    return {
        "source": str(input_path),
        "parameters": parse_case_parameters(input_path.name),
        **result,
    }


def write_json_summary(results, output_path):
    """把一个或多个分析结果写为 JSON。"""
    pathlib.Path(output_path).write_text(
        json.dumps(results, ensure_ascii=False, indent=2, allow_nan=False) + "\n",
        encoding="utf-8",
    )


CSV_COLUMNS = (
    "source",
    "length_mm",
    "d_mm",
    "h_mm",
    "target_frequency_mhz",
    "target_s11_db",
    "target_s11_real",
    "target_s11_imag",
    "target_swr",
    "target_z_real_ohm",
    "target_z_imag_ohm",
    "s11_minimum_frequency_mhz",
    "s11_minimum_db",
    "s11_minimum_swr",
    "s11_minimum_z_real_ohm",
    "s11_minimum_z_imag_ohm",
    "series_resonance_frequency_mhz",
    "series_resonance_resistance_ohm",
)


def _flatten_result(result):
    parameters = result["parameters"]
    target = result["at_target"]
    minimum = result["s11_minimum"]
    resonance = result["series_resonance"]
    return {
        "source": result["source"],
        "length_mm": parameters["length_mm"],
        "d_mm": parameters["d_mm"],
        "h_mm": parameters["h_mm"],
        "target_frequency_mhz": target["frequency_mhz"],
        "target_s11_db": target["s11_db"],
        "target_s11_real": target["s11_real"],
        "target_s11_imag": target["s11_imag"],
        "target_swr": target["swr"],
        "target_z_real_ohm": target["z_real_ohm"],
        "target_z_imag_ohm": target["z_imag_ohm"],
        "s11_minimum_frequency_mhz": minimum["frequency_mhz"],
        "s11_minimum_db": minimum["s11_db"],
        "s11_minimum_swr": minimum["swr"],
        "s11_minimum_z_real_ohm": minimum["z_real_ohm"],
        "s11_minimum_z_imag_ohm": minimum["z_imag_ohm"],
        "series_resonance_frequency_mhz": resonance["frequency_mhz"],
        "series_resonance_resistance_ohm": resonance["resistance_ohm"],
    }


def write_csv_summary(results, output_path):
    """把一个或多个分析结果写为便于表格比较的扁平 CSV。"""
    with pathlib.Path(output_path).open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=CSV_COLUMNS, lineterminator="\n")
        writer.writeheader()
        for result in results:
            writer.writerow(_flatten_result(result))


def build_argument_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=pathlib.Path, help="一个或多个 HFSS S11 CSV")
    parser.add_argument(
        "--target-mhz",
        type=float,
        default=DEFAULT_TARGET_FREQUENCY_MHZ,
        help="需要插值提取的目标频率，默认 1090MHz",
    )
    parser.add_argument("--json", type=pathlib.Path, help="JSON 摘要输出路径")
    parser.add_argument("--csv", type=pathlib.Path, help="CSV 摘要输出路径")
    return parser


def main(argv=None):
    args = build_argument_parser().parse_args(argv)
    try:
        results = [analyze_file(path, args.target_mhz) for path in args.inputs]
        if args.json:
            write_json_summary(results, args.json)
        if args.csv:
            write_csv_summary(results, args.csv)
        if not args.json and not args.csv:
            json.dump(results, sys.stdout, ensure_ascii=False, indent=2, allow_nan=False)
            sys.stdout.write("\n")
    except (OSError, ValueError) as exc:
        print("错误：%s" % exc, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
