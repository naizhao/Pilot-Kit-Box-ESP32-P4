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

    # ── 4.3″ 触摸导航（spec §3.2 / §5）──────────────────────────
    #
    # dock 的六个一级页签。这里只放**页签短名**，与各页面自身的标题分开：
    # 页签宽约 94 px，塞不下「罗盘校准」这类完整标题，而标题在页内还要用。
    (
        "NAV_PFD",
        {
            "en": "PFD",
            "zh": "PFD",
        },
    ),
    (
        "NAV_TRAFFIC",
        {
            "en": "Traffic",
            "zh": "交通",
        },
    ),
    (
        "NAV_LIST",
        {
            "en": "List",
            "zh": "列表",
        },
    ),
    (
        "NAV_SETTINGS",
        {
            "en": "Settings",
            "zh": "设置",
        },
    ),
    (
        "NAV_ABOUT",
        {
            "en": "About",
            "zh": "关于",
        },
    ),
    (
        "NAV_DIAG",
        {
            "en": "Diag",
            "zh": "诊断",
        },
    ),

    # dock 右侧的动作区。「调平」是把当前姿态归零，与 TARE 是同一件事，
    # 但对飞行员说「调平」比说「TARE」直白。
    (
        "ACT_LEVEL",
        {
            "en": "Level",
            "zh": "调平",
        },
    ),
    (
        "ACT_LEVEL_HINT",
        {
            "en": "Hold 1 s to level the horizon",
            "zh": "长按 1 秒调平地平仪",
        },
    ),

    # 校准向导的「稍后再说」——没有物理按键后，跳过必须有可点的出口。
    (
        "CAL_LATER",
        {
            "en": "Later",
            "zh": "稍后再说",
        },
    ),

    # 设置页新增项（为半反半透屏铺路，spec §5 表）。
    (
        "SETTINGS_BRIGHTNESS",
        {
            "en": "Brightness",
            "zh": "屏幕亮度",
        },
    ),
    (
        "SETTINGS_THEME",
        {
            "en": "Theme",
            "zh": "配色",
        },
    ),
    (
        "THEME_DAY",
        {
            "en": "Day",
            "zh": "日间",
        },
    ),
    (
        "THEME_NIGHT",
        {
            "en": "Night",
            "zh": "夜间",
        },
    ),
]
