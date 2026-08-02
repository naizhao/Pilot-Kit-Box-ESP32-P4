/*
 * nav_grid_page.c — 见 nav_grid_page.h。
 *
 * 为什么是模态层、为什么自绘不用 LVGL：见头文件。这里只再强调一句版面
 * 来源——本文件的常量与命中判定几何**必须**与 `sim/proto-navgrid/proto.c`
 * 的 `render_c()` / `draw_bright_pop()` 保持一致，那份原型是评审通过的
 * 版面真源，改版面先改原型再抄回来，不要在这里独立发挥。
 *
 * Task 2 范围：只做纯函数区（分页切分 + 命中判定），渲染与触摸状态机
 * （打开/关闭网格、拖动切页、亮度 pop 的开合）留给后续任务，平台区先留
 * `#ifndef PK_NAV_GRID_HOST_TEST` 的结构占位，暂时是空的。
 */
#include "nav_grid_page.h"

/* ═══════════════════════════════════════════════════════════════════
 * 纯函数区（无 OS / 无全局状态）——host 单测直接把本文件拉进翻译单元。
 * ═══════════════════════════════════════════════════════════════════ */

int pk_nav_page_first(int page) { return page ? PK_NAV_PAGE1_CNT : 0; }

int pk_nav_page_count(int page)
{
    return page ? (PK_NAV_ITEM_CNT - PK_NAV_PAGE1_CNT) : PK_NAV_PAGE1_CNT;
}

pk_nav_hit_t pk_nav_hit_test(int x, int y, int page, bool pop_open)
{
    pk_nav_hit_t r = { PK_NAV_HIT_NONE, -1 };

    if (pop_open) {
        /* 面板几何照 draw_bright_pop()：三按钮 120×64、间距 10，四边留白
         * 同一个 pad=10。留白必须由同一个 pad 推出来——两头各算各的会得到
         * 上 10 下 20（proto.c 里记过的初版错法）。 */
        const int bw = 120, bh = 64, pad = 10;
        const int w = bw * 3 + pad * 2;
        const int x0 = 400 - w / 2;
        const int y0 = PK_NAV_ACT_TOP - 12 - (bh + pad * 2) + pad;
        for (int i = 0; i < 3; ++i) {
            const int bx = x0 + i * (bw + pad);
            if (x >= bx && x < bx + bw && y >= y0 && y < y0 + bh) {
                r.kind = PK_NAV_HIT_BRIGHT_STEP;
                r.index = i;
                return r;
            }
        }
        return r;   /* 点别处 = 收起 pop，由调用方处理 */
    }

    /* x 越界（<0 或 >=800）在网格与动作条两段都要挡：C 的整数除法向零
     * 取整，负 x 会算出负 col/负 slot，`slot >= count` 这类上界检查挡不住
     * 负数；x 偏大则会算出 col>=COLS，落进不存在的列。两段用同一条判据，
     * 与 y 方向已有的边界检查（PK_NAV_BAR_BOT / 网格区下沿）对齐。 */
    if (x < 0 || x >= 800) return r;

    if (y >= PK_NAV_ACT_TOP) {
        const int third = 800 / 3;
        r.kind = (x < third) ? PK_NAV_HIT_LEVEL
               : (x < third * 2) ? PK_NAV_HIT_BRIGHT : PK_NAV_HIT_CLOSE;
        return r;
    }

    if (y < PK_NAV_BAR_BOT || y >= PK_NAV_BAR_BOT + PK_NAV_ROWS * PK_NAV_CELL_H)
        return r;

    const int col = x / PK_NAV_CELL_W;
    const int row = (y - PK_NAV_BAR_BOT) / PK_NAV_CELL_H;
    const int slot = row * PK_NAV_COLS + col;
    if (slot >= pk_nav_page_count(page)) return r;   /* 空格什么都不做 */

    r.kind = PK_NAV_HIT_CELL;
    r.index = pk_nav_page_first(page) + slot;
    return r;
}

#ifndef PK_NAV_GRID_HOST_TEST

/* ═══════════════════════════════════════════════════════════════════
 * 平台区：打开/关闭 + 渲染 + 触摸状态机
 * 留待后续任务（Task 4 渲染 / Task 5 触摸）实现。
 * ═══════════════════════════════════════════════════════════════════ */

#endif /* !PK_NAV_GRID_HOST_TEST */
