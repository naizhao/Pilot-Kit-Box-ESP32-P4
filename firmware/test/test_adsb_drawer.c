/*
 * test_adsb_drawer.c — host proof for 共享飞机详情抽屉（adsb_drawer）的可移植面。
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/test/host_stubs/ui_toast \
 *      -I firmware/test/host_stubs -I firmware/main -DPK_ADSB_DRAWER_HOST_TEST \
 *      -o /tmp/test_adsb_drawer firmware/test/test_adsb_drawer.c \
 *      firmware/main/adsb_drawer.c firmware/main/ui_state.c \
 *      firmware/main/pk_cal_advisor.c && /tmp/test_adsb_drawer
 *
 * 同 test_ui_toast.c 的翻译单元惯例：链接**真** ui_state.c（own-ICAO 绑定、
 * toast、set_mode 都是真实实现），host 上跑不动的外设依赖由 host_stubs 注入。
 * 抽屉的像素渲染走 PK_ADSB_DRAWER_HOST_TEST 切掉（同 test_apt_detail_page.c），
 * 这里锁的是三件"两页共用、错了看不出但行为是假的"的事：
 *
 *   1) 绑定按钮命中区——ADS-B 列表与地图页共用同一组矩形，一边改了另一边
 *      必须跟着变；命中区是半开区间 [x0,x1)×[y0,y1)，与 pk_pfd_fill_rect 同约。
 *   2) 抽屉目标 toggle 语义——点同一架收起、点别架换讲。列表行点击（2026-08
 *      改成 toggle）与地图点飞机必须同一套判定，否则"列表里点一下收起、地图
 *      里点一下只是重开"这种分叉没有任何一层测试能抓到。
 *   3) 绑定动作——绑定→落 own + toast + 跳 PFD；解绑→清 own + toast、不跳页。
 *      这是从列表抽屉原样平移的动作，地图页复用时一个字都不许另写一份。
 */
#include <stdio.h>
#include <string.h>

#include "adsb_drawer.h"
#include "ui_state.h"

static int64_t now_us;
static int failures;
int64_t esp_timer_get_time(void) { return now_us; }
void pk_airband_recordings_page_open(void) {}
void pk_airband_recordings_page_leave(void) {}
void pk_config_own_icao_set(uint32_t icao) { (void)icao; }
void pk_config_own_icao_clear(void) {}

#define CHECK(value) do { if (!(value)) { \
    printf("FAIL line %d: %s\n", __LINE__, #value); failures++; \
} } while (0)

/* ── 1) 版面几何：命中区与宏 ─────────────────────────────────── */

static void test_geometry(void)
{
    /* 抽屉贴底 230 px（原列表 spec §5.3 的同一张版面账）。 */
    CHECK(PK_ADSB_DRAWER_TOP == 250);

    /* 绑定钮命中区：按钮 x[280,520) y[448,478)，命中各向外放宽——
     * x ±16 → [264,536)，y 上放宽 12、下到屏底 → [436,480)。半开区间：
     * 边界值 264/536/436 属于命中，263/535 之外的邻格不属于。 */
    CHECK(!pk_adsb_drawer_bind_hit(263, 460));
    CHECK( pk_adsb_drawer_bind_hit(264, 460));
    CHECK( pk_adsb_drawer_bind_hit(400, 460));
    CHECK(!pk_adsb_drawer_bind_hit(536, 460));
    CHECK(!pk_adsb_drawer_bind_hit(400, 435));
    CHECK( pk_adsb_drawer_bind_hit(400, 436));
    CHECK( pk_adsb_drawer_bind_hit(400, 479));
    /* 抽屉区以外（键值矩阵那一带）不是绑定钮。 */
    CHECK(!pk_adsb_drawer_bind_hit(400, 320));
    CHECK(!pk_adsb_drawer_bind_hit(100, 460));
    CHECK(!pk_adsb_drawer_bind_hit(700, 460));
}

/* ── 2) 紧急码徽章 ──────────────────────────────────────────── */

