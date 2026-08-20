/* test_touch_arbiter.c — host proof for pk_touch_arbiter.
 *   cc -std=c11 -O2 -I firmware/main -o /tmp/test_ta firmware/test/test_touch_arbiter.c && /tmp/test_ta
 *
 * 这组用例复刻的是 2026-08-01 在真机上撞到的那一下：列表页按住右侧 FAB
 * 想拖到左边，手指划过列表内容区右缘之后，列表把按压抢走、FAB 脱手、整张表
 * 反倒跟着手指滚。判据不是「像素对不对」，而是**一次按压的归属有没有中途改变**。
 */
#include <stdbool.h>
#include <stdio.h>
#include "../main/pk_touch_arbiter.c"

static int g_fail = 0;

static void chk(const char *w, int got, int want)
{
    const bool ok = (got == want);
    printf("  [%s] %-46s got=%d want=%d\n", ok ? "PASS" : "FAIL", w, got, want);
    if (!ok) g_fail++;
}

/* 列表页内容区右缘（adsb_list.c 的 CONTENT_R + 8）。FAB 落在它右边，
 * 所以按在 FAB 上时列表不吃、按到它左边列表就吃——正是易主的那条线。 */
#define LIST_R  724

/* 模拟 touch_gt911.c 里的那次命中判定：坐标落在列表区内就吃掉。 */
static bool list_hittest(int x) { return x < LIST_R; }

int main(void)
{
    /* ── 用例 1：按在 FAB 上往左拖，全程必须归 LVGL ──────────────
     *
     * 这就是那个 bug。修复前的实现每帧重判，手指划过 724 时列表返回 true，
     * 归属被改成 PAGE；修复后按下那一帧定死为 LVGL，之后一路 YIELD。 */
    {
        pk_touch_arbiter_t a = {0};
        int page_got_it = 0;

        /* 第 1 帧：手指落在 FAB 上（x=756，在列表区之外）。 */
        chk("FAB 按下首帧做命中判定",
            pk_touch_arbiter_press(&a), PK_TOUCH_ACTION_HITTEST);
        pk_touch_arbiter_settle(&a, list_hittest(756));

        /* 往左拖，跨过 724 那条线。 */
        for (int x = 756; x >= 100; x -= 20) {
            const pk_touch_action_t act = pk_touch_arbiter_press(&a);
            if (act == PK_TOUCH_ACTION_HITTEST && list_hittest(x)) page_got_it++;
            if (act == PK_TOUCH_ACTION_DRAG)                       page_got_it++;
        }
        chk("拖过列表区后页面仍一次都没抢到", page_got_it, 0);
        chk("归属始终是 LVGL", a.owner, PK_TOUCH_OWNER_LVGL);
    }

    /* ── 用例 2：按在列表上拖，全程归页面 ────────────────────────
     *
     * 反向对称：一次归了页面的按压，手指划到 FAB 上也不许被 LVGL 抢走，
     * 否则滚列表滚到右边就会突然把主菜单拉出来。 */
    {
        pk_touch_arbiter_t a = {0};
        chk("列表按下首帧做命中判定",
            pk_touch_arbiter_press(&a), PK_TOUCH_ACTION_HITTEST);
        pk_touch_arbiter_settle(&a, list_hittest(300));

        /* 手指一路划到 FAB 上（x 到 780，早就出了列表区）。 */
        int yielded = 0;
        for (int x = 300; x <= 780; x += 20) {
            (void)x;
            if (pk_touch_arbiter_press(&a) != PK_TOUCH_ACTION_DRAG) yielded++;
        }
        chk("划到 FAB 上仍每帧走页面 drag", yielded, 0);
        chk("归属始终是页面", a.owner, PK_TOUCH_OWNER_PAGE);
    }

    /* ── 用例 3：松手后重新仲裁 ─────────────────────────────────
     * 归属是「本次按压」的属性，不是全局状态；不清掉的话第二次点击会
     * 继承第一次的归属。 */
    {
        pk_touch_arbiter_t a = {0};
        pk_touch_arbiter_press(&a);
        pk_touch_arbiter_settle(&a, false);          /* 归 LVGL */
        pk_touch_arbiter_release(&a);
        chk("松手后归属清空", a.owner, PK_TOUCH_OWNER_NONE);
        chk("下一次按下重新做命中判定",
            pk_touch_arbiter_press(&a), PK_TOUCH_ACTION_HITTEST);
        pk_touch_arbiter_settle(&a, true);           /* 这次归页面 */
        chk("第二次按压可以归页面", a.owner, PK_TOUCH_OWNER_PAGE);
    }

    /* ── 用例 4：settle 只认第一次 ──────────────────────────────
     * 调用方若在归属已定后再 settle 一次（就是原来那个每帧重判的写法），
     * 必须无效——这是防止 bug 复发的那道闸。 */
    {
        pk_touch_arbiter_t a = {0};
        pk_touch_arbiter_press(&a);
        pk_touch_arbiter_settle(&a, false);          /* 归 LVGL */
        pk_touch_arbiter_settle(&a, true);           /* 再判一次，必须被忽略 */
        chk("归属定了之后 settle 无效", a.owner, PK_TOUCH_OWNER_LVGL);
    }

    /* ── 用例 5：LVGL 浮层强制让路 ──────────────────────────────
     * 浮层盖在页面之上时，页面的命中一律作废（历史场景是横向 dock：不让路
     * 的话点页签的坐标会被列表先吃掉，表现是「进了 list 就切不走页」——
     * bc1d625 修过）。dock 已删，但这条规则本身跟着 force_lvgl 一起留着，
     * 见 pk_touch_arbiter.h 那段说明。 */
    {
        pk_touch_arbiter_t a = {0};
        pk_touch_arbiter_press(&a);
        pk_touch_arbiter_settle(&a, true);           /* 本来归了页面 */
        pk_touch_arbiter_force_lvgl(&a);
        chk("浮层让路后强制归 LVGL", a.owner, PK_TOUCH_OWNER_LVGL);
        chk("此后一路让路",
            pk_touch_arbiter_press(&a), PK_TOUCH_ACTION_YIELD);
    }

    printf(g_fail ? "\n%d 处失败\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
