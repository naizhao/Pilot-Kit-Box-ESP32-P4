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
            # 英文全大写：4.3″ 设置页的行标签统一是大写航电风格，与 PFD/列表
            # 页的标注一致。中文无大小写，不受影响。
            "en": "LANGUAGE",
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
        "ABOUT_LVGL",
        {
            "en": "LVGL",
            "zh": "LVGL",
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
            # 页签用 Setup 而非 Settings：后者在 26 px 下墨迹正好 94 px，与
            # 页签同宽、左右零余量，和相邻页签糊成一片。Setup 是 Garmin 等
            # 航电惯用的短写，语义无损。页内标题仍用完整的 SETTINGS。
            "en": "Setup",
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
            "en": "BRIGHTNESS",
            "zh": "屏幕亮度",
        },
    ),
    (
        "SETTINGS_THEME",
        {
            # 页上写的是 COLOR SCHEME 而不是 THEME：后者在座舱语境里容易被读成
            # 「主题模式」，前者是航电面板的惯用词。
            "en": "COLOR SCHEME",
            "zh": "配色方案",
        },
    ),
    (
        "THEME_DAY",
        {
            "en": "DAY",
            "zh": "日间",
        },
    ),
    (
        "THEME_NIGHT",
        {
            "en": "NIGHT",
            "zh": "夜间",
        },
    ),

    # ── 4.3″ 设置页余下的行标签与控件文案（spec §5.4）────────────────
    #
    # 分段控件的选项各占一条词条，而不是把「HDG UP/NORTH UP」拼成一条再切分：
    # 段宽是按**单个选项**的墨迹宽度算的（draw_seg 里的 pk_aa_text_width），
    # 拼串会让翻译者看不出每段实际能占多宽。
    (
        "SETTINGS_QNH",
        {
            # QNH 是 ICAO Q 简语，中文航空同样直接说 QNH，不译。
            # 仍进 catalog 是为了让设置页每一行的取文案方式一致，
            # 免得下一个人以为「这行可以硬编码」。
            "en": "QNH",
            "zh": "QNH",
        },
    ),
    (
        "SETTINGS_MAP_ORIENT",
        {
            "en": "MAP ORIENT",
            "zh": "地图朝向",
        },
    ),
    (
        "MAP_ORIENT_HDG_UP",
        {
            "en": "HDG UP",
            "zh": "航向朝上",
        },
    ),
    (
        "MAP_ORIENT_NORTH_UP",
        {
            "en": "NORTH UP",
            "zh": "正北朝上",
        },
    ),
    (
        "SETTINGS_RADAR_RANGE",
        {
            # NM（海里）是航图法定单位，中文侧照样保留缩写。
            "en": "RADAR RANGE NM",
            "zh": "雷达量程 NM",
        },
    ),
    (
        "BRIGHT_LOW",
        {
            "en": "LOW",
            "zh": "低",
        },
    ),
    (
        "BRIGHT_MID",
        {
            "en": "MID",
            "zh": "中",
        },
    ),
    (
        "BRIGHT_HIGH",
        {
            "en": "HIGH",
            "zh": "高",
        },
    ),
    (
        "BRIGHT_AUTO",
        {
            "en": "AUTO",
            "zh": "自动",
        },
    ),
    (
        "SETTINGS_LOG_STORE",
        {
            "en": "LOG STORAGE",
            "zh": "记录存储",
        },
    ),
    (
        "LOG_STORE_FLASH",
        {
            # 「内置闪存」而不是「闪存」：与隔壁的「SD 卡」并排时要一眼看出
            # 差别在「板载 vs 可插拔」，光写「闪存」两个选项听着是一回事。
            "en": "FLASH",
            "zh": "内置闪存",
        },
    ),
    (
        "LOG_STORE_SD",
        {
            "en": "SD CARD",
            "zh": "SD 卡",
        },
    ),
    (
        "SETTINGS_BLUETOOTH",
        {
            "en": "BLUETOOTH",
            "zh": "蓝牙",
        },
    ),
    (
        "SWITCH_OFF",
        {
            "en": "OFF",
            "zh": "关",
        },
    ),
    (
        "SWITCH_ON",
        {
            "en": "ON",
            "zh": "开",
        },
    ),
    (
        "SETTINGS_RESTART_HINT",
        {
            # 英文原文只有 (restart)，中文补一个「生效」——「(重启)」单看像是
            # 一个可点的重启按钮，而这里只是说明改动何时生效。
            "en": "(restart)",
            "zh": "(重启生效)",
        },
    ),
    (
        "SETTINGS_FORMAT_SD",
        {
            "en": "FORMAT SD",
            "zh": "格式化 SD 卡",
        },
    ),
    (
        "FORMAT_BTN_FORMAT",
        {
            "en": "FORMAT",
            "zh": "格式化",
        },
    ),
    (
        "FORMAT_BTN_ARMED",
        {
            # 两步确认的第二步。5s 是倒计时窗口（fmt_decay），中英都保留数字，
            # 否则看不出「再点」有时限。
            "en": "TAP AGAIN 5s",
            "zh": "再点一次 5s",
        },
    ),
    (
        "FORMAT_BTN_NO_CARD",
        {
            "en": "NO CARD",
            "zh": "无卡",
        },
    ),
    (
        "FORMAT_BTN_IN_USE",
        {
            # 记录正写在 SD 上，格式化会毁掉正在写的文件，故禁用。
            "en": "IN USE",
            "zh": "记录中",
        },
    ),
]
