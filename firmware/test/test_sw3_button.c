/*
 * test_sw3_button.c — SW3 开机态按键纯状态机。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -DPK_SW3_HOST_TEST \
 *      -I firmware/main -o /tmp/test_sw3_button \
 *      firmware/test/test_sw3_button.c firmware/main/sw3_button.c \
 *   && /tmp/test_sw3_button
 *
 * 物理 SW3 同时直连 SY6970 /QON。软件长按必须在“松手”后才写
 * BATFET_DIS，否则芯片进入 shipping mode 时 /QON 仍为低，可能立即满足
 * 退出条件。达到长按阈值只发提示事件，松手再发关机事件。
 */
#include "sw3_button.h"

#include <stdio.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    g_fail++; \
} } while (0)

static void arm_released(pk_sw3_button_t *s)
{
    pk_sw3_button_reset(s, 0);
    CHECK(pk_sw3_button_update(s, false, 0) == PK_SW3_EVENT_NONE);
    CHECK(pk_sw3_button_update(s, false, PK_SW3_STARTUP_GUARD_MS)
          == PK_SW3_EVENT_NONE);
    CHECK(s->armed == true);
}

static void test_wakeup_hold_is_consumed_before_arming(void)
{
    pk_sw3_button_t s;
    pk_sw3_button_reset(&s, 0);

    CHECK(pk_sw3_button_update(&s, true, 0) == PK_SW3_EVENT_NONE);
    CHECK(pk_sw3_button_update(&s, true, PK_SW3_DEBOUNCE_MS)
          == PK_SW3_EVENT_NONE);
    CHECK(pk_sw3_button_update(&s, true, PK_SW3_STARTUP_GUARD_MS + 1000)
          == PK_SW3_EVENT_NONE);
    CHECK(s.armed == false);

    /* 用来退出 shipping mode 的同一次长按，启动后松手只能解除保护，
     * 绝不能被解释成一次软件长按关机。 */
    CHECK(pk_sw3_button_update(&s, false, 3100) == PK_SW3_EVENT_NONE);
    CHECK(pk_sw3_button_update(&s, false, 3100 + PK_SW3_DEBOUNCE_MS)
          == PK_SW3_EVENT_NONE);
    CHECK(s.armed == true);

    /* 之后的新按压才允许产生事件。 */
    CHECK(pk_sw3_button_update(&s, true, 4000) == PK_SW3_EVENT_NONE);
    CHECK(pk_sw3_button_update(&s, true, 4000 + PK_SW3_DEBOUNCE_MS)
          == PK_SW3_EVENT_NONE);
    CHECK(pk_sw3_button_update(&s, false, 4100) == PK_SW3_EVENT_NONE);
    CHECK(pk_sw3_button_update(&s, false, 4100 + PK_SW3_DEBOUNCE_MS)
          == PK_SW3_EVENT_SHORT_PRESS);
}

static void test_bounce_produces_one_short_press_on_release(void)
{
    pk_sw3_button_t s;
    arm_released(&s);

    CHECK(pk_sw3_button_update(&s, true, 2100) == PK_SW3_EVENT_NONE);
    CHECK(pk_sw3_button_update(&s, false, 2110) == PK_SW3_EVENT_NONE);
    CHECK(pk_sw3_button_update(&s, true, 2120) == PK_SW3_EVENT_NONE);
    CHECK(pk_sw3_button_update(&s, true, 2149) == PK_SW3_EVENT_NONE);
    CHECK(pk_sw3_button_update(&s, true, 2150) == PK_SW3_EVENT_NONE);

    CHECK(pk_sw3_button_update(&s, false, 2200) == PK_SW3_EVENT_NONE);
    CHECK(pk_sw3_button_update(&s, true, 2210) == PK_SW3_EVENT_NONE);
    CHECK(pk_sw3_button_update(&s, false, 2220) == PK_SW3_EVENT_NONE);
    CHECK(pk_sw3_button_update(&s, false, 2249) == PK_SW3_EVENT_NONE);
    CHECK(pk_sw3_button_update(&s, false, 2250)
          == PK_SW3_EVENT_SHORT_PRESS);
    CHECK(pk_sw3_button_update(&s, false, 2300) == PK_SW3_EVENT_NONE);
}

