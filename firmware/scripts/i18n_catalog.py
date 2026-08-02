#!/usr/bin/env python3
"""Single source of truth for built-in UI text.

The firmware keeps this catalog embedded for now, while the generated
interfaces are shaped so a future LittleFS/microSD language-pack backend can
replace the lookup implementation without touching render callers.

────────────────────────────────────────────────────────────────────────
词条 ID 的规矩（重要，别绕过）
────────────────────────────────────────────────────────────────────────
PK_TR_* 的枚举值**不由本文件的词条顺序决定**，而由 i18n_ids.json 台账固定。

  • 新增词条：直接加，位置随意（放到同类词条旁边最好读）。跑一次
    gen_i18n_assets.py，它会自动在台账末尾追加 max(id)+1，不碰任何旧 ID。
  • 删除词条：从这里删掉即可。台账里的条目**保留不删**，那个 ID 变成空洞、
    永不复用，生成的枚举里以 PK_TR_RESERVED_<id> 占位。
  • 绝对禁止手工改 i18n_ids.json 里已有 key 的 ID，也禁止删行或重排。

为什么这么严：2026-08 真机上演示模式徽章显示成「(数据为模」。当时生成器按出现
顺序发 ID，有人把新词条插在中间，后面所有 ID 整体后移一位（DEMO_BADGE 62→63）；
恰好一个陈旧的 pk_ui_nav.c.o 没重编、仍按旧 ID 62 取文案，而字符串表已是新的，
62 号换成了另一条。编译零警告、烧录校验通过、串口日志正常——完全静默，而它偏偏
是提示「当前数据是模拟的」的安全件。ID 钉死后，最坏只退化成「新词条缺失」，那是
链接期就报错的显性故障。
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
    # --- 瞬时屏幕提示(toast) — 阶段 5b：SD 写失败/降级告警（闪 3 次，
    # 复用 pk_ui_toast_show_blink，见 ui_state.h / pk_rec_store_fs.c） ---
    (
        "TOAST_REC_SD_NORAW",
        {
            "en": "SD LOW — RAW LOG STOPPED",
            "zh": "SD 空间不足——已停写原始报文",
        },
    ),
    (
        "TOAST_REC_SD_OWNONLY",
        {
            "en": "SD CRITICAL — ONLY OWN TRACK KEPT",
            "zh": "SD 空间告急——仅保留本机航迹",
        },
    ),
    (
        "TOAST_REC_SINK_FAIL",
        {
            "en": "RECORD WRITE FAILED — SINK STOPPED",
            "zh": "记录写入失败——该路已停用",
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
        "NAV_MAP",
        {
            "en": "Map",
            "zh": "地图",
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
    (
        # 全屏导航网格新增的一格。dock 时代放不下第 8 个页签（8×94+100 > 800，
        # 见 search_page.h 文件头），网格没有这个限制。
        "NAV_TOOLS",
        {
            "en": "Tools",
            "zh": "工具",
        },
    ),
    (
        "NAV_SEARCH",
        {
            "en": "Search",
            "zh": "搜索",
        },
    ),
    (
        "NAV_LOGBOOK",
        {
            "en": "Logbook",
            "zh": "记录",
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
    # 这里曾经有一条 BRIGHT_AUTO。删掉是因为**板上没有环境光传感器**
    # （docs/hardware 的引脚表里没有任何 ALS），AUTO 档点了不可能有反应。
    # 一度是摆出来置灰的——但灰掉的选项仍然占着一格触摸宽度，还让人反复
    # 猜「是不是坏了」。没有的能力就不该出现在控件里。
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
        "SETTINGS_DEVNAME",
        {
            # 设置页那一行的行标签。屏上显示的是**广播出去的完整名字**
            # （含 MAC 后缀），所以标签不写「蓝牙名」——用户改的只是前半段。
            "en": "DEVICE NAME",
            "zh": "设备名",
        },
    ),
    (
        "SETTINGS_DEMO",
        {
            # 设置页倒数第二行的行标签（最后一行留给格式化 SD 那个危险按钮）。
            "en": "DEMO MODE",
            "zh": "演示模式",
        },
    ),
    (
        "SETTINGS_DEMO_HINT",
        {
            # 跟在行标签后面的小字。必须写「模拟」两个字而不是「测试」——
            # 「测试模式」听起来像是在测真实传感器，而这里的数据根本不来自
            # 传感器。开关旁边就说清楚，别让人点开才发现。
            "en": "(simulated data)",
            "zh": "(数据为模拟)",
        },
    ),
    (
        "DEMO_BADGE",
        {
            # 常驻徽标。中英文都要短：它挤在顶栏右段与中段之间的固定槽位里，
            # 长了就会压住状态位。中文两字最紧凑，英文用通行的 DEMO。
            "en": "DEMO",
            "zh": "演示",
        },
    ),
    (
        "DEMO_SPLASH",
        {
            # 开机画面上的红字。splash 早于 LVGL，控件层那枚徽标盖不到它，
            # 而演示模式会跨重启存活，用户重新上电时必须被单独告知一次。
            "en": "DEMO MODE - ALL FLIGHT DATA IS SIMULATED",
            "zh": "演示模式：全部飞行数据均为模拟",
        },
    ),
    (
        "TOAST_DEMO_ON",
        {
            "en": "Demo mode ON - data is simulated",
            "zh": "演示模式已开启，数据为模拟",
        },
    ),
    (
        "TOAST_DEMO_OFF",
        {
            "en": "Demo mode OFF - real sensors",
            "zh": "演示模式已关闭，恢复真实数据",
        },
    ),
    (
        "DEVNAME_DEFAULT",
        {
            # 从没改过名字时显示的占位。不写空串：空控件看着像是坏了。
            "en": "DEFAULT",
            "zh": "默认",
        },
    ),
    (
        "KBD_CANCEL",
        {
            "en": "CANCEL",
            "zh": "取消",
        },
    ),
    (
        "KBD_CLEAR",
        {
            "en": "CLEAR",
            "zh": "清除",
        },
    ),
    (
        "KBD_DELETE",
        {
            # 退格键的键帽。中文两字 = S 档 34 px，英文 DEL 33 px，
            # 两边都塞得进 72 px 的键帽，不必为哪一种语言另调键宽。
            "en": "DEL",
            "zh": "删除",
        },
    ),
    (
        "KBD_OK",
        {
            # 确认键的键帽。英文不用 CONFIRM：S 档 77 px 会撑破键帽。
            "en": "OK",
            "zh": "确定",
        },
    ),
    (
        "KBD_CHARSET_HINT",
        {
            # 键盘上只有这几类字符，写明白比让用户找不到小写字母强。
            # 不做中文输入是硬限制：CJK 字形是 catalog 驱动的子集，
            # 任意汉字这台盒子画不出来。
            "en": "A-Z 0-9 - _ ONLY",
            "zh": "仅限 A-Z 0-9 - _",
        },
    ),
    (
        "SETTINGS_AC_CATEGORY",
        {
            # 设置页倒数第二行（阶段 5a）：驱动 pk_flight_phase 相位状态机的
            # 滑行/抬轮/巡航阈值，见设计文档「机型分类阈值」节。缩写成
            # "AC CATEGORY" 而不是 "AIRCRAFT CATEGORY"——本页行标签预算按
            # M 档 15px/字符 算 240px，全拼会溢出压住控件。
            "en": "AC CATEGORY",
            "zh": "机型分类",
        },
    ),
    (
        "AC_CAT_GLIDER",
        {
            # 5 档分段控件的选项，各占一条词条（理由同 MAP_ORIENT_* 一组：
            # 段宽按单个选项的墨迹宽度算，拼串会让翻译者看不出每段能占多宽）。
            # 中文统一 2 字，五段等长最整齐。
            "en": "GLIDER",
            "zh": "滑翔",
        },
    ),
    (
        "AC_CAT_HELI",
        {
            "en": "HELI",
            "zh": "直升",
        },
    ),
    (
        "AC_CAT_PISTON",
        {
            # 默认档（罩哥拍板：本产品用户主流是轻型活塞）。
            "en": "PISTON",
            "zh": "活塞",
        },
    ),
    (
        "AC_CAT_TURBOPROP",
        {
            "en": "TURBO",
            "zh": "涡桨",
        },
    ),
    (
        "AC_CAT_JET",
        {
            "en": "JET",
            "zh": "喷气",
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

    # ══════════════════════════════════════════════════════════════════
    # 诊断页（diag_page.c，spec §5.5）
    #
    # 翻译原则（三页共用）：
    #   1) **航电术语一律不译**——QNH / HDG / TRK / GS / ALT / SQK / ADS-B /
    #      NM / hPa / ft / kt。中文飞行员本来就说这些缩写，译成汉字反而要
    #      在脑子里再翻一道。子系统缩写（IMU / BARO / GPS / SDR / BLE /
    #      HDOP / SNR / microSD）同理。
    #   2) **状态描述一律译**——"no module" / "antenna OPEN" / "searching"
    #      这类是给排查者读的句子，不是术语；读不懂就排查不下去。
    #   3) **带数字的行不做整句格式串**，而是把词拆出来在 C 侧拼。整句
    #      "no fix %d visible" 进 catalog 会让翻译者能改动 %d 的个数与顺序，
    #      而 snprintf 的参数是写死的——对不上就是越界读栈。
    # ══════════════════════════════════════════════════════════════════
    (
        "DIAG_TITLE",
        {
            "en": "DIAGNOSTICS",
            "zh": "诊断",
        },
    ),

    # ── 卡片标题（总览八格 + 滚动区四格；同时也是详情页的页内标题）──
    #
    # 前六个是设备/协议缩写，中英同形：屏上写 IMU 比写「惯性测量单元」短得多，
    # 而这一格的读者正在扫「哪个子系统不对」，长词只会让扫描变慢。
    ("DIAG_CARD_IMU",    {"en": "IMU",     "zh": "IMU"}),
    ("DIAG_CARD_BARO",   {"en": "BARO",    "zh": "BARO"}),
    ("DIAG_CARD_GPS",    {"en": "GPS",     "zh": "GPS"}),
    ("DIAG_CARD_SDR",    {"en": "SDR",     "zh": "SDR"}),
    ("DIAG_CARD_BLE",    {"en": "BLE",     "zh": "BLE"}),
    ("DIAG_CARD_SD",     {"en": "microSD", "zh": "microSD"}),
    ("DIAG_CARD_QNH",    {"en": "QNH",     "zh": "QNH"}),
    # 余下五个是普通名词，缩写只是英文侧为了排版而缩的，中文没有理由跟着缩。
    ("DIAG_CARD_LOG",    {"en": "LOG",     "zh": "记录"}),
    ("DIAG_CARD_CLK",    {"en": "CLK",     "zh": "时钟"}),
    ("DIAG_CARD_SYS",    {"en": "SYS",     "zh": "系统"}),
    ("DIAG_CARD_BATT",   {"en": "BATT",    "zh": "电池"}),
    ("DIAG_CARD_UPTIME", {"en": "UPTIME",  "zh": "运行时长"}),

    # ── 详情页键名 ──
    #
    # 键列宽 224 px（DET_VAL_X - DET_KEY_X）。中文最长的「上次复位」4 字 ×
    # 18 px = 72 px，比英文的 "CALIBRATION"（165 px）还窄，不会挤到值列。
    ("DIAG_K_STATUS",      {"en": "STATUS",      "zh": "状态"}),
    ("DIAG_K_SATELLITES",  {"en": "SATELLITES",  "zh": "卫星"}),
    ("DIAG_K_HDOP",        {"en": "HDOP",        "zh": "HDOP"}),
    ("DIAG_K_ANTENNA",     {"en": "ANTENNA",     "zh": "天线"}),
    ("DIAG_K_POSITION",    {"en": "POSITION",    "zh": "位置"}),
    ("DIAG_K_SNR",         {"en": "SNR",         "zh": "SNR"}),
    ("DIAG_K_SENSOR",      {"en": "SENSOR",      "zh": "传感器"}),
    ("DIAG_K_CALIBRATION", {"en": "CALIBRATION", "zh": "校准"}),
    # 横滚/俯仰/航向是标准中文航空姿态术语，不是英文直译。
    ("DIAG_K_ROLL",        {"en": "ROLL",        "zh": "横滚"}),
    ("DIAG_K_PITCH",       {"en": "PITCH",       "zh": "俯仰"}),
    ("DIAG_K_YAW",         {"en": "YAW",         "zh": "航向"}),
    ("DIAG_K_PRESSURE",    {"en": "PRESSURE",    "zh": "气压"}),
    ("DIAG_K_ALTITUDE",    {"en": "ALTITUDE",    "zh": "高度"}),
    ("DIAG_K_QNH_REF",     {"en": "QNH REF",     "zh": "QNH 基准"}),
    ("DIAG_K_STATE",       {"en": "STATE",       "zh": "状态"}),
    ("DIAG_K_HINT",        {"en": "HINT",        "zh": "提示"}),
    ("DIAG_K_SAMPLE_RATE", {"en": "SAMPLE RATE", "zh": "采样率"}),
    ("DIAG_K_ADSB_MSGS",   {"en": "ADS-B MSGS",  "zh": "ADS-B 报文"}),
    ("DIAG_K_IQ_DROPPED",  {"en": "IQ DROPPED",  "zh": "IQ 丢弃"}),
    ("DIAG_K_LINK",        {"en": "LINK",        "zh": "连接"}),
    ("DIAG_K_PROTOCOL",    {"en": "PROTOCOL",    "zh": "协议"}),
    ("DIAG_K_BACKEND",     {"en": "BACKEND",     "zh": "当前后端"}),
    # 「设置」这一行说的是用户**希望**用哪个后端，与上面那行实际在用的分开。
    ("DIAG_K_SETTING",     {"en": "SETTING",     "zh": "设置为"}),
    ("DIAG_K_WRITTEN",     {"en": "WRITTEN",     "zh": "已写入"}),
    ("DIAG_K_DROPPED",     {"en": "DROPPED",     "zh": "丢弃"}),
    ("DIAG_K_SINK",        {"en": "SINK",        "zh": "输出"}),
    ("DIAG_K_TIME",        {"en": "TIME",        "zh": "时间"}),
    ("DIAG_K_SYNCED",      {"en": "SYNCED",      "zh": "已校时"}),
    ("DIAG_K_SOC_TEMP",    {"en": "SOC TEMP",    "zh": "SoC 温度"}),
    ("DIAG_K_LAST_RESET",  {"en": "LAST RESET",  "zh": "上次复位"}),
    ("DIAG_K_CAPACITY",    {"en": "CAPACITY",    "zh": "总容量"}),
    ("DIAG_K_FREE",        {"en": "FREE",        "zh": "剩余"}),
    ("DIAG_K_SOURCE",      {"en": "SOURCE",      "zh": "来源"}),
    ("DIAG_K_USED_BY",     {"en": "USED BY",     "zh": "用于"}),
    ("DIAG_K_CHARGE",      {"en": "CHARGE",      "zh": "电量"}),
    ("DIAG_K_VOLTAGE",     {"en": "VOLTAGE",     "zh": "电压"}),
    ("DIAG_K_ADC_RAW",     {"en": "ADC RAW",     "zh": "ADC 原始值"}),
    ("DIAG_K_CHARGING",    {"en": "CHARGING",    "zh": "充电中"}),
    ("DIAG_K_ON_UNPLUG",   {"en": "ON UNPLUG",   "zh": "拔掉输入"}),
    ("DIAG_K_SINCE_BOOT",  {"en": "SINCE BOOT",  "zh": "开机至今"}),
    ("DIAG_K_SECONDS",     {"en": "SECONDS",     "zh": "秒数"}),
    ("DIAG_K_DETAIL",      {"en": "DETAIL",      "zh": "详情"}),

    # ── 状态值 ──
    ("DIAG_V_OFFLINE",     {"en": "offline",     "zh": "离线"}),
    ("DIAG_V_IMU_ONLINE",  {"en": "BNO085 online",  "zh": "BNO085 在线"}),
    ("DIAG_V_IMU_OFFLINE", {"en": "BNO085 offline", "zh": "BNO085 离线"}),
    ("DIAG_V_BARO_ONLINE", {"en": "BMP388 online",  "zh": "BMP388 在线"}),
    ("DIAG_V_BARO_OFFLINE",{"en": "BMP388 offline", "zh": "BMP388 离线"}),
    # GPS 的五种情况指向完全不同的处理动作，措辞必须能分开：
    # 没装 / 装了但哑了 / 天线故障 / 搜星中 / 已定位。
    ("DIAG_V_NO_MODULE",   {"en": "no module",         "zh": "无模块"}),
    ("DIAG_V_MODULE_SILENT", {"en": "module silent >5s", "zh": "模块静默 >5s"}),
    ("DIAG_V_ANT_OPEN",    {"en": "antenna OPEN",  "zh": "天线开路"}),
    ("DIAG_V_ANT_SHORT",   {"en": "antenna SHORT", "zh": "天线短路"}),
    ("DIAG_V_SEARCHING",   {"en": "searching...",  "zh": "搜星中..."}),
    ("DIAG_V_FIX",         {"en": "fix",           "zh": "已定位"}),
    ("DIAG_V_NO_FIX",      {"en": "no fix",        "zh": "无定位"}),
    ("DIAG_V_FIX_3D",      {"en": "3D fix",        "zh": "3D 定位"}),
    # 与数字拼在一起的词，见本节开头第 3 条：C 侧用 "%d %s" 之类拼，
    # 中英的词序恰好一致（数字在前、量词在后），不需要调换顺序。
    ("DIAG_U_SATS",        {"en": "sats",     "zh": "星"}),
    ("DIAG_U_VISIBLE",     {"en": "visible",  "zh": "可见"}),
    ("DIAG_U_USED",        {"en": "used",     "zh": "已用"}),
    ("DIAG_U_IN_VIEW",     {"en": "in view",  "zh": "可见"}),
    ("DIAG_U_MSGS",        {"en": "msgs",     "zh": "报文"}),
    ("DIAG_U_RST",         {"en": "rst",      "zh": "复位"}),
    # 总览卡片上的三个前缀词。英文侧是小写缩写（卡片只有 352 px 可用，
    # "CALIBRATION" 一个词就占掉一半），中文侧没有缩写的必要——「校准」
    # 两个字比 "cal" 还窄，直接用详情页同一个说法，两层不换词。
    ("DIAG_U_CAL",         {"en": "cal",      "zh": "校准"}),
    ("DIAG_U_YAW",         {"en": "yaw",      "zh": "航向"}),
    # 日志卡的 "flash  w 1234"：w = written。中文用「写」而不是「已写入」，
    # 后者是详情页那一行的键名，卡片这里是「写了多少条」的量词。
    ("DIAG_U_W",           {"en": "w",        "zh": "写"}),
    ("DIAG_V_ANT_UNKNOWN", {"en": "unknown",  "zh": "未知"}),
    ("DIAG_V_ANT_OK",      {"en": "OK",       "zh": "正常"}),
    ("DIAG_V_ANT_OPEN_S",  {"en": "OPEN",     "zh": "开路"}),
    ("DIAG_V_ANT_SHORT_S", {"en": "SHORT",    "zh": "短路"}),
    ("DIAG_V_NO_SATS",     {"en": "(no satellites in view)", "zh": "(无可见卫星)"}),
    # SDR：没枚举时**直接把该插哪儿写在屏上**——这是接线问题，写 "OFFLINE"
    # 帮不上忙，写 H2 才能解决问题。
    ("DIAG_V_SDR_NONE",    {"en": "NO DONGLE - use H2 USB-C", "zh": "无接收机 - 用 H2 USB-C"}),
    ("DIAG_V_SDR_NONE_S",  {"en": "NO DONGLE",     "zh": "无接收机"}),
    ("DIAG_V_SDR_ATTACH",  {"en": "attached, opening...", "zh": "已连接, 打开中..."}),
    ("DIAG_V_SDR_ATTACH_S",{"en": "attached",      "zh": "已连接"}),
    ("DIAG_V_SDR_STALL",   {"en": "STALLED - no IQ >1s",  "zh": "停滞 - 无 IQ >1s"}),
    ("DIAG_V_SDR_STALL_S", {"en": "STALLED",       "zh": "停滞"}),
    ("DIAG_V_SDR_STREAM",  {"en": "streaming",     "zh": "数据流"}),
    ("DIAG_V_SDR_HINT",    {"en": "connect to H2 (USB OTG)", "zh": "接到 H2 (USB OTG)"}),
    # IMU / BARO 离线时的接线提示，照 SDR 那条的样子给。
    # 没有它，这两页在「外设全没接」时**整页只有一行**「传感器 离线」，下面
    # 四百像素纯黑——空态排查时看到的就是这个，第一反应是详情页没渲染出来。
    # 地址取各自驱动里的常量（imu_task.c IMU_I2C_ADDR / baro_task.c BMP388_ADDR），
    # 不另抄一份数字。
    ("DIAG_V_IMU_HINT",    {"en": "check I2C0 wiring (0x4A)", "zh": "检查 I2C0 接线 (0x4A)"}),
    # microSD / 时钟同理：这两页在空态下也各只有一行。
    ("DIAG_V_SD_HINT",     {"en": "insert a microSD card",   "zh": "插入 microSD 卡"}),
    ("DIAG_V_CLK_HINT",    {"en": "waiting for GPS or BLE",  "zh": "等待 GPS 或 BLE 校时"}),
    ("DIAG_V_BARO_HINT",   {"en": "check I2C0 wiring (0x76)", "zh": "检查 I2C0 接线 (0x76)"}),
    ("DIAG_V_BLE_CONN",    {"en": "connected",     "zh": "已连接"}),
    ("DIAG_V_BLE_ADV",     {"en": "advertising",   "zh": "广播中"}),
    ("DIAG_V_BLE_IDLE",    {"en": "idle",          "zh": "空闲"}),
    ("DIAG_V_BLE_PROTO",   {"en": "GDL90 over BLE","zh": "GDL90 over BLE"}),
    # 日志后端。两个名字与设置页那对选项分开：设置页要区分「板载 vs 可插拔」
    # 所以用「内置闪存 / SD 卡」，诊断页写的是**路径名**，与串口日志里出现的
    # 字样一致才好对照。
    ("DIAG_V_LOG_FLASH",   {"en": "flash",         "zh": "内置闪存"}),
    ("DIAG_V_LOG_SD",      {"en": "microSD",       "zh": "microSD"}),
    ("DIAG_V_SINK_DOWN",   {"en": "sink down",     "zh": "输出中断"}),
    ("DIAG_V_DOWN",        {"en": "down",          "zh": "中断"}),
    # 阶段 5b：LOG 卡片常驻显示 pk_rec_store 的降级档位/失效 sink 数——与
    # 上面的 sink down（record_sink_file，别人飞机那条日志）是两个独立
    # 写入管线，共用同一张卡片只是版面上不再多开一格（罩哥要求"复用现有
    # 实现"）。缩写要短：卡片值行宽度有限，被这三个词占满就顶不下前半句
    # 的 "microSD w N" 了。
    ("DIAG_V_REC_NORAW",   {"en": "raw off",       "zh": "限流"}),
    ("DIAG_V_REC_OWNONLY", {"en": "own only",      "zh": "仅本机"}),
    ("DIAG_V_REC_FAIL",    {"en": "fail",          "zh": "失效"}),
    ("DIAG_V_SD_MOUNTED",  {"en": "mounted",       "zh": "已挂载"}),
    ("DIAG_V_SD_NO_CARD",  {"en": "no card",       "zh": "无卡"}),
    ("DIAG_V_SD_FORMATTING", {"en": "formatting...", "zh": "格式化中..."}),
    ("DIAG_V_YES",         {"en": "yes",           "zh": "是"}),
    ("DIAG_V_NO",          {"en": "no",            "zh": "否"}),
    ("DIAG_V_NO_BATTERY",  {"en": "no battery",    "zh": "无电池"}),
    ("DIAG_V_NOT_DETECTED",{"en": "not detected",  "zh": "未检测到"}),
    # 这块板的电池只接充电通路、没有 power path：拔 USB 是彻底断电再上电。
    ("DIAG_V_ON_UNPLUG",   {"en": "device reboots (no power path)",
                            "zh": "设备重启 (无电源通路)"}),
    ("DIAG_V_QNH_SOURCE",  {"en": "user setting (Settings page)",
                            "zh": "用户设定 (设置页)"}),
    ("DIAG_V_QNH_USED_BY", {"en": "baro altitude calculation",
                            "zh": "气压高度计算"}),
    ("DIAG_V_NO_FURTHER",  {"en": "no further data", "zh": "无更多数据"}),
    # 「重启生效」——LOG 卡/详情用它说明「设置改了但还没换后端」。
    ("DIAG_V_AFTER_RESTART", {"en": "restart",     "zh": "重启生效"}),
    ("DIAG_V_SET_TO",      {"en": "set",           "zh": "设为"}),

    # 上次复位原因。区分能力才是它的价值：BROWNOUT 说明供电撑不住瞬时负载，
    # power-on 说明真的断过电，panic/WDT 说明是固件的锅——三者排查方向完全
    # 不同，靠"它重启了"这一句分不出来。总览卡与详情页共用这一组。
    ("DIAG_RST_UNKNOWN",   {"en": "unknown",    "zh": "未知"}),
    ("DIAG_RST_POWERON",   {"en": "power-on",   "zh": "上电"}),
    ("DIAG_RST_EXT",       {"en": "external",   "zh": "外部"}),
    ("DIAG_RST_SW",        {"en": "software",   "zh": "软件"}),
    ("DIAG_RST_PANIC",     {"en": "panic",      "zh": "崩溃"}),
    ("DIAG_RST_INT_WDT",   {"en": "int WDT",    "zh": "中断 WDT"}),
    ("DIAG_RST_TASK_WDT",  {"en": "task WDT",   "zh": "任务 WDT"}),
    ("DIAG_RST_WDT",       {"en": "other WDT",  "zh": "其它 WDT"}),
    ("DIAG_RST_SLEEP",     {"en": "deep sleep", "zh": "深睡眠"}),
    ("DIAG_RST_BROWNOUT",  {"en": "brownout",   "zh": "欠压"}),
    ("DIAG_RST_SDIO",      {"en": "SDIO",       "zh": "SDIO"}),
    ("DIAG_RST_USB",       {"en": "USB",        "zh": "USB"}),
    ("DIAG_RST_JTAG",      {"en": "JTAG",       "zh": "JTAG"}),

    # ══════════════════════════════════════════════════════════════════
    # 列表页（adsb_list.c，spec §5.3）
    #
    # 列头必须放进既有列宽：宽度账见 adsb_list.c 顶部那张表，一组
    # _Static_assert 钉着列位与分隔线的关系。中文列头一律两字（XS 档 24 px），
    # 比英文的 "CALLSIGN"（80 px）窄，不会破坏那些断言。
    # ══════════════════════════════════════════════════════════════════
    ("LIST_TITLE",     {"en": "AIRCRAFT", "zh": "航空器"}),
    # BRG / ALT / V/S / GS / TRK 是航电缩写，中文侧照样不译（见本文件开头
    # 的翻译原则第 1 条）。只有 CALLSIGN / DIST / AGE 是普通名词。
    ("LIST_COL_BRG",   {"en": "BRG",      "zh": "BRG"}),
    ("LIST_COL_CALL",  {"en": "CALLSIGN", "zh": "呼号"}),
    ("LIST_COL_DIST",  {"en": "DIST",     "zh": "距离"}),
    ("LIST_COL_ALT",   {"en": "ALT",      "zh": "ALT"}),
    ("LIST_COL_VS",    {"en": "V/S",      "zh": "V/S"}),
    ("LIST_COL_GS",    {"en": "GS",       "zh": "GS"}),
    ("LIST_COL_TRK",   {"en": "TRK",      "zh": "TRK"}),
    # AGE 这一列是「上次收到报文距今多少秒」，决定上面七列还能不能信。
    # 中文用「更新」而不是「时长」：读者关心的是新鲜度，不是持续时间。
    ("LIST_COL_AGE",   {"en": "AGE",      "zh": "更新"}),
    ("LIST_NO_CONTACTS", {"en": "NO CONTACTS", "zh": "无目标"}),
    ("LIST_SORT",      {"en": "SORT",     "zh": "排序"}),
    ("LIST_RESET",     {"en": "RESET",    "zh": "重置"}),
    # 详情抽屉的键（XS 档，键列 96 px；中文最长 4 字 = 48 px）。
    ("LIST_D_ICAO",    {"en": "ICAO",     "zh": "ICAO"}),
    ("LIST_D_REG",     {"en": "REG",      "zh": "注册号"}),
    ("LIST_D_TYPE",    {"en": "TYPE",     "zh": "机型"}),
    ("LIST_D_MODEL",   {"en": "MODEL",    "zh": "型号"}),
    ("LIST_D_AIRLINE", {"en": "AIRLINE",  "zh": "航司"}),
    ("LIST_D_COUNTRY", {"en": "COUNTRY",  "zh": "国籍"}),
    # 徽章上的 EMG / RDO / HJK 保持英文（国际通用），但抽屉里这一行是键名，
    # 中文有通行说法「应答机」。
    ("LIST_D_SQUAWK",  {"en": "SQUAWK",   "zh": "应答机"}),
    ("LIST_D_LAST_SEEN", {"en": "LAST SEEN", "zh": "上次报文"}),
    ("LIST_D_AGO",     {"en": "s ago",    "zh": "秒前"}),

    # ══════════════════════════════════════════════════════════════════
    # 交通页（traffic_page.c，spec §5.2）
    #
    # 朝向那两个词不在这里另立一份：设置页的 MAP_ORIENT_HDG_UP /
    # _NORTH_UP 就是同一件事，两处各写一份迟早会说成两种话。
    # ══════════════════════════════════════════════════════════════════
    ("TFC_TITLE",      {"en": "TRAFFIC",    "zh": "交通"}),
    # 本机位置未知 → 极坐标图整幅失去基准，画不了任何目标。
    ("TFC_NO_OWN_POS", {"en": "NO OWN POS", "zh": "无本机位置"}),
    # 光说「没有」不够：用户第一次开机看到的就是这一屏，得让他知道设备在等
    # 什么、他能做什么，否则「没有本机位置」和「设备坏了」在他眼里一个样。
    # 两条来源都要写：GPS 定位是自动等来的，绑定本机是他自己点一下的事。
    # 不写标点：全表 246 个汉字里一个标点都没有，为一句话新增一个全角逗号
    # 字形，既破了行文风格也白占四档字模。
    ("TFC_NO_OWN_HINT", {"en": "WAITING FOR GPS OR OWN-SHIP BIND",
                         "zh": "等待 GPS 定位或绑定本机"}),
    # 收到了目标、但一架都不在当前量程内。与「一架都没收到」观感相同、成因
    # 完全不同：那一种要等信号，这一种按一下 − 就能看见，不能共用一句话。
    ("TFC_ALL_OUT_RANGE", {"en": "ALL TRAFFIC OUTSIDE RANGE",
                           "zh": "目标全在量程外"}),

    # ══════════════════════════════════════════════════════════════════
    # PFD 底部左右信息框的行标签（pfd_infobox.c）
    #
    # 边界原则（本表开头翻译原则第 1 条的具体化）：
    #   **标准缩写一律不译，自造缩写一律改成能读懂的形式。**
    # HDG / VS / GPS / ADS-B / KM/H 属前者——中文飞行员本来就这么说，译成汉字
    # 反而要在脑子里翻一道，所以它们根本不进这张表。下面三条属后者：OWN 是
    # own-ship 的截断（ICAO/FAA 从不这么缩），B 是 BARO 的截断，ALT 在这一格
    # 里指的是「米制高度」而不是 ALT 本身——三个都是当年小屏挤不下才发明的，
    # 换到 800×480 之后位置绰绰有余，没有理由继续让人猜。
    #
    # 宽度账（M 档：拉丁 15 px/字符、汉字 22 px/字；行内可用宽 = 200 - 2×4 = 192）：
    #   本机(44)   / OWN(45)     + 呼号最长 8 字符(120) = 164 或 165 ≤ 192
    #   气压(44)   / BARO(60)    + "99999ft"(105)      = 149 或 165 ≤ 192
    #   米制(44)   / ALT(M)(90)  + "99999m"(90)        = 134 或 180 ≤ 192
    # 英文侧 OWN 保留：OWN-SHIP(120) + 呼号(120) = 240 放不下，而 OWN 在英语
    # 座舱显示里至少还能读成 own-ship 的开头；中文读者没有这条线索，所以中文
    # 侧给全词。
    # ══════════════════════════════════════════════════════════════════
    ("PFD_IB_OWN",   {"en": "OWN",    "zh": "本机"}),
    ("PFD_IB_BARO",  {"en": "BARO",   "zh": "气压"}),
    # 这一行是**上一行那个高度的米制换算**（数据源是 ADS-B 高度）。原来上下
    # 两行一个叫 B 一个叫 ALT，读不出「这行是米」——而中国空域用米制高度层，
    # 这一行对中国飞行员价值最高，标签却最含糊。
    ("PFD_IB_ALT_M", {"en": "ALT(M)", "zh": "米制"}),

    # ══════════════════════════════════════════════════════════════════
    # 地图页（map_page.c，SD 离线地图一期，
    # docs/superpowers/specs/2026-08-01-sd-offline-map-design.md）
    #
    # 页签短名 NAV_MAP 已在导航区那一组里（挨着 NAV_TRAFFIC），这里是页面
    # 自身要用的标题/错误态/按钮文案。
    # ══════════════════════════════════════════════════════════════════
    ("MAP_TITLE", {"en": "MAP", "zh": "地图"}),
    # 无 SD / 无 maps 目录 / 无有效包——三种成因合并成一个标题,靠 HINT 那一行
    # 区分（同 diag 页"同一症状、不同 HINT"的写法）。
    ("MAP_NO_DATA_TITLE", {"en": "NO OFFLINE MAP", "zh": "无离线地图"}),
    ("MAP_HINT_NO_CARD",  {"en": "INSERT A MICROSD WITH A /maps FOLDER",
                           "zh": "请插入含 /maps 目录的 microSD 卡"}),
    ("MAP_HINT_NO_PACK",  {"en": "NO VALID MAP PACKAGE IN /sdcard/maps",
                           "zh": "/sdcard/maps 目录下没有有效地图包"}),
    # 运行中拔卡：已缓存瓦片继续显示,这条只提示"新瓦片拿不到了"。
    ("MAP_SD_REMOVED", {"en": "MICROSD REMOVED — SHOWING CACHED MAP",
                        "zh": "SD 卡已拔出——显示缓存地图"}),
    # 运行中插卡（与上面那条对称）。报的是**挂载结果**而不是"卡插进来了"
    # ——插卡这个动作用户自己看得见，他要知道的是能不能用。分两条：
    #   MOUNTED   → 认卡且扫到了地图包，绿色；
    #   NO_PACKS  → 卡认了但 /maps 下没有有效包，对地图页等于没卡，红色。
    # 开机就插着卡不弹这两条（那是常态不是事件），判定在
    # pk_tile_loader.c handle_sd_transition()。
    ("MAP_SD_MOUNTED", {"en": "MICROSD MOUNTED — MAP PACKS RELOADED",
                        "zh": "SD 卡已挂载——地图包已重新载入"}),
    ("MAP_SD_NO_PACKS", {"en": "MICROSD MOUNTED — NO MAP PACKS FOUND",
                         "zh": "SD 卡已挂载——未找到地图包"}),
    # 卡在位但 FAT 挂不上（分区不是 FAT / 已损坏）。与"没插卡"是两回事，
    # 给用户可执行的下一步，别让他反复插拔一张坏卡。
    ("MAP_SD_UNREADABLE", {"en": "MICROSD UNREADABLE — FORMAT IT AS FAT32",
                           "zh": "SD 卡读不出——请格式化为 FAT32"}),
    # 回中按钮：手动平移后出现,点击回到跟随本机+居中。短词,要塞进一个
    # 56 px 圆按钮。
    ("MAP_RECENTER", {"en": "CENTER", "zh": "回中"}),
    # 越级放大（overzoom）提示：数据只到某个 zoom,继续放大时用父瓦片截取
    # 放大显示,弱化提示"这不是真实精度"。%d 是放大倍数。
    ("MAP_OVERZOOM_FMT", {"en": "OVERZOOM x%d", "zh": "越级放大 x%d"}),
    # 底图署名——FlightMate 把这行画成黑底黑字看不见的教训,不能重蹈。
    # 两种语言原样一致：地图数据版权声明不译（与 QNH/ICAO 那批术语同一原则）。
    # 主署名 Pilot Kit Map（罩哥 2026-08-01 定）；OpenStreetMap 保留在后——
    # 底图数据是 OSM(ODbL)，商用去掉 OSM 署名有合规风险，不可只留自家名。
    ("MAP_ATTRIBUTION", {"en": "© Pilot Kit Map  © OpenStreetMap",
                         "zh": "© Pilot Kit Map  © OpenStreetMap"}),

    # ══════════════════════════════════════════════════════════════════
    # 航空数据搜索页（search_page.c）
    #
    # 这一页曾经整页写死 ASCII 英文字面量，理由是「本页的内容（机场名、
    # ident）本来就只能是 ASCII」。那个理由只覆盖**数据**，不覆盖**框架文字**：
    # 标题、分组标题、三种空态的提示语全是给人读的散文，中文语言下顶着一屏
    # 英文，与地图/交通/诊断三页的做法不一致。
    #
    # 两条词条**刻意不在这里另立一份**（同「朝向那两个词」的先例）：
    #   · CLEAR 清除钮 → KBD_CLEAR，与键盘上那枚同词同义同动作；
    #   · 没有本机位置该怎么办 → TFC_NO_OWN_HINT，交通页那一句已经把两条
    #     来源（等 GPS 定位 / 自己绑定本机）说全了，两处各写一份迟早说岔。
    #
    # 类型徽章 APT / NAV / FIX 也不进表：与 HDG / QNH / ICAO 同一条原则——
    # 标准航空缩写一律不译，中文飞行员本来就这么读。
    #
    # 宽度账（可用宽 = 784 − 16 = 768；XS 档 ASCII 10 / 汉字 15，
    # S 档 ASCII 11 / 汉字 17）：
    #   NO_DB_ABSENT (XS)  6 汉字 90 + 29 拉丁 290 = 380 ≤ 768
    #   NO_MATCH_HINT(XS) 12 汉字 180 + 15 拉丁 150 = 330 ≤ 768
    #   PLACEHOLDER  (S )  9 汉字 153 + 11 拉丁 121 = 274 ≤ 754（有清除钮时更窄）
    #   NO_POS       (S ) 13 汉字 221                      ≤ 768
    # 圆括号一律不用：AA 字库里 '(' ')' 的字形几乎与方括号同形，屏上读成
    # "[ZGGG]"（英文侧已经因此改过一轮，中文侧不重蹈）。
    # ══════════════════════════════════════════════════════════════════
    ("SEARCH_TITLE", {"en": "SEARCH", "zh": "搜索"}),
    ("SEARCH_CLOSE", {"en": "CLOSE", "zh": "关闭"}),
    # 输入框占位。写清楚"能搜什么"而不是只写"搜索"：这台盒子没有实体键盘，
    # 用户点一下才弹软键盘，点之前得先知道值不值得点。
    ("SEARCH_PLACEHOLDER", {"en": "TAP TO TYPE - ICAO / IDENT / NAME",
                            "zh": "点击输入 ICAO / 识别码 / 名称"}),
    ("SEARCH_SEC_NEARBY",  {"en": "NEARBY AIRPORTS", "zh": "附近机场"}),
    ("SEARCH_SEC_RECENT",  {"en": "RECENT SEARCHES", "zh": "最近搜索"}),
    ("SEARCH_SEC_RESULTS", {"en": "RESULTS", "zh": "搜索结果"}),
    # 第 5 桶（名称子串顺扫）真机要好几秒，这一态用户一定看得见。
    ("SEARCH_BUSY",        {"en": "SEARCHING ...", "zh": "搜索中 ..."}),
    ("SEARCH_NO_HISTORY",  {"en": "NO RECENT SEARCHES YET", "zh": "还没有搜索记录"}),
    # 库未就绪：标题一句，成因靠下面三条 HINT 分（同 diag 页「同一症状、
    # 不同 HINT」的写法）。
    ("SEARCH_NO_DB",        {"en": "AERO DATABASE NOT AVAILABLE",
                             "zh": "航空数据库不可用"}),
    ("SEARCH_NO_DB_ABSENT", {"en": "INSERT A microSD CARD WITH /aero/pk_aero.bin",
                             "zh": "请插入含 /aero/pk_aero.bin 的 microSD 卡"}),
    ("SEARCH_NO_DB_LOAD",   {"en": "DATABASE IS STILL LOADING - TRY AGAIN IN A MOMENT",
                             "zh": "数据库仍在加载——稍后再试"}),
    ("SEARCH_NO_DB_ERR",    {"en": "DATABASE ERROR - SEE DIAGNOSTICS PAGE",
                             "zh": "数据库出错——详见诊断页"}),
    # v2 卡：前缀与子串索引全缺席，返回 0 条不是"没有这个机场"。这两句必须
    # 让用户看出该换卡，否则他会反复重敲同一个代码。
    ("SEARCH_NO_INDEX",      {"en": "v2 DATA PACK - NO SEARCH INDEX ON THIS CARD",
                              "zh": "v2 数据包——本卡没有搜索索引"}),
    ("SEARCH_NO_INDEX_HINT", {"en": "COPY A v3 PACK TO THE CARD TO SEARCH BY CODE",
                              "zh": "拷入 v3 数据包才能按代码搜索"}),
    # 真的没命中。提示给两个能照着敲的例子——"换个词试试"等于没说。
    ("SEARCH_NO_MATCH",      {"en": "NO MATCH", "zh": "无匹配"}),
    ("SEARCH_NO_MATCH_HINT", {"en": "TRY AN ICAO CODE LIKE ZGGG OR A NAVAID IDENT LIKE SZA",
                              "zh": "试试 ICAO 代码 ZGGG，或导航台识别码 SZA"}),
    # 「附近」这一组要本机位置才算得出距离；提示语复用 TFC_NO_OWN_HINT。
    ("SEARCH_NO_POS",    {"en": "NO OWN POSITION - NEARBY LIST UNAVAILABLE",
                          "zh": "无本机位置——附近列表不可用"}),
    # 有位置、库也正常，就是这一带没有机场（比如远海）。与上一条成因完全
    # 不同：那一种要等定位，这一种等也没用。
    ("SEARCH_NO_NEARBY", {"en": "NO AIRPORT IN RANGE OF THIS POSITION",
                          "zh": "该位置附近没有机场"}),
    # ══════════════════════════════════════════════════════════════════
    # 机场详情页（apt_detail_page.c）
    #
    # 两个入口：搜索结果点机场、地图上点机场符号。屏上的**数据**（ICAO 码、
    # 机场名、跑道号 06L/24R、频率数字）一律不进表——那是数据包里的原文。
    #
    # 三类缩写刻意不立词条，直接写 ASCII 字面量（同 APT / NAV / FIX 徽章的
    # 先例，也同 HDG / QNH / ICAO 那一条原则）：
    #   · 服务类型 TWR / GND / ATIS / APP / DEP / CTAF / UNICOM / AWOS …
    #     ——ICAO 通用缩写，无线电里就这么念，译成"塔台"反而对不上耳朵；
    #   · 道面 ASPH / CONC / GRASS …——航图上就印这几个词；
    #   · MAG（磁航向）、THR（跑道入口）、ft、NM——航图/仪表通用单位与缩写。
    #
    # 宽度账（可用宽 = 784 − 16 = 768；XS 档 ASCII 10 / 汉字 15，
    # S 档 ASCII 11 / 汉字 17）：
    #   APTD_SHOW_ON_MAP (S ) 11 拉丁 121 ≤ 按钮内宽 190−24=166；
    #                          中文 6 汉字 102 ≤ 166
    #   APTD_BACK        (S )  4 拉丁  44 ≤ 按钮内宽 110−24=86；中文 2 字 34
    #   APTD_UNCTRL      (XS) 12 拉丁 120 ≤ 胶囊上限 200；中文 3 字 45
    #   APTD_NO_FREQ     (S ) 20 拉丁 220 ≤ 768；中文 5 字 85
    # 圆括号一律不用（AA 字库里 '(' ')' 与方括号几乎同形，同搜索页那一条）。
    # ══════════════════════════════════════════════════════════════════
    # 返回上一层。写"返回"而不是"关闭"：这一页有来路——从搜索进来就回搜索、
    # 从地图进来就回地图，"关闭"会让人以为回到的是某个固定的地方。
    ("APTD_BACK", {"en": "BACK", "zh": "返回"}),
    # 把视口挪到这个机场并落一枚 PIN。动词打头，说清楚点下去会发生什么。
    ("APTD_SHOW_ON_MAP", {"en": "SHOW ON MAP", "zh": "在地图上显示"}),
    ("APTD_SEC_RUNWAYS", {"en": "RUNWAYS", "zh": "跑道"}),
    ("APTD_SEC_FREQ",    {"en": "FREQUENCIES", "zh": "频率"}),
    # 头部那三枚胶囊。ELEV 在英文侧保留航图缩写，中文侧用"标高"——这是
    # 航图中文版的标准译法，不是自造词。
    ("APTD_ELEV", {"en": "ELEV", "zh": "标高"}),
    # 是否管制。三态分开报：库里 ctrl 有"未知"这一档（CTRL_ENUM 未命中→0），
    # 把未知并进"无管制"是在替飞行员做一个他没授权的判断。
    ("APTD_CTRL",         {"en": "CONTROLLED",      "zh": "有管制"}),
    ("APTD_UNCTRL",       {"en": "UNCONTROLLED",    "zh": "无管制"}),
    ("APTD_CTRL_UNKNOWN", {"en": "CONTROL UNKNOWN", "zh": "管制情况未知"}),
    # 两种空段。跑道/频率各自为空的成因相同——这个机场在数据包里就没有那一段，
    # 所以不再分 HINT；说清楚"是数据没有"而不是"页面坏了"即可。
    ("APTD_NO_RUNWAY", {"en": "NO RUNWAY DATA IN THIS PACK",
                        "zh": "本数据包内无跑道数据"}),
    ("APTD_NO_FREQ",   {"en": "NO FREQUENCY DATA IN THIS PACK",
                        "zh": "本数据包内无频率数据"}),
    # 整页取不到数据：拔卡、库还没加载完、或下标越界。仍然把页面打开并显示
    # 这一句，而不是静默不开——静默会让用户以为自己没点中那个圆圈。
    ("APTD_UNAVAILABLE", {"en": "AIRPORT DATA UNAVAILABLE",
                          "zh": "机场数据不可用"}),
]
