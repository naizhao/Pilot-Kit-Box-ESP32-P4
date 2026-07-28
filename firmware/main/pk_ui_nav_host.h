/*
 * pk_ui_nav_host.h — 导航层在固件侧的绑定。
 *
 * 回调（on_tab / on_level / on_fab_moved / on_back …）是弱符号覆盖，不需要
 * 声明也会自动生效；对外只暴露开机时那一次恢复。
 */
#pragma once

/* 读 NVS 并把 FAB 摆回上次的落点。须在 pk_ui_nav_init() 之后调用——
 * 它要操作已经建出来的 FAB 对象。 */
void pk_ui_nav_host_init(void);