static void test_emergency_tag(void)
{
    aircraft_t a = {0};

    a.have_squawk = false; a.squawk = 7700;
    CHECK(pk_adsb_emergency_tag(&a) == NULL);   /* 没广播过就不算 */

    a.have_squawk = true;
    a.squawk = 7500; CHECK(strcmp(pk_adsb_emergency_tag(&a), "HJK") == 0);
    a.squawk = 7600; CHECK(strcmp(pk_adsb_emergency_tag(&a), "RDO") == 0);
    a.squawk = 7700; CHECK(strcmp(pk_adsb_emergency_tag(&a), "EMG") == 0);
    a.squawk = 1200; CHECK(pk_adsb_emergency_tag(&a) == NULL);
    a.squawk = 2000; CHECK(pk_adsb_emergency_tag(&a) == NULL);
    a.squawk =    0; CHECK(pk_adsb_emergency_tag(&a) == NULL);
}

/* ── 3) 抽屉目标 toggle：同架收起 / 别架换讲 / 无目标 ─────────── */

static void test_tap_target(void)
{
    const uint32_t A = 0xABC101, B = 0xDEF202;

    CHECK(pk_adsb_drawer_tap_target(0, A) == A);    /* 关着 → 开 */
    CHECK(pk_adsb_drawer_tap_target(A, A) == 0);    /* 讲的就是它 → 收起 */
    CHECK(pk_adsb_drawer_tap_target(B, A) == A);    /* 讲的是别架 → 换讲 */
    CHECK(pk_adsb_drawer_tap_target(A, 0) == 0);    /* 点空（无目标）→ 收起 */
}

/* ── 4) 绑定动作（真 ui_state） ─────────────────────────────── */

static pk_tr_id_t last_toast_id(void)
{
    pk_tr_id_t id = PK_TR_LIST_NO_CONTACTS;
    bool err = true;
    if (!pk_ui_toast_get(&id, &err)) return (pk_tr_id_t)-1;
    return id;
}

static void test_bind_activate(void)
{
    const uint32_t A = 0x123456, B = 0x654321;

    /* 前置：开机默认 own=0（host sdkconfig 桩）、PFD 页。 */
    CHECK(pk_ui_get_own_icao() == 0);
    pk_ui_set_mode(PK_UI_MODE_MAP);
    CHECK(pk_ui_get_mode() == PK_UI_MODE_MAP);

    /* 绑定 A：落 own、绿字 toast、跳 PFD 看效果（列表抽屉的同一动作）。 */
    now_us = 1000000;
    pk_adsb_drawer_bind_activate(A);
    CHECK(pk_ui_get_own_icao() == A);
    CHECK(pk_ui_get_mode() == PK_UI_MODE_PFD);
    CHECK(last_toast_id() == PK_TR_TOAST_OWN_BOUND);

    /* 绑定 B：换绑到 B（不是先解再绑的中间态）。 */
    now_us = 2000000;
    pk_adsb_drawer_bind_activate(B);
    CHECK(pk_ui_get_own_icao() == B);
    CHECK(last_toast_id() == PK_TR_TOAST_OWN_BOUND);

    /* 解绑 B：own 显式清成 0（而不是退回 Kconfig 默认）、不跳页。 */
    now_us = 3000000;
    pk_ui_set_mode(PK_UI_MODE_ADSB_LIST);
    pk_adsb_drawer_bind_activate(B);
    CHECK(pk_ui_get_own_icao() == 0);
    CHECK(pk_ui_get_mode() == PK_UI_MODE_ADSB_LIST);   /* 解绑留在原页 */
    CHECK(last_toast_id() == PK_TR_TOAST_OWN_CLEARED);

    /* 绑定动作只看抽屉目标自己：目标不是当前 own → 换绑成目标（列表抽屉
     * 原语义：按钮文案讲的是"这架"要不要当本机，不是"当前本机"是谁）。
     * 换绑同样跳 PFD。 */
    now_us = 4000000;
    pk_ui_set_mode(PK_UI_MODE_MAP);
    pk_adsb_drawer_bind_activate(B);
    CHECK(pk_ui_get_own_icao() == B);
    CHECK(pk_ui_get_mode() == PK_UI_MODE_PFD);
    now_us = 5000000;
    pk_adsb_drawer_bind_activate(A);
    CHECK(pk_ui_get_own_icao() == A);
    CHECK(last_toast_id() == PK_TR_TOAST_OWN_BOUND);
}

int main(void)
{
    CHECK(pk_ui_init() == ESP_OK);
    test_geometry();
    test_emergency_tag();
    test_tap_target();
    test_bind_activate();
    if (failures) return 1;
    puts("PASS adsb drawer shared surface");
    return 0;
}