static void test_long_press_prompts_once_then_shuts_down_on_release(void)
{
    pk_sw3_button_t s;
    arm_released(&s);

    CHECK(pk_sw3_button_update(&s, true, 2100) == PK_SW3_EVENT_NONE);
    CHECK(pk_sw3_button_update(&s, true, 2100 + PK_SW3_DEBOUNCE_MS)
          == PK_SW3_EVENT_NONE);
    const uint32_t threshold = 2100 + PK_SW3_LONG_PRESS_MS;
    CHECK(pk_sw3_button_update(&s, true, threshold - 1)
          == PK_SW3_EVENT_NONE);
    CHECK(pk_sw3_button_update(&s, true, threshold)
          == PK_SW3_EVENT_LONG_READY);
    CHECK(pk_sw3_button_update(&s, true, threshold + 500)
          == PK_SW3_EVENT_NONE);

    CHECK(pk_sw3_button_update(&s, false, threshold + 600)
          == PK_SW3_EVENT_NONE);
    CHECK(pk_sw3_button_update(&s, false,
                               threshold + 600 + PK_SW3_DEBOUNCE_MS)
          == PK_SW3_EVENT_LONG_RELEASE);
    CHECK(pk_sw3_button_update(&s, false, threshold + 1000)
          == PK_SW3_EVENT_NONE);
}

static void test_release_before_threshold_stays_short_after_debounce(void)
{
    pk_sw3_button_t s;
    arm_released(&s);

    CHECK(pk_sw3_button_update(&s, true, 2100) == PK_SW3_EVENT_NONE);
    CHECK(pk_sw3_button_update(&s, true, 2100 + PK_SW3_DEBOUNCE_MS)
          == PK_SW3_EVENT_NONE);
    const uint32_t release_ms = 2100 + PK_SW3_LONG_PRESS_MS - 10;
    CHECK(pk_sw3_button_update(&s, false, release_ms) == PK_SW3_EVENT_NONE);
    /* 消抖完成时已经越过长按时刻，但实际按住时间少 10ms，仍必须是短按。 */
    CHECK(pk_sw3_button_update(&s, false, release_ms + PK_SW3_DEBOUNCE_MS)
          == PK_SW3_EVENT_SHORT_PRESS);
}

static void test_sparse_poll_reports_prompt_and_release_without_short(void)
{
    pk_sw3_button_t s;
    arm_released(&s);

    CHECK(pk_sw3_button_update(&s, true, 2100) == PK_SW3_EVENT_NONE);
    CHECK(pk_sw3_button_update(&s, true, 2100 + PK_SW3_DEBOUNCE_MS)
          == PK_SW3_EVENT_NONE);

    /* 模拟任务两秒多没获调度：下一拍已经看见松手。状态机必须同时补发
     * 提示与长按松手，且绝不能退化成短按。 */
    CHECK(pk_sw3_button_update(&s, false, 4500) == PK_SW3_EVENT_NONE);
    const pk_sw3_event_t ev = pk_sw3_button_update(
        &s, false, 4500 + PK_SW3_DEBOUNCE_MS);
    CHECK((ev & PK_SW3_EVENT_LONG_READY) != 0);
    CHECK((ev & PK_SW3_EVENT_LONG_RELEASE) != 0);
    CHECK((ev & PK_SW3_EVENT_SHORT_PRESS) == 0);
}

