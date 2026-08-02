/*
 * pk_touch_arbiter.h — 一次按压归谁：自绘页面，还是 LVGL 控件。
 *
 * 为什么要独立出来
 * ----------------
 * 本机的触摸有两个互不相让的消费者：
 *
 *   - **自绘页面**（交通页的量程钮、列表页的表头与数据区、诊断/设置/关于的
 *     滚动）——它们不是 LVGL 对象，命中判定只能在 indev 的 read_cb 里手工做，
 *     吃掉时把上报状态改成 RELEASED，LVGL 那侧就什么都收不到。
 *   - **LVGL 控件**（FAB、二级页返回栏）——正常走 LVGL 自己的事件通路。
 *
 * 判定本身写在 touch_gt911.c 里没问题，出问题的是**判定的时机**：原先每一帧
 * 都重新判一次，于是一次按压可以中途易主。真机实测到的表现（罩哥 2026-08-01）：
 * 在列表页按住右侧 FAB 想拖到左边，手指一旦划过列表内容区的右缘（x=724），
 * 列表就把这次按压抢了过去——FAB 当场脱手，整张表反而跟着手指滚起来。
 *
 * 同一个坑另一侧也踩过：地图页拖动途中手指划过 FAB，那次拖动被 FAB 抢走，
 * map_page.c 为此自己加了 s_press_active 挡着（见 pk_map_page_touch 的注释）。
 * 每个页面各修各的迟早漏一个，所以把「归属」这件事收到一处来管。
 *
 * 规则只有一条
 * ------------
 * **归属在按下那一刻定死，松手之前不得更改。** 手指按下时若没有任何页面吃掉
 * 它，这次按压就归 LVGL，之后无论手指划到哪里，页面都不再有机会插手；反之
 * 归了页面，LVGL 那侧整个按压期间都收不到。
 *
 * 这条规则也正是 adsb_list.h 早就写下的契约（「按下时 pk_adsb_list_touch()
 * 返回过 true 才需要继续调用 drag」）——只是此前没有一个地方负责执行它。
 *
 * 纯状态机，不碰硬件也不碰 LVGL，好让 host 单测能把各种按压序列跑一遍
 * （firmware/test/test_touch_arbiter.c）。
 */
#pragma once

#include <stdbool.h>

typedef enum {
    /* 手指没按着。下一次按下要重新仲裁。 */
    PK_TOUCH_OWNER_NONE = 0,
    /* 本次按压归自绘页面：后续帧走页面的 drag()，LVGL 收不到。 */
    PK_TOUCH_OWNER_PAGE,
    /* 本次按压归 LVGL 控件：页面在松手前不再做任何命中判定。 */
    PK_TOUCH_OWNER_LVGL,
} pk_touch_owner_t;

typedef struct {
    pk_touch_owner_t owner;
} pk_touch_arbiter_t;

/* 本帧该做什么。read_cb 按返回值决定调页面的 touch() 还是 drag()。 */
typedef enum {
    /* 按下的第一帧：调页面的 touch() 做命中判定，结果回填 pk_touch_arbiter_settle()。 */
    PK_TOUCH_ACTION_HITTEST = 0,
    /* 归属已定为页面：调页面的 drag()。 */
    PK_TOUCH_ACTION_DRAG,
    /* 归属已定为 LVGL：什么都不做，原样把 PRESSED 交给 LVGL。 */
    PK_TOUCH_ACTION_YIELD,
} pk_touch_action_t;

/* 手指按着的每一帧调一次，问「这一帧该做什么」。 */
pk_touch_action_t pk_touch_arbiter_press(pk_touch_arbiter_t *a);

/* 仅在上一步返回 HITTEST 时调用，把命中判定的结果告诉仲裁器：
 * eaten=true → 本次按压归页面；false → 归 LVGL。 */
void pk_touch_arbiter_settle(pk_touch_arbiter_t *a, bool eaten);

/* 松手。归属作废，下一次按下重新仲裁。 */
void pk_touch_arbiter_release(pk_touch_arbiter_t *a);

/* 「页面必须整体让路给某个 LVGL 浮层」的场合：把本次按压强制判给 LVGL，
 * 且在松手之前不再回头。
 *
 * 2026-08-02 起**暂无生产调用点**：唯一的用户是横向 dock（它是覆盖屏幕中部
 * 的 LVGL 控件，展开时要把列表页的整片命中区顶掉），dock 已由自绘的全屏
 * 导航网格取代，模态层的优先级改走 pk_ui_modal_top()。函数与其单测保留：
 * 「浮层要整体让路」这条规则本身没变，下一个 LVGL 浮层还会需要它。 */
void pk_touch_arbiter_force_lvgl(pk_touch_arbiter_t *a);
