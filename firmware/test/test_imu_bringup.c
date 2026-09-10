/*
 * test_imu_bringup.c — BNO085 开机瞬态失败必须能自己恢复（2026-09-09 审计）。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/test/host_stubs -I firmware/main \
 *      -o /tmp/test_imu_bringup firmware/test/test_imu_bringup.c \
 *      firmware/main/imu_task.c firmware/main/pk_vib.c \
 *      firmware/main/pk_bringup_retry.c -lm \
 *   && /tmp/test_imu_bringup
 *
 * 缺陷（修复前）
 * --------------
 * pk_imu_init() 在**创建长期任务之前**同步跑 bno_bring_up()，失败就 return：
 * imu 任务从来没被创建。于是那条写了大段注释的 5 s stall watchdog、I²C0 总线
 * generation 重放、RST 自愈——全都没有执行者，整次开机再也不会有姿态，只能
 * 重启整机。而开机那一瞬恰恰最不可靠（2026-08-03 真机日志里的总线塌陷就在
 * 开机阶段）。BNO085 是必装器件，"探不到就算了"不是可接受的降级。
 *
 * 判据（跑的是**生产代码注册的那个任务函数**，不是它的复制品）
 * -----------------------------------------------------------
 * host 桩把 xTaskCreatePinnedToCore 收到的函数指针交给测试就地执行，
 * vTaskDelay 变成测试的调度器：每个让渡点推进模拟时钟、采样
 * pk_imu_sample_get()（PFD 真正看到的出口）。器件模型按**模拟时间**决定
 * 死活（头 12 秒总线事务全超时），对实现细节中立。
 *
 *   1. pk_imu_init() 即使 bring-up 会失败也必须成功返回**并创建任务**；
 *   2. 器件回话后必须自己完成 SH-2 init（两条 Set Feature 真的发出去了）
 *      并进入工作态；
 *   3. 失败期间 pk_imu_sample_get() 恒 false（不许拿单位四元数冒充姿态）；
 *   4. 任务不退出、不忙等；退避有界（≤60 s）、单调不减、封顶后停住；
 *   5. 不打 reset 风暴：总线级恢复请求两次之间 ≥ 8 s 模拟时间；
 *   6. 资源生命周期：device 只 add 一次；建任务失败要摘 device、还 mutex。
 */

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "imu_task.h"
#include "pk_board.h"
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

#define MAX_EVENTS 4096

/* ── 模拟时钟 ─────────────────────────────────────────────────────────── *
 *
 * 时间由两处推进：vTaskDelay（让渡）与**每一笔 I²C 事务**（400 kHz 上几个
 * 字节 ≈ 0.25 ms）。后者不是装饰：bno_bring_up() 的排空循环是
 * `while (esp_timer_get_time() < deadline)`，读成功时不 delay——只靠让渡推
 * 时钟的话，器件一活过来这个循环就永远出不来（真机上时间照走）。 */

#define I2C_TXN_US 250

static int64_t g_now_us;

int64_t esp_timer_get_time(void) { return g_now_us; }

/* ── 假 BNO085（SHTP over I²C）────────────────────────────────────────── */

#define SHTP_CH_SENSORHUB 3

static int64_t g_alive_at_us;
static int     g_add_device_calls;
static int     g_rm_device_calls;
static int     g_setfeat_rv;      /* 收到（或尝试发出）的 RV Set Feature 条数 */
static int     g_setfeat_la;      /* 同上，Linear Acceleration */
static int     g_setfeat_rv_ok;   /* 真正发成功的（器件活着时） */
static int     g_setfeat_la_ok;

static bool device_alive(void) { return g_now_us >= g_alive_at_us; }

/* 一帧 Rotation Vector：单位四元数、accuracy=3。
 * 布局见 SH-2 §6.5.18 / parse_rotation_vector()：cargo[0]=0x05、[2]=status、
 * [4..11] = qi,qj,qk,qw（int16 小端，Q14）。 */
