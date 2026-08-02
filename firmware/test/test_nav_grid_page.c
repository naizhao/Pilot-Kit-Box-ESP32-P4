/* test_nav_grid_page.c — host proof for 导航网格的纯函数区（分页切分 / 命中判定）。
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/main -DPK_NAV_GRID_HOST_TEST \
 *      -o /tmp/test_navgrid firmware/test/test_nav_grid_page.c && /tmp/test_navgrid
 *
 * 同 test_apt_detail_page.c / test_search_page.c 的翻译单元惯例：把被测 .c
 * 直接拉进来，用 PK_NAV_GRID_HOST_TEST 切掉要 IDF 的渲染与触摸状态机两段
 * （本任务尚未实现那两段，纯函数区先落地）。
 *
 * 六段，每一段都对着一个"版面评审拍板过、代码里不能悄悄改回去"的坑：
 *   1) 分页切分——第 1 页 7 项、第 2 页 3 项，不是塞满 8 个换页；
 *   2) 格子位置跨页对齐——同一坐标在两页上必须落到"首格"这同一个语义位，
 *      不许因为项数不同就重新居中（产品负责人 2026-08-02 明确否掉过这条）；
 *   3) 空格不可点——第 2 页第 4 格没有内容，点它不能算出越界的 index；
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
 * 第 1 页 7 项、第 2 页 3 项——不是"4×2=8 塞满才换页"，切分点是常用度。 */
static void test_paging(void)
{
    chk_int("第 1 页起点", pk_nav_page_first(0), 0);
    chk_int("第 1 页项数", pk_nav_page_count(0), 7);
    chk_int("第 2 页起点", pk_nav_page_first(1), 7);
    chk_int("第 2 页项数", pk_nav_page_count(1), 3);
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
    chk_int("第 2 页首格 index", b.index, 7);
}

/* ── 3) 第 2 页只有 3 项，第 4 格是空的 ──────────────────────────
 * 点空格必须什么都不发生，不能落到 index 10（越界）。 */
static void test_empty_cell(void)
{
    const int x = 3 * PK_NAV_CELL_W + 10, y = PK_NAV_BAR_BOT + 40;
    chk_int("第 2 页第 4 格是空的",
            pk_nav_hit_test(x, y, 1, false).kind, PK_NAV_HIT_NONE);
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
    chk_true("记录不可点", !pk_nav_item_enabled(5));
    chk_true("工具不可点", !pk_nav_item_enabled(6));
    chk_true("诊断可点",    pk_nav_item_enabled(7));
    chk_true("关于可点",    pk_nav_item_enabled(9));

    /* 越界一律当不可用，别让调用方拿越界 index 去查表。 */
    chk_true("越界不可点", !pk_nav_item_enabled(-1));
    chk_true("越界不可点", !pk_nav_item_enabled(PK_NAV_ITEM_CNT));
}

/* ── 8) 点在置灰格上：命中判定必须返回 NONE，不能返回 CELL ────────── */
static void test_disabled_cell_not_hittable(void)
{
    /* 第 1 页第 6 格（index 5「记录」）= 第 2 行第 2 列。 */
    const int x = 1 * PK_NAV_CELL_W + 10;
    const int y = PK_NAV_BAR_BOT + PK_NAV_CELL_H + 10;
    pk_nav_hit_t h = pk_nav_hit_test(x, y, 0, false);
    chk_int("点「记录」不命中", h.kind, PK_NAV_HIT_NONE);

    /* 相邻的第 5 格（index 4「搜索」）仍然可点——别一刀切把整行禁掉。 */
    pk_nav_hit_t ok = pk_nav_hit_test(0 * PK_NAV_CELL_W + 10, y, 0, false);
    chk_int("点「搜索」命中", ok.kind, PK_NAV_HIT_CELL);
    chk_int("命中的是 index 4", ok.index, 4);
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
    test_empty_cell();
    test_action_bar();
    test_pop_swallows_grid();
    test_x_out_of_bounds();
    test_disabled_items();
    test_disabled_cell_not_hittable();
    test_swipe();
    printf("%s (%d fail)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
