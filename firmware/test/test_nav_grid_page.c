/* test_nav_grid_page.c — host proof for 导航网格的纯函数区（分页切分 / 命中判定）。
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/main -DPK_NAV_GRID_HOST_TEST \
 *      -o /tmp/test_navgrid firmware/test/test_nav_grid_page.c && /tmp/test_navgrid
 *
 * 同 test_apt_detail_page.c / test_search_page.c 的翻译单元惯例：把被测 .c
 * 直接拉进来，用 PK_NAV_GRID_HOST_TEST 切掉要 IDF 的渲染与触摸状态机两段
 * （本任务尚未实现那两段，纯函数区先落地）。
 *
 * 五段，每一段都对着一个"版面评审拍板过、代码里不能悄悄改回去"的坑：
 *   1) 分页切分——第 1 页 7 项、第 2 页 3 项，不是塞满 8 个换页；
 *   2) 格子位置跨页对齐——同一坐标在两页上必须落到"首格"这同一个语义位，
 *      不许因为项数不同就重新居中（产品负责人 2026-08-02 明确否掉过这条）；
 *   3) 空格不可点——第 2 页第 4 格没有内容，点它不能算出越界的 index；
 *   4) 动作条三分——调平/亮度/关闭三等分 800 px 宽；
 *   5) pop 打开时网格整层吞掉触摸——压暗的东西不能被点中，pop 自己的三个
 *      档位按钮要能精确命中。
 */
#include <stdio.h>

#include "../main/nav_grid_page.c"

static int g_fail;

static void chk_int(const char *what, int got, int want)
{
    if (got != want) { printf("FAIL %s: got %d want %d\n", what, got, want); g_fail++; }
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

int main(void)
{
    test_paging();
    test_cell_position_stable();
    test_empty_cell();
    test_action_bar();
    test_pop_swallows_grid();
    printf("%s (%d fail)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