static size_t build_rv_frame(uint8_t *out)
{
    const uint8_t cargo[12] = {
        0x05, 0x00, 0x03, 0x00,
        0x00, 0x00,          /* qi = 0 */
        0x00, 0x00,          /* qj = 0 */
        0x00, 0x00,          /* qk = 0 */
        0x00, 0x40,          /* qw = 16384 = 1.0 (Q14) */
    };
    size_t total = 4 + sizeof(cargo);
    out[0] = (uint8_t)(total & 0xFF);
    out[1] = (uint8_t)(total >> 8);
    out[2] = SHTP_CH_SENSORHUB;
    out[3] = 0;
    memcpy(out + 4, cargo, sizeof(cargo));
    return total;
}

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
    (void)dev; (void)timeout_ms;
    g_now_us += I2C_TXN_US;

    /* SHTP 帧：[0..1]=len, [2]=channel, [3]=seq, [4..]=cargo。
     * Set Feature 的 cargo[0]=0xFD、cargo[1]=feature id。 */
    if (wr_len >= 6 && wr[4] == 0xFD) {
        if (wr[5] == 0x05) { g_setfeat_rv++; if (device_alive()) g_setfeat_rv_ok++; }
        if (wr[5] == 0x04) { g_setfeat_la++; if (device_alive()) g_setfeat_la_ok++; }
    }
    return device_alive() ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t pk_i2c0_bus_receive(i2c_master_dev_handle_t dev,
                              uint8_t *rd, size_t rd_len, int timeout_ms)
{
    (void)dev; (void)timeout_ms;
    g_now_us += I2C_TXN_US;
    if (!device_alive() || rd == NULL) return ESP_ERR_TIMEOUT;

    uint8_t frame[64];
    size_t total = build_rv_frame(frame);
    /* shtp_recv 先读 4 字节头，再按头里的长度整帧重读一次；两次都从帧首给。 */
    size_t n = rd_len < total ? rd_len : total;
    memset(rd, 0, rd_len);
    memcpy(rd, frame, n);
    return ESP_OK;
}

esp_err_t pk_i2c0_bus_transmit_receive(i2c_master_dev_handle_t dev,
                                       const uint8_t *wr, size_t wr_len,
                                       uint8_t *rd, size_t rd_len, int timeout_ms)
{
    (void)dev; (void)wr; (void)wr_len; (void)rd; (void)rd_len; (void)timeout_ms;
    g_now_us += I2C_TXN_US;
    return ESP_ERR_TIMEOUT;   /* BNO085 驱动不用寄存器式读写 */
}

static int     g_recover_calls;
static int64_t g_recover_us[MAX_EVENTS];

esp_err_t pk_i2c0_recover_request(const char *who)
{
    (void)who;
    if (g_recover_calls < MAX_EVENTS) g_recover_us[g_recover_calls] = g_now_us;
    g_recover_calls++;
    return ESP_ERR_INVALID_STATE;
}

void pk_i2c0_client_init(pk_i2c0_client_t *c, const char *name,
                         uint32_t min_fail_count, int64_t min_fail_span_us)
{
    (void)name; (void)min_fail_count; (void)min_fail_span_us;
    memset(c, 0, sizeof(*c));
}
bool pk_i2c0_client_report(pk_i2c0_client_t *c, bool ok) { (void)c; (void)ok; return false; }
void pk_i2c0_client_reset(pk_i2c0_client_t *c) { (void)c; }

/* 安装变换：本测试不验姿态数学（那是 test_imu_mount / test_pk_board_mount 的
 * 活），给单位四元数即可。 */
pk_board_profile_t pk_board_profile(void) { return PK_BOARD_PROFILE_V4; }
const char *pk_board_profile_name(pk_board_profile_t p) { (void)p; return "host"; }
void pk_board_imu_body_fix_quat(pk_board_profile_t p, float q[4])
{ (void)p; q[0] = 1.0f; q[1] = q[2] = q[3] = 0.0f; }

bool pk_demo_enabled(void) { return false; }
bool pk_demo_imu_sample(int64_t now_us, pk_imu_sample_t *out)
{ (void)now_us; (void)out; return false; }

/* ── 调度器 ───────────────────────────────────────────────────────────── */

