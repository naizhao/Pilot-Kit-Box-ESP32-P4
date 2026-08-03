/* pk_i2c0_recover.c — 见 pk_i2c0_recover.h 的设计说明。 */

#include "pk_i2c0_recover.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "imu_task.h"   /* pk_i2c0_bus_get() —— 全局唯一的 I²C0 handle */

/* 独立的 tag。这个故障是偶发的，下次现场只有串口日志可看，必须能一眼
 * grep 出"总线塌了 → 恢复中 → 成功/失败"这条线。 */
static const char *TAG = "i2c0";

/* 总线上必然存在的两颗芯片。复位之后拿它们探活，作为"到底救没救回来"
 * 的判据——只看 i2c_master_bus_reset() 的返回值是不够的，它复位的是本
 * 机这侧的控制器，从机还被拖死时照样返回 ESP_OK。 */
#define PK_I2C0_ADDR_BNO085   0x4A
#define PK_I2C0_ADDR_BMP388   0x76

/* 探活超时取 250 ms，而不是 touch_gt911.c 探地址时用的 50 ms。
 * i2c_master_probe() 的这个参数**同时**是"等总线锁"和"等这笔传输"的上限
 * （i2c_master.c:1353 与 :1391 共用 ticks_to_wait）。刚复位完的这一刻，
 * 很可能有别的任务的事务正被打断、还要最多 100 ms 才超时退出并放锁
 * （imu 的 shtp_send/recv、baro 的 reg_read/write 全是 100 ms 超时）。
 * 取 50 ms 会先于它超时，把"锁没抢到"误判成"总线还是死的"，白白多退避
 * 一轮。芯片正常时探活是微秒级返回，这个上限平时一点都不花。 */
#define PK_I2C0_PROBE_MS      250

/* 冷却：一轮恢复之后至少隔 2 s，失败则 2/4/8/16/32 s 指数退避，封顶 30 s。
 *
 * 2 s 的来历：BNO085 一次 bring-up 是 250 ms 复位 + 500 ms 排空 ≈ 800 ms，
 * BMP388 一次 configure_and_calibrate 含 100 ms 等首转。两个器件重来一遍
 * 差不多 1 s，冷却必须比它长，否则"恢复 → 器件还没init完 → 又判定失败 →
 * 又恢复"会自己咬自己的尾巴。
 *
 * 封顶 30 s 而不是无限退避：真出问题时用户在飞，30 s 一次的重试成本可以
 * 忽略（一轮 <1 ms 的 reset + 一次 50 ms 探测），而"再也不试了"意味着
 * 一次偶发抖动就得落地重启。 */
#define PK_I2C0_COOLDOWN_BASE_US   (2 * 1000000LL)
#define PK_I2C0_COOLDOWN_MAX_US    (30 * 1000000LL)

/* 全局唯一的闸门。用 spinlock 而不是 FreeRTOS mutex：
 *   - 闸门操作只有几条赋值，临界区以纳秒计，不会拖住别的核；
 *   - 不需要动态分配，也就没有"谁负责创建这把锁"的初始化次序问题
 *     （请求方可能是 imu 任务，也可能是 baro 任务，谁先起来不确定）。
 * 真正耗时的 bus_reset + probe 在临界区**之外**跑，靠 gate 的 busy 位
 * 保证同一时刻只有一个任务在跑。 */
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static pk_i2c0_gate_t    s_gate = {
    .cooldown_base_us = PK_I2C0_COOLDOWN_BASE_US,
    .cooldown_max_us  = PK_I2C0_COOLDOWN_MAX_US,
};

uint32_t pk_i2c0_recover_generation(void)
{
    uint32_t g;
    taskENTER_CRITICAL(&s_mux);
    g = s_gate.generation;
    taskEXIT_CRITICAL(&s_mux);
    return g;
}

