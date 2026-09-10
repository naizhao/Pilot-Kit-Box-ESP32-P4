/*
 * test_imu_tare_gate.c — 没有真实姿态就不许 tare / factory reset（父级复核 C）。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 \
 *      -I firmware/test/host_stubs -I firmware/main \
 *      -o /tmp/test_imu_tare_gate firmware/test/test_imu_tare_gate.c \
 *      firmware/main/imu_task.c firmware/main/pk_vib.c \
 *      firmware/main/pk_bringup_retry.c -lm \
 *   && /tmp/test_imu_tare_gate
 *
 * 缺陷（修复前）
 * --------------
 * 2026-09-09 把 bring-up 挪进 imu 任务之后，pk_imu_init() 在**芯片还没握手**
 * 时就置 s_imu_ready=true（那面旗现在的含义是"子系统起来了"）。而
 * pk_imu_tare_now / pk_imu_tare_persist / pk_imu_factory_reset 仍然只看这面
 * 旗，于是在退避重试期间：
 *   - TARE 会把**默认单位四元数**当成"当前姿态"存下来（persist 还会写进
 *     NVS），下次开机恢复出来的是一个凭空的姿态基准，PFD 的地平线从此偏着；
 *   - FACTORY RESET 会在另一个任务里发 SH-2 命令（Tare Set Reorientation /
 *     Clear DCD / bno_bring_up），与 imu 任务正在跑的 bring-up 重试并发写
 *     s_dev / s_tx_seq / s_cmd_seq —— 两个任务同时做 SH-2 会话是数据竞争。
 *
 * 判据（跑生产任务函数本身，见 test_imu_bringup.c 的同一套调度器桩）
 * ---------------------------------------------------------------
 *   1. 还没拿到任何有效姿态样本时，三个入口一律 ESP_ERR_INVALID_STATE，
 *      且**一次 NVS 写/擦都没有**、**一条 SH-2 命令帧都没发**；
 *   2. 收到第一条有效 Rotation Vector 之后才放行：tare 返回 ESP_OK、
 *      persist 真的写一次 NVS、factory reset 真的发出 SH-2 命令；
 *   3. 不得破坏"任务先创建、init 快速返回"的自愈架构——init 仍返回 ESP_OK
 *      并创建任务（这一条与 test_imu_bringup.c 的场景 1 同源，写在这里是
 *      为了让本文件单独跑也能证明门禁不是靠"把 init 改回去"实现的）。
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
#include "nvs.h"          /* pk_host_nvs_* 计数器（写/擦 NVS 的观察点） */
#include "pk_board.h"
#include "pk_i2c0_bus.h"
#include "pk_i2c0_recover.h"

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("         at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

#define I2C_TXN_US 250
#define SHTP_CH_SENSORHUB 3

static int64_t g_now_us;
int64_t esp_timer_get_time(void) { return g_now_us; }

/* ── 假 BNO085 ────────────────────────────────────────────────────── */
static bool g_alive;              /* 器件回不回话 */
static int  g_sh2_cmd_frames;     /* 控制通道上的 0xF2 Command Request 条数 */

static size_t build_rv_frame(uint8_t *out)
{
    const uint8_t cargo[12] = {
        0x05, 0x00, 0x03, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x40,          /* qw = 1.0 (Q14)，其余 0 → 单位四元数 */
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
{ (void)bus; if (out_handle) *out_handle = (i2c_master_dev_handle_t)(intptr_t)cfg->device_address;
  return ESP_OK; }
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev) { (void)dev; return ESP_OK; }
i2c_master_bus_handle_t pk_i2c0_bus_get(void) { return (i2c_master_bus_handle_t)1; }
uint32_t pk_i2c0_bus_generation(void) { return 0; }

esp_err_t pk_i2c0_bus_transmit(i2c_master_dev_handle_t dev,
                               const uint8_t *wr, size_t wr_len, int timeout_ms)
{
    (void)dev; (void)timeout_ms;
    g_now_us += I2C_TXN_US;
    /* SHTP: [0..1]=len [2]=channel [3]=seq [4..]=cargo；cargo[0]=0xF2 是
     * SH-2 Command Request（Tare / Clear DCD 都走它）。 */
    if (wr_len >= 5 && wr[4] == 0xF2) g_sh2_cmd_frames++;
    return g_alive ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t pk_i2c0_bus_receive(i2c_master_dev_handle_t dev,
                              uint8_t *rd, size_t rd_len, int timeout_ms)
{
    (void)dev; (void)timeout_ms;
    g_now_us += I2C_TXN_US;
    if (!g_alive || rd == NULL) return ESP_ERR_TIMEOUT;
    uint8_t frame[64];
    size_t total = build_rv_frame(frame);
    size_t n = rd_len < total ? rd_len : total;
    memset(rd, 0, rd_len);
    memcpy(rd, frame, n);
    return ESP_OK;
}

esp_err_t pk_i2c0_bus_transmit_receive(i2c_master_dev_handle_t dev,
                                       const uint8_t *wr, size_t wr_len,
                                       uint8_t *rd, size_t rd_len, int timeout_ms)
{ (void)dev; (void)wr; (void)wr_len; (void)rd; (void)rd_len; (void)timeout_ms;
  g_now_us += I2C_TXN_US; return ESP_ERR_TIMEOUT; }

esp_err_t pk_i2c0_recover_request(const char *who) { (void)who; return ESP_ERR_INVALID_STATE; }
void pk_i2c0_client_init(pk_i2c0_client_t *c, const char *name,
                         uint32_t a, int64_t b)
{ (void)name; (void)a; (void)b; memset(c, 0, sizeof(*c)); }
bool pk_i2c0_client_report(pk_i2c0_client_t *c, bool ok) { (void)c; (void)ok; return false; }
void pk_i2c0_client_reset(pk_i2c0_client_t *c) { (void)c; }

pk_board_profile_t pk_board_profile(void) { return PK_BOARD_PROFILE_V4; }
const char *pk_board_profile_name(pk_board_profile_t p) { (void)p; return "host"; }
void pk_board_imu_body_fix_quat(pk_board_profile_t p, float q[4])
{ (void)p; q[0] = 1.0f; q[1] = q[2] = q[3] = 0.0f; }
bool pk_demo_enabled(void) { return false; }
bool pk_demo_imu_sample(int64_t now_us, pk_imu_sample_t *out)
{ (void)now_us; (void)out; return false; }

/* ── 调度器 ───────────────────────────────────────────────────────── */
static jmp_buf g_escape;
static int  g_yields;
static int  g_budget;
static bool g_probe_done;

/* 被测的三个入口在"当时那一刻"的返回值。 */
static esp_err_t g_rc_tare, g_rc_persist, g_rc_factory;
static int g_nvs_writes_at_probe, g_nvs_erase_at_probe, g_sh2_at_probe;

/* probe_when_no_attitude: true = 在还没有有效姿态时探；false = 等到有效姿态。 */
static bool g_probe_when_no_attitude;

/* 探测期间 on_delay 必须"透明"：被测的三个入口自己会调 vTaskDelay
 * （factory reset 里有两处 100 ms 等待），若那时再进探测/再 longjmp，
 * 就会从 factory_reset 的中间跳出去（或无限递归探测把栈撑爆）。 */
static bool g_probing;

static void probe(void)
{
    g_probing = true;
    g_sh2_cmd_frames = 0;
    pk_host_nvs_set_blob_calls = 0;
    pk_host_nvs_erase_calls    = 0;

    g_rc_tare    = pk_imu_tare_now();
    g_rc_persist = pk_imu_tare_persist();
    g_rc_factory = pk_imu_factory_reset();

    g_nvs_writes_at_probe = pk_host_nvs_set_blob_calls;
    g_nvs_erase_at_probe  = pk_host_nvs_erase_calls;
    g_sh2_at_probe        = g_sh2_cmd_frames;
    g_probe_done = true;
    g_probing    = false;
}

static void on_delay(unsigned ms)
{
    g_yields++;
    g_now_us += (int64_t)ms * 1000;
    if (g_probing) return;   /* 见 g_probing 的说明 */

    pk_imu_sample_t s;
    bool attitude_valid = pk_imu_sample_get(&s);

    if (!g_probe_done) {
        if (g_probe_when_no_attitude) {
            if (!attitude_valid && g_yields >= 3) probe();
        } else if (attitude_valid) {
            probe();
        }
    }
    if ((g_probe_done && g_yields > 5) || g_yields >= g_budget) longjmp(g_escape, 1);
}

static void on_delete(void *h) { (void)h; longjmp(g_escape, 2); }

static void run_imu(bool alive, bool probe_no_attitude, int budget)
{
    g_alive = alive;
    g_probe_when_no_attitude = probe_no_attitude;
    g_budget = budget;
    pk_host_task_delay_hook  = on_delay;
    pk_host_task_delete_hook = on_delete;

    if (setjmp(g_escape) == 0) {
        esp_err_t err = pk_imu_init();
        CHECK(err == ESP_OK, "pk_imu_init 应快速返回 ESP_OK got=0x%x\n", err);
        CHECK(pk_host_task_create_count == 1, "任务必须先创建\n");
        if (pk_host_task_last_fn) pk_host_task_last_fn(pk_host_task_last_arg);
    }
}

/* ══ 场景 1：bring-up 重试期间（从未有过有效姿态）══════════════════ */
static void scenario_no_attitude_rejects_all(void)
{
    run_imu(/*alive=*/false, /*probe_no_attitude=*/true, 400);

    CHECK(g_probe_done, "没能在无姿态状态下探到（场景没跑起来）\n");
    CHECK(g_rc_tare == ESP_ERR_INVALID_STATE,
          "无有效姿态时 tare_now 应返回 INVALID_STATE got=0x%x\n", g_rc_tare);
    CHECK(g_rc_persist == ESP_ERR_INVALID_STATE,
          "无有效姿态时 tare_persist 应返回 INVALID_STATE got=0x%x\n", g_rc_persist);
    CHECK(g_rc_factory == ESP_ERR_INVALID_STATE,
          "无有效姿态时 factory_reset 应返回 INVALID_STATE got=0x%x\n", g_rc_factory);
    CHECK(g_nvs_writes_at_probe == 0,
          "被拒的 tare 不得写 NVS（写了 %d 次）\n", g_nvs_writes_at_probe);
    CHECK(g_nvs_erase_at_probe == 0,
          "被拒的 factory reset 不得擦 NVS（擦了 %d 次）\n", g_nvs_erase_at_probe);
    CHECK(g_sh2_at_probe == 0,
          "被拒的 factory reset 不得发 SH-2 命令（发了 %d 帧，正与 bring-up "
          "重试并发写 SH-2 会话）\n", g_sh2_at_probe);
}

/* ══ 场景 2：拿到第一条有效姿态之后必须放行 ═══════════════════════ */
static void scenario_after_valid_attitude_allows(void)
{
    run_imu(/*alive=*/true, /*probe_no_attitude=*/false, 400);

    CHECK(g_probe_done, "没能在有姿态状态下探到（场景没跑起来）\n");
    CHECK(g_rc_tare == ESP_OK, "有有效姿态时 tare_now 应成功 got=0x%x\n", g_rc_tare);
    CHECK(g_rc_persist == ESP_OK, "有有效姿态时 tare_persist 应成功 got=0x%x\n", g_rc_persist);
    CHECK(g_nvs_writes_at_probe >= 1,
          "放行后的 persist 必须真的写一次 NVS（写了 %d 次）\n", g_nvs_writes_at_probe);
    CHECK(g_rc_factory == ESP_OK,
          "有有效姿态时 factory_reset 应成功 got=0x%x\n", g_rc_factory);
    CHECK(g_sh2_at_probe >= 2,
          "放行后的 factory reset 应真的发出 SH-2 命令（发了 %d 帧）\n", g_sh2_at_probe);
}

static int run_forked(const char *name, void (*fn)(void))
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) { printf("%s\n", name); fn(); fflush(stdout); _exit(g_fail ? 1 : 0); }
    int status = 0;
    waitpid(pid, &status, 0);
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("  -> %s\n", ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

int main(void)
{
    int failed = 0;
    failed += run_forked("scenario 1: 无有效姿态 → tare/persist/factory 全部拒绝且无副作用",
                         scenario_no_attitude_rejects_all);
    failed += run_forked("scenario 2: 首次有效姿态之后放行",
                         scenario_after_valid_attitude_allows);
    if (failed) { printf("test_imu_tare_gate: %d scenario(s) FAILED\n", failed); return 1; }
    printf("test_imu_tare_gate: all scenarios passed\n");
    return 0;
}
