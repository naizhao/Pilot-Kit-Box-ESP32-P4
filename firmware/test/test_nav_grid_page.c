/* test_nav_grid_page.c — host proof for 导航网格的纯函数区（分页切分 / 命中判定）。
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/main -DPK_NAV_GRID_HOST_TEST \
 *      -o /tmp/test_navgrid firmware/test/test_nav_grid_page.c && /tmp/test_navgrid
 *
 * 同 test_apt_detail_page.c / test_search_page.c 的翻译单元惯例：把被测 .c
 * 直接拉进来，用 PK_NAV_GRID_HOST_TEST 切掉要 IDF 的渲染与触摸状态机两段
 * （本任务尚未实现那两段，纯函数区先落地）。
 *
 * 六段，每一段都对着一个"版面评审拍板过、代码里不能悄悄改回去"的坑：
 *   1) 分页切分——航空电台占第 1 页原空白第 8 格，第 2 页仍从诊断开始；
 *   2) 格子位置跨页对齐——同一坐标在两页上必须落到"首格"这同一个语义位，
 *      不许因为项数不同就重新居中（产品负责人 2026-08-02 明确否掉过这条）；
 *   3) 空格不可点——第 2 页第 5 格没有内容，点它不能算出越界的 index；
 *   4) 动作条三分——调平/亮度/关闭三等分 800 px 宽；
 *   5) pop 打开时网格整层吞掉触摸——压暗的东西不能被点中，pop 自己的三个
 *      档位按钮要能精确命中；
 *   6) x 越界——触摸驱动理论上不给越界坐标，但 Task 5 的拖动会算出相对
 *      坐标，负值不是不可能：C 的整数除法向零取整，x=-250 会算出
 *      col=-1、slot=-1，`slot >= count` 挡不住负数，调用方拿 -1 去索引
 *      items 数组就是越界读。x 偏大同理会算出 col>=COLS，误命中不存在的格。
 */
#include <stdio.h>

#include "../main/nav_grid_page.c"

static int g_fail;

static void chk_int(const char *what, int got, int want)
{
    if (got != want) { printf("FAIL %s: got %d want %d\n", what, got, want); g_fail++; }
}

static void chk_true(const char *what, bool got)
{
    if (!got) { printf("FAIL %s: 期望 true\n", what); g_fail++; }
}

/* ── 1) 分页切分 ──────────────────────────────────────────────────
 * 第 1 页 8 项、第 2 页 4 项；航空电台占用原先预留的第 8 格。 */
static void test_paging(void)
{
    chk_int("第 1 页起点", pk_nav_page_first(0), 0);
    chk_int("第 1 页项数", pk_nav_page_count(0), 8);
    chk_int("第 2 页起点", pk_nav_page_first(1), 8);
    /* 2026-09-12 加了「电源」（关机/重启），第 2 页 3 → 4 项。 */
    chk_int("第 2 页项数", pk_nav_page_count(1), 4);
}

/* ── 2) 格子位置在两页之间必须完全对齐 ───────────────────────────
 * 产品负责人 2026-08-02 明确否掉了"末页居中"：位置随项数浮动会让同一功能
 * 在两页落在不同坐标，手指记不住位置——而记住位置正是网格相对列表的
 * 全部优势。钉死在测试里。 */
static void test_cell_position_stable(void)
{
    const int x = 100, y = PK_NAV_BAR_BOT + 40;
    pk_nav_hit_t a = pk_nav_hit_test(x, y, 0, false);
    pk_nav_hit_t b = pk_nav_hit_test(x, y, 1, false);
    chk_int("第 1 页首格", a.kind, PK_NAV_HIT_CELL);
    chk_int("第 1 页首格 index", a.index, 0);
    chk_int("第 2 页同坐标也是首格", b.kind, PK_NAV_HIT_CELL);
    chk_int("第 2 页首格 index", b.index, 8);
}

/* ── 2b) 长标签必须降档留在格子边框内 ────────────────────────────
 * 生产回归：英文 Recordings 在 L 档是 10×21=210 px，而 200 px 格子扣掉
 * 左右各 16 px 的边框呼吸区只剩 168 px。继续用 L 会同时压出格子和选中框。
 * 期望值由生成字体的固定 cell 尺寸手算，不调用被测逻辑构造。
 * 突变哨兵：删掉 L→M 降档分支，第一条必须红。 */
