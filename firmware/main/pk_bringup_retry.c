/* pk_bringup_retry.c — 见 pk_bringup_retry.h 的病因与契约。 */

#include "pk_bringup_retry.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

uint32_t pk_bringup_retry_next_ms(uint32_t cur_ms, uint32_t max_ms)
{
    if (max_ms == 0) return 0;
    if (cur_ms >= max_ms) return max_ms;
    /* 先比再乘：cur*2 溢出 uint32 会绕回一个很小的值，那正好是「退避看起来
     * 生效了、实际退回忙等」这种不会有人发现的失效。 */
    if (cur_ms > max_ms / 2) return max_ms;
    return cur_ms * 2;
}

uint32_t pk_bringup_retry_run(const pk_bringup_retry_t *cfg)
{
    uint32_t max_ms   = cfg->backoff_max_ms   ? cfg->backoff_max_ms   : PK_BRINGUP_RETRY_MAX_MS;
    uint32_t backoff  = cfg->backoff_start_ms ? cfg->backoff_start_ms : PK_BRINGUP_RETRY_START_MS;
    if (backoff > max_ms) backoff = max_ms;

    for (uint32_t attempt_no = 1; ; attempt_no++) {
        if (cfg->attempt(cfg->ctx)) return attempt_no;

        if (cfg->on_failed_attempt) cfg->on_failed_attempt(cfg->ctx, attempt_no, backoff);

        /* 无条件让渡。器件缺席时这条循环会跑到关机为止，少一次让渡就是把
         * 一个核喂给 IDLE 之外的死循环（taskYIELD 饿死 IDLE 那次的教训：
         * 让渡必须是真的 vTaskDelay，不是 taskYIELD）。 */
        vTaskDelay(pdMS_TO_TICKS(backoff));
        backoff = pk_bringup_retry_next_ms(backoff, max_ms);
    }
}