static jmp_buf  g_escape;
static int      g_delay_count;
static unsigned g_delay_ms[MAX_EVENTS];
static int      g_yield_budget;
static int      g_valid_true;
static int      g_valid_false;
static int64_t  g_first_valid_us = -1;
static int      g_valid_before_alive;
static int      g_delete_calls;

static void on_delay(unsigned ms)
{
    if (g_delay_count < MAX_EVENTS) g_delay_ms[g_delay_count] = ms;
    g_delay_count++;
    g_now_us += (int64_t)ms * 1000;

    pk_imu_sample_t s;
    if (pk_imu_sample_get(&s)) {
        g_valid_true++;
        if (g_first_valid_us < 0) g_first_valid_us = g_now_us;
        if (!device_alive()) g_valid_before_alive++;
    } else {
        g_valid_false++;
    }

    if (g_valid_true >= 5 || g_delay_count >= g_yield_budget) longjmp(g_escape, 1);
}

static void on_delete(void *handle)
{
    (void)handle;
    g_delete_calls++;
    longjmp(g_escape, 2);
}

/* 退避等待 = ≥1 s 的那些让渡。imu 任务自身的等待都比它短（轮询 5 ms、
 * 排空 20 ms、复位脉冲 10/250 ms），所以这个筛子在本测试里无歧义。 */
#define BACKOFF_FLOOR_MS 1000

static void check_backoff_shape(int expect_cap)
{
    unsigned prev = 0, maxd = 0;
    int count = 0, monotonic = 1, over_cap = 0;
    int n = g_delay_count < MAX_EVENTS ? g_delay_count : MAX_EVENTS;
    for (int i = 0; i < n; i++) {
        unsigned d = g_delay_ms[i];
        if (d < BACKOFF_FLOOR_MS) continue;
        count++;
        if (d > 60000) over_cap++;
        if (d < prev) monotonic = 0;
        if (d > maxd) maxd = d;
        prev = d;
    }
    CHECK(count > 0, "一次退避等待都没有——失败在忙等\n");
    CHECK(over_cap == 0, "有 %d 次退避超过 60 s 封顶\n", over_cap);
    CHECK(monotonic, "退避不是单调不减\n");
    if (expect_cap) CHECK(maxd == 60000, "退避封顶是 %u ms，应为 60000\n", maxd);
    /* 每次失败的 bring-up 之后必须有一次等待（否则就是忙等重试）。 */
    CHECK(count >= g_setfeat_rv - 1,
          "bring-up 尝试 %d 次，退避等待只有 %d 次\n", g_setfeat_rv, count);
}

static void check_recover_throttle(void)
{
    int storm = 0;
    int m = g_recover_calls < MAX_EVENTS ? g_recover_calls : MAX_EVENTS;
    for (int i = 1; i < m; i++) {
        if (g_recover_us[i] - g_recover_us[i - 1] < 8 * 1000000LL) storm++;
    }
    CHECK(storm == 0, "有 %d 次总线恢复请求间隔不足 8 s\n", storm);
}

static void run_imu(int yield_budget)
{
    g_yield_budget = yield_budget;
    pk_host_task_delay_hook  = on_delay;
    pk_host_task_delete_hook = on_delete;

    if (setjmp(g_escape) == 0) {
        esp_err_t err = pk_imu_init();
        CHECK(err == ESP_OK,
              "pk_imu_init 返回 0x%x —— 器件此刻不回话，但任务必须照样建起来\n", err);
        CHECK(g_add_device_calls == 1, "add_device 调了 %d 次，应为 1\n", g_add_device_calls);
        CHECK(pk_host_task_create_count == 1,
              "任务创建 %d 次，应为 1（长期任务必须先存在）\n", pk_host_task_create_count);
        if (pk_host_task_last_fn) pk_host_task_last_fn(pk_host_task_last_arg);
        CHECK(false, "任务函数返回了——长期任务不该 return\n");
    }
}

/* ── 场景 1：开机头 12 秒器件不回话，之后正常 ─────────────────────────── */