static void test_long_label_uses_largest_fitting_size(void)
{
    chk_int("Recordings 210px 超宽时降到 M(150px)",
            pk_nav_label_fit(210, 150, 168),
            PK_NAV_LABEL_MEDIUM);
    chk_int("Settings 168px 刚好装下时保留 L",
            pk_nav_label_fit(168, 120, 168),
            PK_NAV_LABEL_LARGE);
    chk_int("极长标签 M 仍超宽时继续降到 S",
            pk_nav_label_fit(252, 180, 168),
            PK_NAV_LABEL_SMALL);
    chk_int("录音中文 64px 保留 L",
            pk_nav_label_fit(64, 44, 168),
            PK_NAV_LABEL_LARGE);
}

/* 第 1 页原本留空的第 8 格现在是航空电台。缺这一格就只能从机场详情或
 * 诊断页间接进入，用户在主导航中找不到功能。 */
static void test_airband_is_reachable_from_primary_navigation(void)
{
    const int x = 3 * PK_NAV_CELL_W + PK_NAV_CELL_W / 2;
    const int y = PK_NAV_BAR_BOT + PK_NAV_CELL_H + PK_NAV_CELL_H / 2;
    const pk_nav_hit_t h = pk_nav_hit_test(x, y, 0, PK_NAV_POP_NONE);
    chk_int("第一页第 8 格可点", h.kind, PK_NAV_HIT_CELL);
    chk_int("第一页第 8 格是 AIRBAND index", h.index, 7);
}

/* ── 3) 第 2 页 4 项，第 5 格起是空的 ────────────────────────────
 * 点空格必须什么都不发生，不能落到 index 11（越界）。
 * 第 4 格（电源）自 2026-09-12 起有内容且可点——顺带钉住它，防止哪天
 * 第 2 页项数变了又把「空格」判据错位到有内容的格上。 */
static void test_empty_cell(void)
{
    const int x4 = 3 * PK_NAV_CELL_W + 10, y = PK_NAV_BAR_BOT + 40;
    pk_nav_hit_t h4 = pk_nav_hit_test(x4, y, 1, PK_NAV_POP_NONE);
    chk_int("第 2 页第 4 格是电源，可点", h4.kind, PK_NAV_HIT_CELL);
    chk_int("第 2 页第 4 格 index", h4.index, 11);

    /* 第 5 格 = 第 2 行第 1 列（slot 4），第 2 页只有 4 项 → 空。 */
    const int y2 = PK_NAV_BAR_BOT + PK_NAV_CELL_H + 10;
    chk_int("第 2 页第 5 格是空的",
            pk_nav_hit_test(10, y2, 1, PK_NAV_POP_NONE).kind, PK_NAV_HIT_NONE);
}

/* ── 3b) 电源 pop：两个按钮各自命中，点别处收起 ──────────────────
 * 与亮度 pop 同一条铁律——"看得见的必须是点得中的"，几何宏两边共用。
 * 突变哨兵：把 PWR_BW 改掉而不改渲染，这里会红。 */
static void test_power_pop(void)
{
    const int cy = PWR_Y0 + PWR_BH / 2;
    pk_nav_hit_t off = pk_nav_hit_test(PWR_X0 + PWR_BW / 2, cy, 1,
                                       PK_NAV_POP_POWER);
    chk_int("点「关机」", off.kind, PK_NAV_HIT_POWER_OFF);

    pk_nav_hit_t rst = pk_nav_hit_test(PWR_X0 + PWR_BW + PWR_PAD + PWR_BW / 2,
                                       cy, 1, PK_NAV_POP_POWER);
    chk_int("点「重启」", rst.kind, PK_NAV_HIT_POWER_RESTART);

    /* 电源 pop 开着时，底下的网格整层不可点（同亮度 pop）。 */
    chk_int("pop 开着时网格被吞掉",
            pk_nav_hit_test(100, PK_NAV_BAR_BOT + 40, 1,
                            PK_NAV_POP_POWER).kind, PK_NAV_HIT_NONE);

    /* 两个面板都在屏幕居中，按钮区必然**重叠**——所以"同一坐标在两个 pop
     * 下命中不同按钮"是正确行为，不是 bug。要钉死的是不串**语义**：电源
     * pop 开着时，无论点哪儿都不可能得到亮度档位那个 kind，否则一次误判
     * 就会把"关机"执行成"调亮度"（或反过来，那更糟）。 */
    chk_true("电源 pop 下绝不返回亮度档位 kind",
             pk_nav_hit_test(270, 340, 1, PK_NAV_POP_POWER).kind
                 != PK_NAV_HIT_BRIGHT_STEP);
    chk_true("亮度 pop 下绝不返回关机/重启 kind",
             pk_nav_hit_test(270, 340, 1, PK_NAV_POP_BRIGHT).kind
                     != PK_NAV_HIT_POWER_OFF &&
             pk_nav_hit_test(270, 340, 1, PK_NAV_POP_BRIGHT).kind
                     != PK_NAV_HIT_POWER_RESTART);
}

