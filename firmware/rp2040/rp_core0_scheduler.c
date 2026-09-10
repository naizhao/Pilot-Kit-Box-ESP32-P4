#include "rp_core0_scheduler.h"

unsigned rp_core0_schedule_once(const rp_core0_ops_t *ops)
{
    unsigned sent = 0;
    while (sent < RP_CORE0_MODES_BUDGET &&
           ops->send_one_modes(ops->user))
        sent++;

    ops->poll_p4_rx(ops->user);
    ops->poll_spim(ops->user);
    ops->poll_control(ops->user);
    ops->poll_periodic(ops->user);
    return sent;
}
