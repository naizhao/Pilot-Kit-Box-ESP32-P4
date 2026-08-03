#!/usr/bin/env python3
"""从模拟器批量生成 UI 截图，落到仓库根的 images/。

为什么要脚本而不是手工截
------------------------
这批图要进 README 和官网。UI 每改一次它们就全部过期，而手工截图的代价高到
让人不想重截——于是网站上挂着半年前的界面。写成脚本后重出一遍只要一条命令，
过期成本降到接近零。

同时它也是**回归基线**：改完布局跑一次，用 git diff 看哪些图变了，就知道
改动波及了哪些场景。没变的图 git 不会记录，变了的一眼可见。

每张图的「场景」由环境变量拼出来（模拟器一路加过来的那些开关），
所以这里的场景表同时也是一份「模拟器支持哪些状态」的清单。
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

SIM_DIR = Path(__file__).resolve().parent
REPO = SIM_DIR.parent
BUILD = SIM_DIR / "build-capture"
OUT = REPO / "images"

# 定格在动画的哪一秒。13.7 s 时 pitch≈0、roll≈-24°，地平线倾斜但不极端，
# 俯仰正负刻度同时可见——是最能说明问题的一帧。换值会让所有图一起变。
AT_SEC = "13.7"

# name → 环境变量。空 dict = 默认态（PFD 主页，中文，菜单未打开）。
SCENES: list[tuple[str, dict[str, str], str]] = [
    # PFD 主页**没有**中英两版：这一屏全是国际通用的符号、数字与固定缩写
    # （HDG / KM/H / ALT / VS），一个 i18n 词条都没有，两种语言渲染逐字节相同。
    # 这与 ICAO 标准仪表不做本地化是一致的——语言只影响导航与设置这类文字界面。
    ("ui-4.3-pfd",          {},                                   "PFD 主页"),

    # ── 全屏导航网格（主菜单）──────────────────────────────────────
    #
    # 取代了原来的横向 dock（ui-4.3-dock / -dock-en / -dock-left 三张，
    # 2026-08-02 随 dock 一起删）。**没有 -left 的对应物**：dock 锚在 FAB 上、
    # 随吸附边缘反向铺开，网格是全屏的，FAB 在哪一侧它都长一个样。
    #
    # 四张各压一件事：默认那一屏、英文（标签最宽的一版）、第 2 页（3 项 +
    # 5 格空位，验"末页不居中"这条产品决定）、亮度快调 pop（网格再压一档）。
    ("ui-4.3-menu",         {"PK_SIM_MENU": "1"},                 "主菜单第 1 页：7 项 + 1 格余量 + 动作条"),
    ("ui-4.3-menu-en",      {"PK_SIM_MENU": "1",
                             "PK_SIM_LANG": "en"},                "主菜单第 1 页（英文，标签最宽的一版）"),
    # PK_SIM_UI_MODE=6 = PK_UI_MODE_DIAG：让第 2 页也带一个选中框，
    # 否则选中态只在第 1 页验得到。
    ("ui-4.3-menu-page2",   {"PK_SIM_MENU_PAGE": "1",
                             "PK_SIM_UI_MODE": "6"},              "主菜单第 2 页：3 项从左上角起排，不居中"),
    ("ui-4.3-menu-bright",  {"PK_SIM_MENU_BRIGHT": "1"},          "主菜单 + 亮度快调 pop：网格再压一档"),
    # 调平长按的 ③④ 两态（spec §6）。它们只在手指按住的那 1 s 与随后的
    # 200 ms 里存在，真机上根本没法在截图里定格——这两张就是唯一的验收物：
    # 填充确实从左向右长、且没把橙色标签吃掉；绿闪把那一格整块反白。
    # 70% 是量出来的，不是随手取的：中文标签「调平」占 x∈[102,162]，格宽 266，
    # 走到 60% 时填充边界落在 x=161——正好卡在字的右缘上，"字压在填充上"和
    # "字被填充截断"两种情况在图上分不出来。70%（x≈186）把整个标签**完整**
    # 圈进填充里，这张图才真的在验对比度。
    ("ui-4.3-menu-level",   {"PK_SIM_MENU_LEVEL": "70"},          "调平长按进行中：橙色填充走到 70%，标签整个压在填充上仍可读"),
    ("ui-4.3-menu-level-done", {"PK_SIM_MENU_LEVEL_DONE": "1"},   "调平完成：那一格绿闪 200 ms（标签反白成底色）"),

    ("ui-4.3-subpage",      {"PK_SIM_SUB": "1"},                  "二级页面：返回栏 + FAB 变 ←"),
    ("ui-4.3-toast",        {"PK_SIM_TOAST": "1"},                "Toast 提示压在最上层"),
    ("ui-4.3-toast-rec",    {"PK_SIM_TOAST_REC": "1"},            "阶段5b：SD写失败/降级告警红色Toast，核对长文案不裁切"),
    ("ui-4.3-battery-low",  {"PK_SIM_BATT": "3"},                 "低电量：电池转 alert 图标并变红"),
    ("ui-4.3-charging",     {"PK_SIM_BATT": "45",
                             "PK_SIM_CHARGING": "1"},             "充电中：电池播放逐帧动画"),

    # ── 极端无数据 ──────────────────────────────────────────────────
    #
    # 上面那批压的都是「极端大数据」：最长呼号、目标扎堆、数值取极值。另一个
    # 极端一直没人系统压过——而**用户第一次开机看到的就是它**：什么都没接、
    # 什么都没收到。产品负责人手上那台盒子（IMU/气压/GPS/SDR 全没接）反馈
    # 「无本机位置几个字被挡住了」，正是这一侧从没被截过图的直接后果。
    #
    # PK_SIM_EMPTY=1 是总开关（等于把所有单项开关一起打开）；单项开关见
    # sim/main.c 文件头那张表，用来定位「只缺这一样」时该页降级成什么样。
    # 中英两版都要：中文比英文宽，提示语的居中与遮挡在两侧不是同一回事。
    ("empty-4.3-pfd",       {"PK_SIM_EMPTY": "1"},                "PFD 全空：ATT FAIL + 罗盘撤除 + 各读数 ---"),
    ("empty-4.3-traffic",   {"PK_SIM_PAGE": "traffic",
                             "PK_SIM_EMPTY": "1"},                "交通页全空：无本机位置 + 等什么的提示"),
    ("empty-4.3-traffic-en",{"PK_SIM_PAGE": "traffic",
                             "PK_SIM_EMPTY": "1",
                             "PK_SIM_LANG": "en"},                "同上英文（英文更宽，另测一版）"),
    ("empty-4.3-traffic-noown", {"PK_SIM_PAGE": "traffic",
                             "PK_SIM_NO_OWN": "1"},               "只缺本机位置：收得到目标但画不出"),
    ("empty-4.3-traffic-far",   {"PK_SIM_PAGE": "traffic",
                             "PK_SIM_TFC_FAR": "1"},              "目标全在量程外：与「没收到」要分开说"),
    ("empty-4.3-list",      {"PK_SIM_PAGE": "list",
                             "PK_SIM_EMPTY": "1"},                "看板全空：NO CONTACTS"),
    ("empty-4.3-list-bare", {"PK_SIM_PAGE": "list",
                             "PK_SIM_TFC_BARE": "1"},             "目标只有位置：呼号/高度/速度各列降级"),
    ("empty-4.3-diag",      {"PK_SIM_PAGE": "diag",
                             "PK_SIM_EMPTY": "1"},                "诊断总览：八卡全离线（真机当前状态）"),
    ("empty-4.3-diag-imu",  {"PK_SIM_PAGE": "diag",
                             "PK_SIM_EMPTY": "1",
                             "PK_SIM_DIAG_DETAIL": "0"},          "诊断详情：离线也要给出下一步"),
    ("empty-4.3-settings",  {"PK_SIM_PAGE": "settings",
                             "PK_SIM_EMPTY": "1",
                             "PK_SIM_SET_SCROLL": "400"},         "设置页无卡：存储行与格式化按钮置灰"),

    # ── 正常态（演示模式关）─────────────────────────────────────────
    #
    # 上面两组一组压极值、一组压全空，唯独「有数据、一切正常」这一档从来没有
    # 基线图——而演示模式的全部意义就是把这一档搬到没接外设的桌面上。开与关
    # 两版必须成对存在，否则没法一眼比出「多出来的只有那两块标识」。
    ("ui-4.3-traffic",      {"PK_SIM_PAGE": "traffic"},           "交通页正常态"),
    ("ui-4.3-list",         {"PK_SIM_PAGE": "list"},              "看板正常态"),
    ("ui-4.3-diag",         {"PK_SIM_PAGE": "diag",
                             "PK_SIM_DIAG_OK": "1"},              "诊断总览正常态"),
    ("ui-4.3-diag-rec-fail", {"PK_SIM_PAGE": "diag",
                             "PK_SIM_DIAG_OK": "1",
                             "PK_SIM_REC_TIER": "2",
                             "PK_SIM_REC_FAIL": "2"},             "阶段5b：LOG卡片接入REC降级+失效sink数，核对不挤爆前半句"),
    ("ui-4.3-settings",     {"PK_SIM_PAGE": "settings",
                             "PK_SIM_SET_SCROLL": "400"},         "设置页底部：演示模式那一行（关）"),

    # ── 演示模式开 ──────────────────────────────────────────────────
    #
    # 逐页各截一张，中英各一版。**这不是为了好看**：演示模式的安全底线是
    # 「开着就一定看得见」，而那枚徽标与红框画在控件层、与各页的绘制代码互不
    # 相干——唯一能证明「每一页都在」的办法就是每一页都截一张。少截一页，就是
    # 那一页可能没有标识而没人知道。
    ("demo-4.3-pfd",        {"PK_SIM_DEMO": "1"},                 "演示模式 · PFD"),
    ("demo-4.3-pfd-en",     {"PK_SIM_DEMO": "1",
                             "PK_SIM_LANG": "en"},                "演示模式 · PFD（英文）"),
    ("demo-4.3-traffic",    {"PK_SIM_DEMO": "1",
                             "PK_SIM_PAGE": "traffic"},           "演示模式 · 交通页"),
    ("demo-4.3-traffic-en", {"PK_SIM_DEMO": "1",
                             "PK_SIM_PAGE": "traffic",
                             "PK_SIM_LANG": "en"},                "演示模式 · 交通页（英文）"),
    ("demo-4.3-list",       {"PK_SIM_DEMO": "1",
                             "PK_SIM_PAGE": "list"},              "演示模式 · 看板"),
    ("demo-4.3-list-en",    {"PK_SIM_DEMO": "1",
                             "PK_SIM_PAGE": "list",
                             "PK_SIM_LANG": "en"},                "演示模式 · 看板（英文）"),
    ("demo-4.3-diag",       {"PK_SIM_DEMO": "1",
                             "PK_SIM_PAGE": "diag",
                             "PK_SIM_DIAG_OK": "1"},              "演示模式 · 诊断总览"),
    ("demo-4.3-diag-en",    {"PK_SIM_DEMO": "1",
                             "PK_SIM_PAGE": "diag",
                             "PK_SIM_DIAG_OK": "1",
                             "PK_SIM_LANG": "en"},                "演示模式 · 诊断总览（英文）"),
    ("demo-4.3-settings",   {"PK_SIM_DEMO": "1",
                             "PK_SIM_PAGE": "settings",
                             "PK_SIM_SET_SCROLL": "400"},         "演示模式 · 设置页（开关已打开）"),
    ("demo-4.3-settings-en",{"PK_SIM_DEMO": "1",
                             "PK_SIM_PAGE": "settings",
                             "PK_SIM_SET_SCROLL": "400",
                             "PK_SIM_LANG": "en"},                "演示模式 · 设置页（英文）"),
    # 开机画面单独一张：它早于 LVGL，控件层那枚徽标盖不到，横幅是另画的一份。
    # 演示模式跨重启存活，这一屏是用户重新上电后第一个知情点。
    ("demo-4.3-splash",     {"PK_SIM_DEMO": "1",
                             "PK_SIM_PAGE": "splash"},            "演示模式 · 开机画面横幅"),
    ("demo-4.3-splash-en",  {"PK_SIM_DEMO": "1",
                             "PK_SIM_PAGE": "splash",
                             "PK_SIM_LANG": "en"},                "演示模式 · 开机画面横幅（英文）"),
    ("ui-4.3-splash",       {"PK_SIM_PAGE": "splash"},            "开机画面（演示模式关）"),

    # ── SD 离线地图页 ────────────────────────────────────────────────
    #
    # PK_SIM_SET_SD=1：地图页第一步就检查 pk_sdcard_is_mounted()，不给这个
    # 开关整页只会是"插入 microSD"提示（page_stub.c 里 SD 默认不挂载，与真机
    # 出厂开机一致，见该文件注释）。
    #
    # PK_SIM_MAPS_DIR：sim/compat/pk_tile_loader_sim.c 同步实现读的目录，
    # 默认 datafiles/maps（4 个真实 pmtiles 包：global z0-9、cn/us_conus
    # z10-12、prd_pilot 珠三角试点包 z0-12，全球+珠三角都覆盖）。这里显式
    # 写出绝对路径而不是依赖默认值，避免 capture.py 的 cwd 假设跟 sim 二进制
    # 的默认值悄悄脱节。
    #
    # own_ship 落在 prd_pilot 试点包范围内（22.54N,113.90E，见
    # sim/compat/mock_runtime.c 的 MAP_DEMO_OWN_LAT/LON 注释），Z10 正好是
    # 该包的高清区间，默认场景应该是清晰底图，不是 overzoom 马赛克。
    ("ui-4.3-map",          {"PK_SIM_PAGE": "map",
                             "PK_SIM_SET_SD": "1",
                             "PK_SIM_MAPS_DIR": str(REPO / "datafiles" / "maps")},
                                                                   "地图页正常态：底图 + 本机 + 5 个 ADS-B 目标"),
    # 2026-08-03：north-up / heading-up 对照组。PK_SIM_HDG 把航向钉在 60°
    # （不是 0/90/180/270 这种巧合角度，转没转、转对没对一眼能分辨）。
    # north-up 那张应该与 ui-4.3-map 视觉上几乎一致（航向只影响本机符号的
    # 指向，不影响底图/铬层）；heading-up 那张底图要整体转 60°、本机符号
    # 应该垂直指向屏幕正上方、指北箭头应偏离正上方 60°、比例尺/按钮/顶栏
    # 位置与 north-up 那张完全一致（铬层不旋转）。
    ("ui-4.3-map-northup",  {"PK_SIM_PAGE": "map",
                             "PK_SIM_SET_SD": "1",
                             "PK_SIM_MAPS_DIR": str(REPO / "datafiles" / "maps"),
                             "PK_SIM_ORIENT": "north",
                             "PK_SIM_HDG": "60"},
                                                                   "地图朝向=north-up：底图轴对齐，本机符号指向 60°航向"),
    ("ui-4.3-map-headingup", {"PK_SIM_PAGE": "map",
                             "PK_SIM_SET_SD": "1",
                             "PK_SIM_MAPS_DIR": str(REPO / "datafiles" / "maps"),
                             "PK_SIM_ORIENT": "heading",
                             "PK_SIM_HDG": "60"},
                                                                   "地图朝向=heading-up：底图整体反向旋转 60°，本机符号垂直"
                                                                   "指向屏幕正上方，指北箭头偏离正上方 60°，铬层不转"),
    ("ui-4.3-map-clump",    {"PK_SIM_PAGE": "map",
                             "PK_SIM_SET_SD": "1",
                             "PK_SIM_MAPS_DIR": str(REPO / "datafiles" / "maps"),
                             "PK_SIM_MAP_CLUMP": "1"},
                                                                   "地图页目标扎堆：验证标签防遮挡（只留一个标签）"),
    ("ui-4.3-map-no-gps",   {"PK_SIM_PAGE": "map",
                             "PK_SIM_SET_SD": "1",
                             "PK_SIM_MAPS_DIR": str(REPO / "datafiles" / "maps"),
                             "PK_SIM_NO_OWN": "1"},
                                                                   "地图页无 GPS：无本机符号，视口停在上次/默认中心"),
    # 目录存在与否都行——pk_map_store_scan 对 opendir 失败只是记一条警告后
    # 返回 0 个包，跟"目录存在但没有 .pmtiles"是同一个降级结果，不用真的
    # mkdir 一个空目录出来。
    ("ui-4.3-map-no-pack",  {"PK_SIM_PAGE": "map",
                             "PK_SIM_SET_SD": "1",
                             "PK_SIM_MAPS_DIR": str(BUILD / "no-such-maps-dir")},
                                                                   "地图页无有效包：SD 有卡但 /maps 下没有 pmtiles"),
    # 悉尼：四个真实包里只有 global（z0-9）覆盖，cn/us_conus/prd_pilot 都够
    # 不到。Z10 请求会从 global 的 z9 回退，触发 route.scale=2 的"越级放大"
    # 提示——这与「缩放拉到超出包数据」是同一件事：不是 UI 缩放挡位超限
    # （挡位上限 12，见 map_page.c 的 MAP_ZOOM_MAX），而是当前位置的底图精度
    # 不够深，provider 只能用更粗的父瓦片放大凑数。
    ("ui-4.3-map-overzoom", {"PK_SIM_PAGE": "map",
                             "PK_SIM_SET_SD": "1",
                             "PK_SIM_MAPS_DIR": str(REPO / "datafiles" / "maps"),
                             "PK_SIM_MAP_OWN_LAT": "-33.9",
                             "PK_SIM_MAP_OWN_LON": "151.2"},
                                                                   "地图页 overzoom：只有全球包覆盖，父瓦片放大 + 提示徽标"),
    # 阶段 4c：地面目标符号（本机内引擎）。三架贴着停机坪/滑行道（空心剪影，
    # 无气压高度）与两架空中目标（实心剪影）混排在同一屏，验证"一眼可辨"——
    # 分开两张各截一种反而验不出对比度，见 mock_runtime.c map_demo_traffic()
    # 的 PK_SIM_MAP_GROUND 分支注释。
    ("ui-4.3-map-ground",   {"PK_SIM_PAGE": "map",
                             "PK_SIM_SET_SD": "1",
                             "PK_SIM_MAPS_DIR": str(REPO / "datafiles" / "maps"),
                             "PK_SIM_MAP_GROUND": "1"},
                                                                   "地图页地面目标：空心剪影(地面) 与 实心剪影(空中) 混排对照，"
                                                                   "本机相位 unknown(默认)——两侧都不压暗，同时验地面目标的"
                                                                   "独立色相在这片有真实道路的底图上是否与路网混淆"),

    # 阶段 4d：显著性跟随本机相位。同一份地面态数据（三架地面+两架空中）分别
    # 用本机在地面 / 在空中两种相位截图，只对比压暗方向对不对——数据、镜头
    # 位置全部与 ui-4.3-map-ground 一致，唯一变量是 PK_SIM_OWN_PHASE。
    ("ui-4.3-map-phase-ground", {"PK_SIM_PAGE": "map",
                             "PK_SIM_SET_SD": "1",
                             "PK_SIM_MAPS_DIR": str(REPO / "datafiles" / "maps"),
                             "PK_SIM_MAP_GROUND": "1",
                             "PK_SIM_OWN_PHASE": "ground"},
                                                                   "地图页显著性：本机在地面——地面目标全亮，空中目标压暗 45%"),
    ("ui-4.3-map-phase-air",    {"PK_SIM_PAGE": "map",
                             "PK_SIM_SET_SD": "1",
                             "PK_SIM_MAPS_DIR": str(REPO / "datafiles" / "maps"),
                             "PK_SIM_MAP_GROUND": "1",
                             "PK_SIM_OWN_PHASE": "airborne"},
                                                                   "地图页显著性：本机在空中——空中目标全亮，地面目标压暗 45%"),
]


def run(cmd: list[str], **kw) -> None:
    subprocess.run(cmd, check=True, capture_output=True, **kw)


def ensure_sim() -> Path:
    """配置并构建模拟器，返回可执行文件路径。

    用独立的 build-capture 目录：开发时常在 sim/build* 里切分辨率做实验，
    截图必须固定在 800×480，不能受那些实验状态影响。
    """
    run(["cmake", "-S", str(SIM_DIR), "-B", str(BUILD), "-DPANEL=800x480"])
    run(["cmake", "--build", str(BUILD), "-j8"])
    exe = BUILD / "pkbox_sim"
    if not exe.exists():
        sys.exit(f"构建后仍找不到 {exe}")
    return exe


def capture(exe: Path, name: str, env_extra: dict[str, str], tmp: Path) -> Path:
    bmp = tmp / f"{name}.bmp"
    env = {**os.environ, **env_extra}
    run([str(exe), "--shot", AT_SEC, str(bmp)], env=env)

    png = OUT / f"{name}.png"
    # -strip 去掉时间戳等元数据：否则内容没变的图每次也会产生 git diff。
    run(["magick", str(bmp), "-strip", str(png)])
    return png


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", help="只出匹配该子串的场景")
    args = ap.parse_args()

    if shutil.which("magick") is None:
        sys.exit("需要 ImageMagick（magick）——本项目的字体/图标生成也依赖它")

    exe = ensure_sim()
    OUT.mkdir(exist_ok=True)
    tmp = BUILD / "shots"
    tmp.mkdir(exist_ok=True)

    scenes = [s for s in SCENES if not args.only or args.only in s[0]]
    if not scenes:
        sys.exit(f"没有场景匹配 --only {args.only!r}")

    print(f"定格于 t={AT_SEC}s，共 {len(scenes)} 张 → {OUT.relative_to(REPO)}/\n")
    for name, env_extra, desc in scenes:
        png = capture(exe, name, env_extra, tmp)
        kb = png.stat().st_size / 1024
        print(f"  {name:24s} {kb:6.1f} KB   {desc}")

    print("\n完成。UI 改动后重跑本脚本，再用 git diff 看哪些场景受影响。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
