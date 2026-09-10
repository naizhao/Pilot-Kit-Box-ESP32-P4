/* RP2040 core0 一次调度轮次：Mode-S 发送与链路/控制服务的公平边界。 */
#pragma once

#include <stdbool.h>

/* 921600 baud 下每帧阻塞发送约数百微秒；8 帧把其他链路的最坏等待
 * 控制在数毫秒，同时可在每轮吸收一批突发。 */
#define RP_CORE0_MODES_BUDGET 8u

typedef struct {
    bool (*send_one_modes)(void *user);
    void (*poll_p4_rx)(void *user);
    void (*poll_spim)(void *user);
    void (*poll_control)(void *user);
    void (*poll_periodic)(void *user);
    void *user;
} rp_core0_ops_t;

/* 返回本轮实际发送的 Mode-S 帧数。 */
unsigned rp_core0_schedule_once(const rp_core0_ops_t *ops);
