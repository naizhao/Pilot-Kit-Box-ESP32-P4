/*
 * pk_i2c0_recover.h — I²C0 **总线级**故障恢复的唯一仲裁点。
 *
 * 背景与那次真机日志见 pk_i2c0_policy.h 的文件头。一句话：坏的是总线，
 * 不是某个器件；所以 imu_task 那个只重新初始化 BNO085 的 5 秒 stall
 * watchdog 必然救不回来（日志里 `bring-up after stall failed:
 * ESP_ERR_TIMEOUT` 就是证据），baro_task 更是连重试都没有。
 *
 * 恢复动作只有一个：i2c_master_bus_reset()
 * ---------------------------------------
 * IDF v6.0.1 现成的 API（driver/i2c_master.h:313）。它做两件事：
 *   1. s_i2c_master_clear_bus()：SCL 上打最多 9 个时钟再补一个 STOP，
 *      把被从机拖在低电平的 SDA 放出来（i2c_master.c:53）；
 *   2. 复位 I²C 控制器的硬件状态机并恢复时序/滤波配置（同文件 :108）。
 * 它**不销毁 bus handle，已挂的 device handle 也不失效**——这正是选它
 * 而不是 i2c_del_master_bus + 重建的原因：后者要牵动 imu / baro / touch
 * 三个模块的 handle 生命周期，代价大得多。
 *
 * 器件怎么重新初始化：靠"代数"，不靠回调
 * ------------------------------------
 * 恢复成功后 pk_i2c0_recover_generation() 会 +1。各器件任务在**自己的
 * 循环里**比对代数，发现变了就重放自己既有的 bring-up：
 *   imu_task  → bno_bring_up()          （拉 RST + 重放 SH-2 init）
 *   baro_task → configure_and_calibrate()（重写 OSR/ODR/IIR/PWR + 重读标定）
 *
 * 为什么不做成"注册回调、恢复时由请求方逐个调"：那样 baro 任务会去调
 * imu 的 bring-up，而 imu 任务此刻正在同一条总线上轮询、并且会写
 * s_tx_seq / s_dev——两个任务同时跑 bring-up 是数据竞争。代数比对让每个
 * 器件都在自己的任务上下文里重来一遍，天然没有这个问题，也顺带避免请求
 * 方被 BNO085 那 750 ms 的复位+排空阻塞住。
 *
 * 「拔插一律用代数、别轮询电平」这条同样是 SD 热插拔那次踩出来的教训
 * （pk_sdcard 的挂载代数），这里照抄。
 *
 * 这套东西**盖不住**的失败模式（别指望它，出事时先排除这几条）
 * ----------------------------------------------------------
 *   1. 从机把 SDA 焊死在低电平，9 拍时钟也放不出来。i2c_master_bus_reset()
 *      的 clear_bus 只打到 9 拍就收手（i2c_master.c:69 那个 i++ < 9），
 *      从机若是在一个长读里被打断且还要更多拍才走完一个字节，就救不回来。
 *      症状：复位返回 ESP_OK，但两颗芯片探活全无应答，日志里
 *      「恢复失败：总线已复位但 0x4A / 0x76 均无应答」一路退避到 30 s。
 *      真要治得给每颗芯片单独的电源门或复位线，这块板上没有。
 *   2. GT911 只有 RST（GPIO23）没有可用的 INT（R35 不贴），本模块**不会**
 *      去动它——重新走一遍它的初始化时序需要在 LVGL 那个任务里做，见
 *      touch_gt911.c 的 pk_touch_retry_after_bus_recovery()。所以"GT911
 *      拖死总线"这个**根因**这里治不了，只治得了它造成的后果。
 *   3. 复位的那一瞬间，别的任务可能正有一笔事务在飞。i2c_master_bus_reset()
 *      不取 IDF 的 bus_lock_mux（i2c_master.c:1241），那笔事务会被打断、
 *      以超时告终。本机所有事务都带 100 ms 超时，代价上限就是一次 100 ms
 *      的失败——而走到这一步时总线本来就不通了。要彻底消掉这个窗口，得让
 *      imu / baro / touch 三处**每一笔**事务都过一道共享的门，触摸那一路
 *      在 LVGL 的 read_cb 里，代价和风险都比这个窗口本身大。
 *   4. 板载音频 codec 也在这条总线上，它不参与探活、也不会重新初始化。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "pk_i2c0_policy.h"

/* 一个器件的"我这边到底还行不行"计数器 + 它的名字（只进日志）。
 * 每个器件任务持有一份，**只在自己的任务里**读写，所以无需加锁。 */
typedef struct {
    const char        *name;
    pk_i2c0_detector_t det;
} pk_i2c0_client_t;

/* min_fail_count / min_fail_span_us 的含义见 pk_i2c0_detector_init()。 */
void pk_i2c0_client_init(pk_i2c0_client_t *c, const char *name,
                         uint32_t min_fail_count, int64_t min_fail_span_us);

/* 报一次总线访问结果。达到去抖门槛时会自动调 pk_i2c0_recover_request()。
 * 返回 true = 本次调用真的跑完了一轮总线恢复（被冷却/不重入挡下则为 false）。 */
bool pk_i2c0_client_report(pk_i2c0_client_t *c, bool ok);

/* 把失败计数清零。器件刚重新初始化成功、或刚看到总线代数变化时调。 */
void pk_i2c0_client_reset(pk_i2c0_client_t *c);

/*
 * 直接请求一轮总线恢复（绕过去抖，但仍受串行化与冷却约束）。
 * 给"上层自己已经确信总线坏了"的场合用，例如 baro 开机连探 10 次
 * CHIP_ID 全败。who 只进日志。
 *
 * 返回：
 *   ESP_OK                —— 真的做了一轮，且复位后器件有应答
 *   ESP_ERR_INVALID_STATE —— 被串行化/冷却挡下，本次什么都没做
 *   ESP_ERR_NOT_FOUND     —— 做了，但复位后 0x4A / 0x76 都没应答（总线还是死的）
 *   其它                  —— i2c_master_bus_reset() 自己返回的错误
 */
esp_err_t pk_i2c0_recover_request(const char *who);

/*
 * 成功复位过的总线轮数。开机为 0，每成功恢复一轮 +1。
 *
 * 各器件任务的用法（照抄 imu_task.c / baro_task.c 里那两处）：
 *
 *     static uint32_t bus_gen = 0;               // 任务起来时取一次
 *     ...
 *     uint32_t gen = pk_i2c0_recover_generation();
 *     if (gen != bus_gen) { bus_gen = gen; 重放自己的 bring_up(); }
 *
 * 用"变了没有"而不是"等于几"，这样谁先谁后、中途漏看几轮都不影响正确性。
 */
uint32_t pk_i2c0_recover_generation(void);
