/*
 * test_baro_bringup.c — BMP388 开机瞬态失败必须能自己恢复（2026-09-09 审计）。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/test/host_stubs -I firmware/main \
 *      -o /tmp/test_baro_bringup firmware/test/test_baro_bringup.c \
 *      firmware/main/baro_task.c firmware/main/baro_compensate.c \
 *      firmware/main/pk_bringup_retry.c -lm \
 *   && /tmp/test_baro_bringup
 *
 * 缺陷（修复前）
 * --------------
 * baro_task() 开头只探两轮 CHIP_ID（2 轮 × 10 次 × 100 ms ≈ 2 s），仍失败就
 * vTaskDelete(NULL) —— **任务本体消失**。于是后面所有自愈机制（I²C0 总线
 * generation 重放、EVENT.por_detected 重配、配置失败每秒重试）都没有消费者，
 * 整次开机再也不会有气压高度，只能重启整机。而开机那一瞬恰恰最不可靠：
 * 2026-08-03 真机日志里的总线塌陷就发生在开机阶段（GT911 只 found 没 ready）,
 * 谁排在它后面谁中招。
 *
 * 判据（本文件跑的是**生产任务函数本身**，不是它的复制品，也不搜源码文本）
 * ------------------------------------------------------------------------
 * host 桩把 xTaskCreatePinnedToCore 记下的函数指针交给测试就地执行，
 * vTaskDelay 变成测试自己的调度器：在每个让渡点推进模拟时钟、采样
 * pk_baro_get()（PFD / 飞行记录真正看到的那个出口）。器件模型是"上电后头
 * 12 秒总线上的事务全超时，之后正常应答"——**按模拟时间**决定，不按第几次
 * 尝试，所以它对实现细节中立：旧实现在 2 s 处就已经把任务删了。
 *
 *   1. 瞬态失败后恢复：12 s 后器件回话 → 必须自己进工作态（valid=true）；
 *   2. 失败期间不撒谎：进工作态之前每一次采样都必须 valid=false；
 *   3. 任务不退出：vTaskDelete 一次都不许调；
 *   4. 退避有界且非忙等：每次等待 ∈ [1 s, 60 s]、单调不减、涨到 60 s 封顶
 *      就停在那里（不会翻到 120 s，qmc5883p 那个越界 bug 的同类）；
 *   5. 不造成 reset 风暴：必装器件长期缺席时，总线级恢复请求两次之间的
 *      模拟间隔 ≥ 8 s（BARO_UP_RECOVER_MIN_BACKOFF_MS），而不是每轮都请求；
 *   6. 资源生命周期：i2c device 只挂一次（重试不重复 add_device）；建任务
 *      失败时 device 摘掉、mutex 还回去。
 */

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "baro.h"
#include "config_qnh.h"
#include "gps.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pk_i2c0_bus.h"
#include "pk_i2c0_recover.h"

/* ── 断言 ─────────────────────────────────────────────────────────────── */

static int g_fail;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  [FAIL] " __VA_ARGS__);                                 \
            printf("         at %s:%d\n", __FILE__, __LINE__);               \
            g_fail++;                                                        \
        }                                                                    \
    } while (0)

/* ── 模拟时钟 ─────────────────────────────────────────────────────────── */

static int64_t g_now_us;

int64_t esp_timer_get_time(void) { return g_now_us; }

/* ── 假 BMP388 ────────────────────────────────────────────────────────── *
 *
 * 标定字节与 raw 读数取自 test_baro_compensate.c 的冻结黄金向量 "sea"
 * （CALIB_A / up=0x504DD4 / ut=0x9300A3 → 101325 Pa、15 °C）：那边已经用
 * 独立 double 参考实现互证过，这里只借它构造一组"补偿之后物理上说得通"的
 * 读数，让 valid=true 这件事有真实数据支撑，而不是随手编 21 个字节碰运气
 * （负压会被 baro_task 的 press_pa>0 守卫挡掉，valid 永远上不来）。 */

static const uint8_t FAKE_CALIB[21] = {
    0x9E, 0x8E,  0x03, 0xDB,  0xE9,  0xB7, 0x7F,  0x64, 0xFC,
    0x23,  0x05,  0xF4, 0x01,  0xB4, 0xFF,  0x25,  0x9C,
    0xC6, 0x59,  0xA6,  0x7F
};
#define FAKE_RAW_PRESS 0x504DD4u
#define FAKE_RAW_TEMP  0x9300A3u