/* ── 3c) 关机返回代表失败，画面必须留下可见恢复路径 ───────────────
 * 真正仅电池关机成功后 MCU 当场掉电，函数不会返回；能返回 false 就是
 * “系统仍活着”。2026-09-28 H1 下游供电实测曾先关面板再忽略 false，留下
 * 黑屏/背光/LED 常亮。这里钉住用户可见结果，而不是检查源码关键词。
 * 突变哨兵：把 FAILED_VISIBLE 退成 POWER_CUT，测试必须红。 */
static void test_poweroff_returned_failure_stays_visible(void)
{
    enum {
        WANT_BLOCKED_NEED_UNPLUG = 0,
        WANT_POWER_CUT = 1,
        WANT_FAILED_VISIBLE = 2,
    };

    chk_int("U19 VBUS 在位时不尝试关机",
            pk_nav_poweroff_outcome(true, false),
            WANT_BLOCKED_NEED_UNPLUG);
    chk_int("关机调用返回 false 必须显示失败",
            pk_nav_poweroff_outcome(false, false),
            WANT_FAILED_VISIBLE);
    chk_int("只有执行器确认成功才算电源已切断",
            pk_nav_poweroff_outcome(false, true),
            WANT_POWER_CUT);
}

/* ── 4) 动作条三分 ────────────────────────────────────────────── */
static void test_action_bar(void)
{
    const int y = PK_NAV_ACT_TOP + 40;
    chk_int("调平", pk_nav_hit_test(100, y, 0, false).kind, PK_NAV_HIT_LEVEL);
    chk_int("亮度", pk_nav_hit_test(400, y, 0, false).kind, PK_NAV_HIT_BRIGHT);
    chk_int("关闭", pk_nav_hit_test(700, y, 0, false).kind, PK_NAV_HIT_CLOSE);
}

/* ── 5) pop 打开时网格整层不可点 ─────────────────────────────────
 * 屏上它已被压暗，点得中就是"看得见的不是点得中的"。 */
static void test_pop_swallows_grid(void)
{
    chk_int("pop 开着时点网格不命中",
            pk_nav_hit_test(100, PK_NAV_BAR_BOT + 40, 0, true).kind, PK_NAV_HIT_NONE);
    chk_int("点「低」", pk_nav_hit_test(270, 340, 0, true).kind, PK_NAV_HIT_BRIGHT_STEP);
    chk_int("「低」= 档 0", pk_nav_hit_test(270, 340, 0, true).index, 0);
    chk_int("「中」= 档 1", pk_nav_hit_test(400, 340, 0, true).index, 1);
    chk_int("「高」= 档 2", pk_nav_hit_test(530, 340, 0, true).index, 2);
}

/* ── 6) x 越界 ────────────────────────────────────────────────────
 * 触摸驱动理论上不给越界坐标，但 Task 5 的拖动逻辑会算出相对坐标，
 * 负值不是不可能。y 方向已经挡住了（PK_NAV_BAR_BOT 与网格区下沿两条），
 * x 方向必须对齐同一语义：越界一律 PK_NAV_HIT_NONE。 */