static void scenario_transient_then_recovers(void)
{
    g_alive_at_us = 12 * 1000000LL;
    run_imu(1200);

    CHECK(g_delete_calls == 0,
          "vTaskDelete 调了 %d 次——瞬态失败不得删掉长期任务\n", g_delete_calls);
    CHECK(g_valid_true >= 5,
          "器件恢复后没能进入工作态（valid=true 只有 %d 次）\n", g_valid_true);
    CHECK(g_valid_false > 0, "从来没采到过失败期，场景没跑起来\n");
    CHECK(g_valid_before_alive == 0,
          "器件还没应答就报了 %d 次姿态有效\n", g_valid_before_alive);
    CHECK(g_first_valid_us < 0 || g_first_valid_us >= g_alive_at_us,
          "第一次姿态有效在 %.1f s，早于器件复活时刻 %.1f s\n",
          (double)g_first_valid_us / 1e6, (double)g_alive_at_us / 1e6);
    /* 恢复不是"碰巧收到帧"，而是真的把 SH-2 init 重放了一遍。 */
    CHECK(g_setfeat_rv_ok >= 1 && g_setfeat_la_ok >= 1,
          "器件回话后没有重放 Set Feature（RV %d / LA %d）\n",
          g_setfeat_rv_ok, g_setfeat_la_ok);
    CHECK(g_setfeat_rv > 1, "只尝试过 %d 次 bring-up，没证明重试发生过\n", g_setfeat_rv);
    CHECK(g_add_device_calls == 1,
          "重试期间重复挂了器件：add_device %d 次\n", g_add_device_calls);
    CHECK(g_recover_calls <= 1,
          "一次 12 s 的瞬态失败请求了 %d 次总线恢复（风暴）\n", g_recover_calls);
    check_backoff_shape(0);
}

/* ── 场景 2：器件长期缺席 ─────────────────────────────────────────────── */

static void scenario_absent_forever_never_dies(void)
{
    g_alive_at_us = INT64_MAX;
    run_imu(900);

    CHECK(g_delete_calls == 0,
          "vTaskDelete 调了 %d 次——器件缺席也不许删任务\n", g_delete_calls);
    CHECK(g_valid_true == 0, "器件缺席却报了 %d 次姿态有效\n", g_valid_true);
    CHECK(g_setfeat_rv >= 5, "只尝试了 %d 次 bring-up，重试没在跑\n", g_setfeat_rv);
    check_backoff_shape(1);
    CHECK(g_recover_calls > 0, "长期缺席一次总线恢复都没请求过，升级路径断了\n");
    check_recover_throttle();
    CHECK(g_recover_calls < g_setfeat_rv,
          "每一轮失败都请求了总线恢复（%d 次 / %d 轮）\n",
          g_recover_calls, g_setfeat_rv);
}

/* ── 场景 3：建任务失败必须把资源还回去 ───────────────────────────────── */

static void scenario_task_create_failure_cleans_up(void)
{
    pk_host_task_create_fail = 1;
    pk_host_task_delay_hook  = on_delay;
    pk_host_task_delete_hook = on_delete;
    g_yield_budget = 100000;      /* 这个场景不跑任务循环，不该被预算打断 */
    g_alive_at_us  = 0;           /* 器件正常，失败点只在建任务 */

    esp_err_t err = pk_imu_init();

    CHECK(err != ESP_OK, "建任务失败时 pk_imu_init 却返回了 ESP_OK\n");
    CHECK(pk_host_task_create_count == 0, "任务不该创建成功\n");
    CHECK(g_add_device_calls == 1, "add_device 应调 1 次，实际 %d\n", g_add_device_calls);
    CHECK(g_rm_device_calls == 1,
          "建任务失败后没摘器件（rm_device %d 次）\n", g_rm_device_calls);
    CHECK(pk_host_mutex_live == 0,
          "建任务失败后漏了 %d 把 mutex\n", pk_host_mutex_live);
}

/* ── 入口 ─────────────────────────────────────────────────────────────── */

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
        printf("test_imu_bringup: %d scenario(s) FAILED\n", failed);
        return 1;
    }
    printf("test_imu_bringup: all scenarios passed\n");
    return 0;
}