/* 器件在这个模拟时刻之前对任何事务都超时（总线被别人拖死 / 器件还没上电）。 */
static int64_t g_alive_at_us;
static int     g_add_device_calls;
static int     g_rm_device_calls;

static bool device_alive(void) { return g_now_us >= g_alive_at_us; }

/* ── 被测模块的协作者（全部由测试提供实现）───────────────────────────── */

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                    const i2c_device_config_t *cfg,
                                    i2c_master_dev_handle_t *out_handle)
{
    (void)bus;
    g_add_device_calls++;
    if (out_handle) *out_handle = (i2c_master_dev_handle_t)(intptr_t)cfg->device_address;
    return ESP_OK;
}

esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev)
{
    (void)dev;
    g_rm_device_calls++;
    return ESP_OK;
}

i2c_master_bus_handle_t pk_i2c0_bus_get(void) { return (i2c_master_bus_handle_t)1; }
uint32_t pk_i2c0_bus_generation(void) { return 0; }

esp_err_t pk_i2c0_bus_transmit(i2c_master_dev_handle_t dev,
                               const uint8_t *wr, size_t wr_len, int timeout_ms)
{
    (void)dev; (void)wr; (void)wr_len; (void)timeout_ms;
    return device_alive() ? ESP_OK : ESP_ERR_TIMEOUT;   /* 寄存器写 */
}

esp_err_t pk_i2c0_bus_receive(i2c_master_dev_handle_t dev,
                              uint8_t *rd, size_t rd_len, int timeout_ms)
{
    (void)dev; (void)rd; (void)rd_len; (void)timeout_ms;
    return ESP_ERR_TIMEOUT;   /* BMP388 驱动不用裸读 */
}