static void test_sparse_poll_during_press_debounce_keeps_real_hold_time(void)
{
    pk_sw3_button_t s;
    arm_released(&s);

    CHECK(pk_sw3_button_update(&s, true, 2100) == PK_SW3_EVENT_NONE);
    /* 首次低电平之后任务停顿 2 秒；稳定确认虽迟到，按住时长必须从原始
     * 低沿 2100ms 起算，不能从 4100ms 重新计时。 */
    CHECK(pk_sw3_button_update(&s, true, 4100)
          == PK_SW3_EVENT_LONG_READY);
    CHECK(pk_sw3_button_update(&s, false, 4200) == PK_SW3_EVENT_NONE);
    const pk_sw3_event_t ev = pk_sw3_button_update(
        &s, false, 4200 + PK_SW3_DEBOUNCE_MS);
    CHECK((ev & PK_SW3_EVENT_LONG_RELEASE) != 0);
    CHECK((ev & PK_SW3_EVENT_SHORT_PRESS) == 0);
}

static void test_events_map_to_screen_prompt_usb_and_shutdown_actions(void)
{
    pk_sw3_policy_t p = pk_sw3_policy(PK_SW3_EVENT_SHORT_PRESS,
                                      true, false);
    CHECK(p.set_screen == true);
    CHECK(p.screen_awake == false);
    CHECK(p.show_release_to_poweroff == false);
    CHECK(p.request_shutdown == false);

    p = pk_sw3_policy(PK_SW3_EVENT_SHORT_PRESS, false, false);
    CHECK(p.set_screen == true);
    CHECK(p.screen_awake == true);

    p = pk_sw3_policy(PK_SW3_EVENT_LONG_READY, false, false);
    CHECK(p.set_screen == true);
    CHECK(p.screen_awake == true);
    CHECK(p.show_release_to_poweroff == true);
    CHECK(p.request_shutdown == false);

    p = pk_sw3_policy(PK_SW3_EVENT_LONG_RELEASE, true, true);
    CHECK(p.show_need_unplug == true);
    CHECK(p.request_shutdown == false);

    p = pk_sw3_policy(PK_SW3_EVENT_LONG_RELEASE, true, false);
    CHECK(p.show_need_unplug == false);
    CHECK(p.request_shutdown == true);
}

static void test_runtime_input_stays_ready_when_pmic_is_unavailable(void)
{
    CHECK(pk_sw3_runtime_ready(true, true, true) == true);
    CHECK(pk_sw3_runtime_ready(false, true, true) == false);
    CHECK(pk_sw3_runtime_ready(true, false, true) == false);
    /* PMIC 诊断暂不可用时仍要读取 GPIO4，才能让短按息屏/亮屏工作；
     * 真正的 BATFET 关机由 power_sy6970_shutdown() 独立 fail-closed。 */
    CHECK(pk_sw3_runtime_ready(true, true, false) == true);

    CHECK(pk_sw3_external_power_present(false, true) == true);
    /* 旧快照只能表示“未知”，不能永久阻止用户拔 USB 后关机。真实 USB
     * 若仍在，BATFET 动作会返回并显示失败，不会产生错误掉电。 */
    CHECK(pk_sw3_external_power_present(true, true) == false);
    CHECK(pk_sw3_external_power_present(false, false) == false);
}

int main(void)
{
    test_wakeup_hold_is_consumed_before_arming();
    test_bounce_produces_one_short_press_on_release();
    test_long_press_prompts_once_then_shuts_down_on_release();
    test_release_before_threshold_stays_short_after_debounce();
    test_sparse_poll_reports_prompt_and_release_without_short();
    test_sparse_poll_during_press_debounce_keeps_real_hold_time();
    test_events_map_to_screen_prompt_usb_and_shutdown_actions();
    test_runtime_input_stays_ready_when_pmic_is_unavailable();

    if (g_fail) {
        printf("test_sw3_button: %d FAIL\n", g_fail);
        return 1;
    }
    printf("test_sw3_button: all OK\n");
    return 0;
}