esp_err_t pk_i2c0_recover_request(const char *who)
{
    if (who == NULL) who = "?";

    i2c_master_bus_handle_t bus = pk_i2c0_bus_get();
    if (bus == NULL) {
        /* 总线还没建起来（pk_imu_init 之前），没有可恢复的对象。 */
        return ESP_ERR_INVALID_STATE;
    }

    const int64_t t0 = esp_timer_get_time();

    pk_i2c0_gate_decision_t d;
    uint32_t round;
    taskENTER_CRITICAL(&s_mux);
    d     = pk_i2c0_gate_begin(&s_gate, t0);
    round = s_gate.generation + 1;   /* 只用于日志：本轮如果成功会是第几轮 */
    taskEXIT_CRITICAL(&s_mux);

    if (d == PK_I2C0_GATE_BUSY) {
        ESP_LOGD(TAG, "%s 请求恢复，但已有一轮在跑 —— 跳过", who);
        return ESP_ERR_INVALID_STATE;
    }
    if (d == PK_I2C0_GATE_COOLDOWN) {
        ESP_LOGD(TAG, "%s 请求恢复，但仍在冷却 —— 跳过", who);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGE(TAG, "I²C0 总线塌了（%s 报的）—— 开始第 %lu 轮总线级恢复",
             who, (unsigned long)round);

    /* ① 复位总线：SCL 打 9 拍放掉被拖住的 SDA + 复位控制器状态机。
     *
     * 已知窗口：i2c_master_bus_reset() 内部**不取** IDF 的 bus_lock_mux
     * （i2c_master.c:1241 没有加锁），所以别的任务此刻若正好有一笔事务在
     * 飞，那笔事务会被打断、以超时告终。本机所有 I²C 事务都带 100 ms 超时
     * （imu 的 shtp_send/recv、baro 的 reg_read/write 都是 100），代价上限
     * 就是一次 100 ms 的失败——而走到这里时总线本来就已经不通了。 */
    esp_err_t err = i2c_master_bus_reset(bus);

    /* ② 探活。任一颗应答就算总线回来了：只用一颗做判据的话，那颗芯片本身
     *    坏了/没焊会把总线误判成永远救不回来，退避一路涨到 30 s 封顶。 */
    bool imu_ack = false, baro_ack = false;
    if (err == ESP_OK) {
        /* i2c_master_probe() 是取 bus_lock_mux 的（i2c_master.c:1354），
         * 到这一步就已经和别的任务的事务重新串行起来了。
         *
         * 它只发 START + 地址 + STOP（i2c_master.c:1365 那张 i2c_ops），
         * 一个数据字节都不写，对 BNO085 的 SHTP 会话和 BMP388 的寄存器
         * 指针都没有副作用。它会把总线时序临时设成 100 kHz 且不还原，但
         * 每笔器件事务都会按自己 add_device 时的 scl_speed_hz 重设时序——
         * touch_gt911.c 开机探地址走的就是同一条路，400 kHz 照常。 */
        imu_ack  = (i2c_master_probe(bus, PK_I2C0_ADDR_BNO085, PK_I2C0_PROBE_MS) == ESP_OK);
        baro_ack = (i2c_master_probe(bus, PK_I2C0_ADDR_BMP388, PK_I2C0_PROBE_MS) == ESP_OK);
    }
    const bool recovered = (err == ESP_OK) && (imu_ack || baro_ack);

    const int64_t t1 = esp_timer_get_time();
    int64_t cooldown_us;
    taskENTER_CRITICAL(&s_mux);
    pk_i2c0_gate_finish(&s_gate, recovered, t1);
    cooldown_us = pk_i2c0_gate_cooldown_us(&s_gate);
    round       = s_gate.generation;
    taskEXIT_CRITICAL(&s_mux);

    if (recovered) {
        ESP_LOGW(TAG, "I²C0 恢复成功（第 %lu 轮，耗时 %lld ms；"
                      "探活 BNO085(0x4A)=%s BMP388(0x76)=%s）—— "
                      "各器件将在下一轮循环里重放自己的初始化",
                 (unsigned long)round, (long long)((t1 - t0) / 1000),
                 imu_ack ? "ACK" : "无应答", baro_ack ? "ACK" : "无应答");
        return ESP_OK;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I²C0 恢复失败：i2c_master_bus_reset = %s；"
                      "下次最早 %.1f s 后再试",
                 esp_err_to_name(err), (double)cooldown_us / 1e6);
        return err;
    }
    ESP_LOGE(TAG, "I²C0 恢复失败：总线已复位但 0x4A / 0x76 均无应答"
                  "（从机仍被拖死？）；下次最早 %.1f s 后再试",
             (double)cooldown_us / 1e6);
    return ESP_ERR_NOT_FOUND;
}

/* ── 器件侧的小外壳 ────────────────────────────────────────────────── */

void pk_i2c0_client_init(pk_i2c0_client_t *c, const char *name,
                         uint32_t min_fail_count, int64_t min_fail_span_us)
{
    if (c == NULL) return;
    c->name = (name != NULL) ? name : "?";
    pk_i2c0_detector_init(&c->det, min_fail_count, min_fail_span_us);
}

void pk_i2c0_client_reset(pk_i2c0_client_t *c)
{
    if (c == NULL) return;
    pk_i2c0_detector_reset(&c->det);
}

bool pk_i2c0_client_report(pk_i2c0_client_t *c, bool ok)
{
    if (c == NULL) return false;
    if (!pk_i2c0_detector_report(&c->det, ok, esp_timer_get_time())) return false;
    return pk_i2c0_recover_request(c->name) == ESP_OK;
}
