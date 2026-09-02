#!/usr/bin/env python3
"""与 KiCad 界面语言无关的 DRC 项目分类 helper。"""

import re


_KIND_MARKERS = (
    ("zone", ("zone", "填充区")),
    ("pad", ("pad", "焊盘")),
    ("via", ("via", "过孔")),
    ("track", ("track", "走线")),
)


def description_kind(description):
    normalized = description.casefold()
    for kind, markers in _KIND_MARKERS:
        if any(marker in normalized for marker in markers):
            return kind
    return "unknown"


def extract_net_name(item):
    for entry in item.get("items", ()):
        match = re.search(r"\[([^\]]*)\]", entry.get("description", ""))
        if match:
            return match.group(1)
    return "?"


def extract_layer_name(description):
    patterns = (
        r"\bon ([A-Za-z0-9.]+)",
        r"^([A-Za-z0-9.]+) 上",
        r"\(([A-Za-z0-9.]+)(?:\s*-\s*[A-Za-z0-9.]+)?\)",
        r"在 ([A-Za-z0-9.]+) 上",
    )
    for pattern in patterns:
        match = re.search(pattern, description)
        if match:
            return match.group(1)
    return "F.Cu"


def classify_unconnected_item(item):
    kinds = {
        description_kind(entry.get("description", ""))
        for entry in item.get("items", ())
    }
    if "zone" in kinds:
        return "plane"
    if "pad" in kinds:
        return "need"
    return "orphan"


def drc_failure_summary(report):
    """返回电气 DRC 的失败摘要；warning 保留展示，但不阻断制造验收。"""
    failures = []
    violation_count = sum(
        1 for item in report.get("violations", ())
        if item.get("severity", "error") == "error"
    )
    unconnected_count = len(report.get("unconnected_items", ()))
    if violation_count:
        failures.append(f"DRC 错误 {violation_count} 项")
    if unconnected_count:
        failures.append(f"未连通 {unconnected_count} 项")
    return "；".join(failures)
