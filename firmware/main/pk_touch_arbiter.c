/*
 * pk_touch_arbiter.c — 见 pk_touch_arbiter.h。
 *
 * 实现刻意保持得能一眼看完：这一层的价值全在「什么时候允许改归属」这一条
 * 规则上，逻辑一复杂就又会长出当初那种「每帧重判」的缝。
 */
#include "pk_touch_arbiter.h"

pk_touch_action_t pk_touch_arbiter_press(pk_touch_arbiter_t *a)
{
    switch (a->owner) {
    case PK_TOUCH_OWNER_PAGE: return PK_TOUCH_ACTION_DRAG;
    case PK_TOUCH_OWNER_LVGL: return PK_TOUCH_ACTION_YIELD;
    case PK_TOUCH_OWNER_NONE:
    default:                  return PK_TOUCH_ACTION_HITTEST;
    }
}

void pk_touch_arbiter_settle(pk_touch_arbiter_t *a, bool eaten)
{
    /* 只在还没定归属时才写。HITTEST 之外的帧调到这里说明调用方用错了顺序，
     * 与其悄悄改掉归属（那就是这个文件要根治的 bug），不如什么都不做。 */
    if (a->owner != PK_TOUCH_OWNER_NONE) return;
    a->owner = eaten ? PK_TOUCH_OWNER_PAGE : PK_TOUCH_OWNER_LVGL;
}

void pk_touch_arbiter_release(pk_touch_arbiter_t *a)
{
    a->owner = PK_TOUCH_OWNER_NONE;
}

void pk_touch_arbiter_force_lvgl(pk_touch_arbiter_t *a)
{
    a->owner = PK_TOUCH_OWNER_LVGL;
}
