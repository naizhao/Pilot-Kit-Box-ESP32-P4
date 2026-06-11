#!/usr/bin/env python3
"""Single source of truth for built-in UI text.

The firmware keeps this catalog embedded for now, while the generated
interfaces are shaped so a future LittleFS/microSD language-pack backend can
replace the lookup implementation without touching render callers.
"""

from __future__ import annotations

LANGS = ("en", "zh")

STRINGS = [
    (
        "SETTINGS_TITLE",
        {
            "en": "SETTINGS",
            "zh": "设置",
        },
    ),
    (
        "SETTINGS_LANGUAGE",
        {
            "en": "Language",
            "zh": "语言",
        },
    ),
    (
        "LANG_ENGLISH",
        {
            "en": "English",
            "zh": "英文",
        },
    ),
    (
        "LANG_CHINESE",
        {
            "en": "Chinese",
            "zh": "中文",
        },
    ),
    (
        "SETTINGS_FOOTER",
        {
            "en": "MODE page  UP/DOWN row  TARE change",
            "zh": "MODE 页  UP/DOWN 选  TARE 改",
        },
    ),
    (
        "ABOUT_TITLE",
        {
            "en": "ABOUT",
            "zh": "关于",
        },
    ),
    (
        "ABOUT_PROJECT",
        {
            "en": "Project",
            "zh": "项目",
        },
    ),
    (
        "ABOUT_VERSION",
        {
            "en": "Version",
            "zh": "版本",
        },
    ),
    (
        "ABOUT_BUILD",
        {
            "en": "Build",
            "zh": "构建",
        },
    ),
    (
        "ABOUT_IDF",
        {
            "en": "ESP-IDF",
            "zh": "ESP-IDF",
        },
    ),
    (
        "ABOUT_BOARD",
        {
            "en": "Board",
            "zh": "开发板",
        },
    ),
    (
        "ABOUT_CHIP",
        {
            "en": "Chip rev",
            "zh": "芯片",
        },
    ),
    (
        "ABOUT_DISPLAY",
        {
            "en": "Display",
            "zh": "屏幕",
        },
    ),
    (
        "ABOUT_IMU",
        {
            "en": "IMU",
            "zh": "姿态",
        },
    ),
    (
        "ABOUT_DONGLE",
        {
            "en": "Dongle",
            "zh": "接收机",
        },
    ),
    (
        "ABOUT_CAL",
        {
            "en": "IMU cal",
            "zh": "校准",
        },
    ),
    (
        "ABOUT_HINT_CONVERGED",
        {
            "en": "Fusion converged.",
            "zh": "融合已收敛",
        },
    ),
    (
        "ABOUT_HINT_CONVERGING",
        {
            "en": "Converging - keep moving",
            "zh": "正在收敛 - 继续移动",
        },
    ),
    (
        "ABOUT_HINT_FIG8",
        {
            "en": "Move in figure-8 to calibrate",
            "zh": "画 8 字校准",
        },
    ),
    (
        "ABOUT_FOOTER",
        {
            "en": "MODE cycle    UP/DOWN scroll",
            "zh": "MODE 切换    UP/DOWN 滚动",
        },
    ),
    (
        "CAL_TITLE",
        {
            "en": "COMPASS CAL",
            "zh": "罗盘校准",
        },
    ),
    (
        "CAL_LINE1",
        {
            "en": "Move device in a figure-8",
            "zh": "画 8 字移动设备",
        },
    ),
    (
        "CAL_LINE2",
        {
            "en": "rotating in all directions",
            "zh": "各方向旋转",
        },
    ),
    (
        "CAL_QUALITY",
        {
            "en": "Quality",
            "zh": "质量",
        },
    ),
    (
        "CAL_FOOTER",
        {
            "en": "Press MODE to skip",
            "zh": "按 MODE 跳过",
        },
    ),
    # --- 瞬时屏幕提示(toast) — TARE 保存 / own 绑定反馈 ---
    (
        "TOAST_TARE_SAVED",
        {
            "en": "Saved",
            "zh": "已保存",
        },
    ),
    (
        "TOAST_TARE_SAVE_FAIL",
        {
            "en": "Save failed",
            "zh": "保存失败",
        },
    ),
    (
        "TOAST_OWN_BOUND",
        {
            "en": "Own set",
            "zh": "已绑定本机",
        },
    ),
    (
        "TOAST_OWN_CLEARED",
        {
            "en": "Own cleared",
            "zh": "已取消本机",
        },
    ),
]