static void test_x_out_of_bounds(void)
{
    const int y = PK_NAV_BAR_BOT + 40;   /* 落在第 1 行 */

    /* 右侧屏幕外：x=850 时 col = 850/200 = 4，若不挡会被当成第 5 列，
     * slot = 0*4+4 = 4，「4 >= 7」为假，误命中第 5 项（index=4）。 */
    chk_int("x=850 右侧越界不命中",
            pk_nav_hit_test(850, y, 0, false).kind, PK_NAV_HIT_NONE);

    /* 左侧越界：C 整数除法向零取整，x=-250 → col = -1 → slot = -1，
     * 「-1 >= 7」为假，会返回 index = -1（越界读的源头）。 */
    chk_int("x=-250 左侧越界不命中",
            pk_nav_hit_test(-250, y, 0, false).kind, PK_NAV_HIT_NONE);

    /* 合法边界必须仍然命中，不能被越界修复误伤。 */
    chk_int("x=0 最左合法边界命中第一格",
            pk_nav_hit_test(0, y, 0, false).kind, PK_NAV_HIT_CELL);
    chk_int("x=0 命中 index 0",
            pk_nav_hit_test(0, y, 0, false).index, 0);
    chk_int("x=799 最右合法边界命中最后一列",
            pk_nav_hit_test(799, y, 0, false).kind, PK_NAV_HIT_CELL);
    chk_int("x=799 命中 index 3（第 1 行第 4 列）",
            pk_nav_hit_test(799, y, 0, false).index, 3);

    /* 动作条那段同样要挡：x=850 落进「x < third*2 之外」分支，若不挡会被
     * 误判成 CLOSE；x=-250 落进「x < third」分支，会被误判成 LEVEL。 */
    const int ay = PK_NAV_ACT_TOP + 40;
    chk_int("动作条 x=850 越界不命中",
            pk_nav_hit_test(850, ay, 0, false).kind, PK_NAV_HIT_NONE);
    chk_int("动作条 x=-250 越界不命中",
            pk_nav_hit_test(-250, ay, 0, false).kind, PK_NAV_HIT_NONE);
}

/* ── 7) 未实现的页面先置灰占位而不是从版面拿掉 ───────────────────
 * 格子位置要永久钉死，将来补上这两个页面时版面一个数都不用改
 * （产品负责人 2026-08-02 定）。置灰项必须不可点：命中判定要挡住它们，
 * 否则会切到一个不存在的 mode。 */
static void test_disabled_items(void)
{
    chk_true("PFD 可点",    pk_nav_item_enabled(0));
    chk_true("搜索可点",    pk_nav_item_enabled(4));
    chk_true("录音可点",    pk_nav_item_enabled(5));
    chk_true("工具不可点", !pk_nav_item_enabled(6));
    chk_true("诊断可点",    pk_nav_item_enabled(7));
    chk_true("关于可点",    pk_nav_item_enabled(9));

    /* 越界一律当不可用，别让调用方拿越界 index 去查表。 */
    chk_true("越界不可点", !pk_nav_item_enabled(-1));
    chk_true("越界不可点", !pk_nav_item_enabled(PK_NAV_ITEM_CNT));
}

/* ── 8) 点在置灰格上：命中判定必须返回 NONE，不能返回 CELL ────────── */
static void test_recordings_reachable_and_tools_disabled(void)
{
    /* 第 1 页第 6 格（index 5「录音」）= 第 2 行第 2 列。 */
    const int x = 1 * PK_NAV_CELL_W + 10;
    const int y = PK_NAV_BAR_BOT + PK_NAV_CELL_H + 10;
    pk_nav_hit_t h = pk_nav_hit_test(x, y, 0, false);
    chk_int("点「录音」命中", h.kind, PK_NAV_HIT_CELL);
    chk_int("命中的是 index 5", h.index, 5);

    /* 相邻的工具仍是未实现占位，必须 disabled 且不产生错误动作。 */
    pk_nav_hit_t off = pk_nav_hit_test(2 * PK_NAV_CELL_W + 10, y, 0, false);
    chk_int("点「工具」不命中", off.kind, PK_NAV_HIT_NONE);
}

/* ── 9) 滑动翻页的阈值 ───────────────────────────────────────────
 * 横向位移够大、且明显大于纵向位移，才算翻页——斜着划不该翻页，
 * 手抖几像素更不该。返回 -1 上一页 / +1 下一页 / 0 不翻。 */
static void test_swipe(void)
{
    chk_int("右划回上一页", pk_nav_swipe_dir(90, 5), -1);
    chk_int("左划到下一页", pk_nav_swipe_dir(-90, 5), 1);
    chk_int("位移不够不翻", pk_nav_swipe_dir(30, 5), 0);
    chk_int("斜划不翻", pk_nav_swipe_dir(90, 80), 0);
    chk_int("纯竖划不翻", pk_nav_swipe_dir(0, 120), 0);
}

int main(void)
{
    test_paging();
    test_cell_position_stable();
    test_long_label_uses_largest_fitting_size();
    test_airband_is_reachable_from_primary_navigation();
    test_empty_cell();
    test_power_pop();
    test_poweroff_returned_failure_stays_visible();
    test_action_bar();
    test_pop_swallows_grid();
    test_x_out_of_bounds();
    test_disabled_items();
    test_recordings_reachable_and_tools_disabled();
    test_swipe();
    printf("%s (%d fail)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
