/*
 * test_rp_core0_scheduler.c — RP2040 core0 公平调度 host 单测。
 *
 * 捕获的回归：把 Mode-S 帧环排到空才轮询其他链路；若生产者在每次
 * tail 前进后立即补帧，p4_link_poll_rx、spim_poll、CDC/控制路径和
 * 1 Hz/core1 看护永远得不到执行机会。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/rp2040 -o /tmp/test_rp_core0_scheduler \
 *      firmware/test/test_rp_core0_scheduler.c \
 *      firmware/rp2040/rp_core0_scheduler.c \
 *   && /tmp/test_rp_core0_scheduler
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "rp_core0_scheduler.h"

static void read_file(const char *path, char *buf, size_t buf_len)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        buf[0] = '\0';
        return;
    }
    size_t n = fread(buf, 1, buf_len - 1, f);
    fclose(f);
    buf[n] = '\0';
}

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("        at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

typedef struct {
    unsigned pending;
    unsigned sent;
    unsigned producer_limit;
    unsigned p4_polls;
    unsigned spi_polls;
    unsigned control_polls;
    unsigned periodic_polls;
    unsigned first_service_after_sends;
} fixture_t;

/* 模拟真实 SPSC 竞争：消费方推进 tail 后，生产方立即在新 head 补一帧。 */
static bool send_one_and_refill(void *user)
{
    fixture_t *f = user;
    if (f->pending == 0) return false;
    f->pending--;
    f->sent++;
    if (f->sent < f->producer_limit)
        f->pending++;
    return true;
}

static void note_service(fixture_t *f)
{
    if (f->first_service_after_sends == 0)
        f->first_service_after_sends = f->sent;
}

static void poll_p4(void *user)
{
    fixture_t *f = user;
    note_service(f);
    f->p4_polls++;
}

static void poll_spi(void *user)
{
    fixture_t *f = user;
    note_service(f);
    f->spi_polls++;
}

static void poll_control(void *user)
{
    fixture_t *f = user;
    note_service(f);
    f->control_polls++;
}

static void poll_periodic(void *user)
{
    fixture_t *f = user;
    note_service(f);
    f->periodic_polls++;
}

static rp_core0_ops_t ops_for(fixture_t *f)
{
    return (rp_core0_ops_t) {
        .send_one_modes = send_one_and_refill,
        .poll_p4_rx = poll_p4,
        .poll_spim = poll_spi,
        .poll_control = poll_control,
        .poll_periodic = poll_periodic,
        .user = f,
    };
}

static void test_sustained_producer_cannot_starve_other_services(void)
{
    fixture_t f = { .pending = 1, .producer_limit = 100 };
    rp_core0_ops_t ops = ops_for(&f);

    unsigned sent = rp_core0_schedule_once(&ops);

    CHECK(sent == RP_CORE0_MODES_BUDGET,
          "one pass sent=%u, budget=%u\n", sent, RP_CORE0_MODES_BUDGET);
    CHECK(f.first_service_after_sends == RP_CORE0_MODES_BUDGET,
          "first service after %u sends, budget=%u\n",
          f.first_service_after_sends, RP_CORE0_MODES_BUDGET);
    CHECK(f.p4_polls == 1, "p4_link_poll_rx opportunity=%u\n", f.p4_polls);
    CHECK(f.spi_polls == 1, "spim_poll opportunity=%u\n", f.spi_polls);
    CHECK(f.control_polls == 1, "control opportunity=%u\n", f.control_polls);
    CHECK(f.periodic_polls == 1, "1Hz/watchdog opportunity=%u\n",
          f.periodic_polls);
    CHECK(f.pending == 1, "producer remains active, pending=%u\n", f.pending);
}

static void test_idle_ring_still_services_every_path(void)
{
    fixture_t f = { .producer_limit = 100 };
    rp_core0_ops_t ops = ops_for(&f);

    unsigned sent = rp_core0_schedule_once(&ops);

    CHECK(sent == 0, "idle pass sent=%u\n", sent);
    CHECK(f.p4_polls == 1, "idle p4 opportunity=%u\n", f.p4_polls);
    CHECK(f.spi_polls == 1, "idle spi opportunity=%u\n", f.spi_polls);
    CHECK(f.control_polls == 1, "idle control opportunity=%u\n",
          f.control_polls);
    CHECK(f.periodic_polls == 1, "idle periodic opportunity=%u\n",
          f.periodic_polls);
}

static void test_production_wires_fair_scheduler(void)
{
    static char app[32768];
    static char cmake[32768];
    read_file("firmware/rp2040/adsb1090.c", app, sizeof(app));
    read_file("firmware/rp2040/CMakeLists.txt", cmake, sizeof(cmake));
    CHECK(strstr(app, "rp_core0_schedule_once(&ops)") != NULL,
          "adsb1090.c must call rp_core0_schedule_once in the core0 loop\n");
    CHECK(strstr(cmake, "rp_core0_scheduler.c") != NULL,
          "CMakeLists.txt must compile rp_core0_scheduler.c into adsb1090\n");
}

int main(void)
{
    test_sustained_producer_cannot_starve_other_services();
    test_idle_ring_still_services_every_path();
    test_production_wires_fair_scheduler();
    printf(g_fail ? "FAIL (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