esp_err_t pk_i2c0_bus_transmit_receive(i2c_master_dev_handle_t dev,
                                       const uint8_t *wr, size_t wr_len,
                                       uint8_t *rd, size_t rd_len, int timeout_ms)
{
    (void)dev; (void)timeout_ms;
    if (!device_alive()) return ESP_ERR_TIMEOUT;
    if (wr_len != 1 || rd == NULL) return ESP_ERR_INVALID_ARG;

    switch (wr[0]) {
    case 0x00:                                  /* CHIP_ID */
        if (rd_len != 1) return ESP_ERR_INVALID_SIZE;
        rd[0] = 0x50;
        return ESP_OK;
    case 0x10:                                  /* EVENT：无 POR */
        if (rd_len != 1) return ESP_ERR_INVALID_SIZE;
        rd[0] = 0x00;
        return ESP_OK;
    case 0x31:                                  /* 21 字节标定 */
        if (rd_len != sizeof(FAKE_CALIB)) return ESP_ERR_INVALID_SIZE;
        memcpy(rd, FAKE_CALIB, sizeof(FAKE_CALIB));
        return ESP_OK;
    case 0x04:                                  /* PRESS/TEMP 各 3 字节 */
        if (rd_len != 6) return ESP_ERR_INVALID_SIZE;
        rd[0] = (uint8_t)(FAKE_RAW_PRESS & 0xFF);
        rd[1] = (uint8_t)((FAKE_RAW_PRESS >> 8) & 0xFF);
        rd[2] = (uint8_t)((FAKE_RAW_PRESS >> 16) & 0xFF);
        rd[3] = (uint8_t)(FAKE_RAW_TEMP & 0xFF);
        rd[4] = (uint8_t)((FAKE_RAW_TEMP >> 8) & 0xFF);
        rd[5] = (uint8_t)((FAKE_RAW_TEMP >> 16) & 0xFF);
        return ESP_OK;
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

/* 总线级恢复请求：只记录"什么时候请求过"，不真的救活器件——器件复活由
 * g_alive_at_us（模拟时间）决定，否则这个测试就变成在测自己的桩。 */
#define MAX_EVENTS 512
static int     g_recover_calls;
static int64_t g_recover_us[MAX_EVENTS];

esp_err_t pk_i2c0_recover_request(const char *who)
{
    (void)who;
    if (g_recover_calls < MAX_EVENTS) g_recover_us[g_recover_calls] = g_now_us;
    g_recover_calls++;
    return ESP_ERR_INVALID_STATE;   /* 真实实现常被冷却挡下 */
}

/* 运行期失败探测器：本测试针对**启动期**，这里只做计数，不触发恢复。
 * （它自己的判据由 test_pk_i2c0_policy.c 覆盖。） */
static int g_client_report_fail;
void pk_i2c0_client_init(pk_i2c0_client_t *c, const char *name,
                         uint32_t min_fail_count, int64_t min_fail_span_us)
{
    (void)name; (void)min_fail_count; (void)min_fail_span_us;
    memset(c, 0, sizeof(*c));
}
bool pk_i2c0_client_report(pk_i2c0_client_t *c, bool ok)
{
    (void)c;
    if (!ok) g_client_report_fail++;
    return false;
}
void pk_i2c0_client_reset(pk_i2c0_client_t *c) { (void)c; }

float pk_qnh_get(void) { return 1013.25f; }
/* auto-QNH 的协作者（2026-09-11 新增）：本测试只关心 bring-up，让模式返回
 * MANUAL 即可跳过 auto 路径；其余符号仍要被链接，故一并补桩。 */
pk_qnh_mode_t pk_qnh_mode_get(void) { return PK_QNH_MODE_MANUAL; }
void  pk_qnh_set_auto(float hpa) { (void)hpa; }
bool  pk_gps_get(pk_gps_state_t *out) { (void)out; return false; }
float pk_qnh_from_pressure_alt(float p, float a) { (void)p; (void)a; return 1013.25f; }
bool  pk_demo_enabled(void) { return false; }
bool  pk_demo_baro(int64_t now_us, pk_baro_state_t *out)
{ (void)now_us; (void)out; return false; }

/* ── 调度器：vTaskDelay 就是让渡点 ────────────────────────────────────── */

static jmp_buf g_escape;
static int     g_delay_count;
static unsigned g_delay_ms[MAX_EVENTS];
static int     g_yield_budget;
static int     g_valid_true;          /* 采到 valid=true 的次数 */
static int     g_valid_false;         /* 采到 valid=false 的次数 */
static int64_t g_first_valid_us = -1;
static int     g_valid_before_alive;  /* 器件还没活就报 valid 的次数（撒谎） */
static int     g_delete_calls;

static void on_delay(unsigned ms)
{
    if (g_delay_count < MAX_EVENTS) g_delay_ms[g_delay_count] = ms;
    g_delay_count++;
    g_now_us += (int64_t)ms * 1000;

    pk_baro_state_t st;
    bool v = pk_baro_get(&st);
    if (v) {
        g_valid_true++;
        if (g_first_valid_us < 0) g_first_valid_us = g_now_us;
        if (!device_alive()) g_valid_before_alive++;
    } else {
        g_valid_false++;
    }

    /* 收工条件：已经稳定进入工作态，或者跑满预算（缺席场景） */
    if (g_valid_true >= 5 || g_delay_count >= g_yield_budget) longjmp(g_escape, 1);
}

static void on_delete(void *handle)
{
    (void)handle;
    g_delete_calls++;
    longjmp(g_escape, 2);   /* 任务真的退出了，没法再往下跑 */
}

/* 跑一遍生产的 pk_baro_start() + 它注册的那个任务函数。 */
static void run_baro(int yield_budget)
{
    g_yield_budget = yield_budget;
    pk_host_task_delay_hook  = on_delay;
    pk_host_task_delete_hook = on_delete;

    if (setjmp(g_escape) == 0) {
        pk_baro_start();
        CHECK(g_add_device_calls == 1, "add_device 调了 %d 次，应为 1\n", g_add_device_calls);
        CHECK(pk_host_task_create_count == 1,
              "任务创建 %d 次，应为 1（长期任务必须先存在）\n", pk_host_task_create_count);
        if (pk_host_task_last_fn) pk_host_task_last_fn(pk_host_task_last_arg);
        CHECK(false, "任务函数返回了——长期任务不该 return\n");
    }
}

/* ── 场景 1：开机头 12 秒总线不通，之后器件正常 ───────────────────────── */

static void scenario_transient_then_recovers(void)
{
    g_alive_at_us = 12 * 1000000LL;
    run_baro(400);

    CHECK(g_delete_calls == 0,
          "vTaskDelete 调了 %d 次——瞬态失败不得删掉长期任务\n", g_delete_calls);
    CHECK(g_valid_true >= 5,
          "器件恢复后没能进入工作态（valid=true 只有 %d 次）\n", g_valid_true);
    CHECK(g_valid_false > 0, "从来没采到过失败期，场景没跑起来\n");
    CHECK(g_valid_before_alive == 0,
          "器件还没应答就报了 %d 次 valid=true\n", g_valid_before_alive);
    CHECK(g_first_valid_us < 0 || g_first_valid_us >= g_alive_at_us,
          "第一次 valid=true 发生在 %.1f s，早于器件复活时刻 %.1f s\n",
          (double)g_first_valid_us / 1e6, (double)g_alive_at_us / 1e6);
    CHECK(g_add_device_calls == 1,
          "重试期间重复挂了器件：add_device %d 次\n", g_add_device_calls);
    CHECK(g_recover_calls <= 1,
          "一次 12 s 的瞬态失败请求了 %d 次总线恢复（风暴）\n", g_recover_calls);
}

/* ── 场景 2：器件长期缺席（坏了/没焊）───────────────────────────────── */

static void scenario_absent_forever_never_dies(void)
{
    g_alive_at_us = INT64_MAX;
    run_baro(60);

    CHECK(g_delete_calls == 0,
          "vTaskDelete 调了 %d 次——器件缺席也不许删任务\n", g_delete_calls);
    CHECK(g_valid_true == 0, "器件缺席却报了 %d 次 valid=true\n", g_valid_true);
    CHECK(g_delay_count >= 60, "只让渡了 %d 次，预算没跑满\n", g_delay_count);

    /* 退避：有界、非忙等、单调不减、封顶后停住 */
    unsigned prev = 0, maxd = 0;
    int monotonic = 1, in_range = 1;
    int n = g_delay_count < MAX_EVENTS ? g_delay_count : MAX_EVENTS;
    for (int i = 0; i < n; i++) {
        unsigned d = g_delay_ms[i];
        if (d < 1000 || d > 60000) in_range = 0;
        if (d < prev) monotonic = 0;
        if (d > maxd) maxd = d;
        prev = d;
    }
    CHECK(in_range, "有等待落在 [1s, 60s] 之外（忙等或越过封顶）\n");
    CHECK(monotonic, "退避不是单调不减\n");
    CHECK(maxd == 60000, "退避封顶是 %u ms，应为 60000\n", maxd);

    /* 反 reset 风暴：两次总线恢复请求之间至少隔 8 s 模拟时间 */
    CHECK(g_recover_calls > 0, "长期缺席一次总线恢复都没请求过，升级路径断了\n");
    int storm = 0;
    int m = g_recover_calls < MAX_EVENTS ? g_recover_calls : MAX_EVENTS;
    for (int i = 1; i < m; i++) {
        if (g_recover_us[i] - g_recover_us[i - 1] < 8 * 1000000LL) storm++;
    }
    CHECK(storm == 0, "有 %d 次总线恢复请求间隔不足 8 s\n", storm);
    CHECK(g_recover_calls < g_delay_count,
          "每一轮失败都请求了总线恢复（%d 次 / %d 轮）\n",
          g_recover_calls, g_delay_count);
}

/* ── 场景 3：建任务失败必须把已经拿到的资源还回去 ─────────────────────── */

static void scenario_task_create_failure_cleans_up(void)
{
    pk_host_task_create_fail = 1;
    pk_host_task_delay_hook  = on_delay;
    pk_host_task_delete_hook = on_delete;
    g_yield_budget = 10;

    pk_baro_start();

    CHECK(pk_host_task_create_count == 0, "任务不该创建成功\n");
    CHECK(g_add_device_calls == 1, "add_device 应调 1 次，实际 %d\n", g_add_device_calls);
    CHECK(g_rm_device_calls == 1,
          "建任务失败后没摘器件（rm_device %d 次）\n", g_rm_device_calls);
    CHECK(pk_host_mutex_live == 0,
          "建任务失败后漏了 %d 把 mutex\n", pk_host_mutex_live);
}

/* ── 入口：每个场景 fork 一个干净进程（生产模块是文件级静态状态）──────── */

static int run_forked(const char *name, void (*fn)(void))
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        printf("%s\n", name);
        fn();
        fflush(stdout);
        _exit(g_fail ? 1 : 0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("  -> %s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

int main(void)
{
    int failed = 0;
    failed += run_forked("scenario 1: 12 s 瞬态失败后必须自己进入工作态",
                         scenario_transient_then_recovers);
    failed += run_forked("scenario 2: 器件长期缺席——任务存活、退避有界、不打 reset 风暴",
                         scenario_absent_forever_never_dies);
    failed += run_forked("scenario 3: 建任务失败必须摘器件、还 mutex",
                         scenario_task_create_failure_cleans_up);

    if (failed) {
        printf("test_baro_bringup: %d scenario(s) FAILED\n", failed);
        return 1;
    }
    printf("test_baro_bringup: all scenarios passed\n");
    return 0;
}
