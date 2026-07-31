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
]
