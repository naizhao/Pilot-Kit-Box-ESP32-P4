/*
 * pk_ui_nav_host.c — 把导航层的动作接到固件上。
 *
 * pk_ui_nav.c 是平台无关的（固件与模拟器编同一份），它只报告「用户点了 FAB」
 * 「拖动了 FAB」「要返回上一级」，不知道这些在固件里意味着什么。那些弱符号
 * 回调的强符号实现就落在这里。
 *
 * 单独一个文件而不是塞进 pfd.c：pfd.c 的职责是画 PFD 那一页，让它顺带管全局
 * 页面切换与 NVS，下次找「点了设置为什么没反应」时不会有人想到去翻它。
 *
 * 模拟器不编译本文件，于是那边继续用弱符号的空实现——正是这套机制存在的理由。
 */
#include "pk_ui_nav.h"

#include "esp_log.h"

#include "apt_detail_page.h"
#include "config_fab.h"
#include "i18n_catalog.h"
#include "imu_task.h"
#include "diag_page.h"
#include "map_page.h"
#include "nav_grid_page.h"
#include "search_page.h"
#include "ui_state.h"

static const char *TAG = "nav_host";

/*
 * 点 FAB 打开主菜单 = 全屏导航网格。
 *
 * 这里**没有**「页签 → 页面」的映射表了：dock 时代它回出来的是 i18n 词条
 * id，宿主查表切页；网格自己就握着项表（nav_grid_page.c 的 ITEMS），项与
 * 目标 mode 写在同一行，反而不会走偏。
 *
 * 网格自己会藏掉 FAB 并在关闭时放回来（pk_nav_grid_page_open 里那段），
 * 所以这里只管开，不必再操心浮层避让。
 */
void pk_ui_nav_on_menu(void)
{
    ESP_LOGI(TAG, "menu -> nav grid");
    pk_nav_grid_page_open();
}

/*
 * 调平：把当前姿态设为水平基准并落盘。
 *
 * 语义与 TARE 键长按完全一致（见 main.c 的按键分发），照抄那一段——同一个
 * 动作在两个入口上行为必须一样，否则「我按键调过了，怎么屏上又要调一次」。
 *
 * 用 persist 而不是 tare_now：这个入口在导航网格的动作条里，用户要按满 1 s
 * 才会走到这儿，那是明确的「就按现在这个姿态定下来」，理应活过重启。
 */
void pk_ui_nav_on_level(void)
{
    esp_err_t e = pk_imu_tare_persist();
    pk_ui_toast_show(e == ESP_OK ? PK_TR_TOAST_TARE_SAVED
                                 : PK_TR_TOAST_TARE_SAVE_FAIL,
                     e != ESP_OK);
    ESP_LOGI(TAG, "level (tare persist) -> %s", esp_err_to_name(e));
}

/* 短按：多数人会以为点一下就行，得告诉他要按住。不是错误，用 false。 */
void pk_ui_nav_on_level_hint(void)
{
    pk_ui_toast_show(PK_TR_ACT_LEVEL_HINT, false);
}

void pk_ui_nav_on_fab_moved(bool left, int y_pct)
{
    pk_fab_pos_set(left, y_pct);
}

/*
 * 二级页面返回。
 *
 * 目标写死成诊断页而不是记一个「来时的页面」：spec §4.3 定的是最多两层、
 * 且子页只能从诊断进入，所以返回目标唯一确定。真要留返回栈，得先有第二个
 * 能进子页的入口——那时再加，现在加只是替一个不存在的场景做准备。
 */
void pk_ui_nav_on_back(void)
{
    /* 诊断的子系统详情是"页内的第二层"，不是独立页面：在它里面按返回应该
     * 回到诊断总览，而不是把整页切一遍（那样会连滚动位置一起丢掉）。
     * 只有不在详情里时，返回才是"切回诊断页"。 */
    if (pk_diag_page_in_detail()) {
        pk_diag_page_leave_detail();
        return;
    }
    pk_ui_set_mode(PK_UI_MODE_DIAG);
}

/*
 * 地图页右侧那枚放大镜 —— map_page.c 里弱符号 pk_map_page_on_search 的强实现。
 *
 * 落在这里而不是 pfd.c，理由同本文件开头那段：pfd.c 只负责画 PFD 那一页，
 * 「页面之间怎么跳」归导航宿主。搜索页是模态层（不是 pk_ui_mode_t），
 * 所以这里只是把它打开，不切 mode——用户仍然在地图页上，只是被盖住了。
 */
void pk_map_page_on_search(void)
{
    pk_search_page_open();
}

/*
 * 地图上点中机场符号 —— map_page.c 里弱符号 pk_map_page_on_apt_detail 的强实现。
 *
 * 落在这里的理由同上：页面之间怎么跳归导航宿主。命中测试本身在
 * pk_aero_layer（它握着屏上要素的快照），map_page 只负责"把这一下判成点击"，
 * 三者各管一段，谁都不需要知道另外两个的内部。
 */
void pk_map_page_on_apt_detail(uint32_t apt_idx)
{
    pk_apt_detail_page_open(apt_idx, PK_APT_DETAIL_FROM_MAP);
}

void pk_ui_nav_host_init(void)
{
    pk_config_fab_load();
    pk_ui_nav_set_fab_side(pk_fab_left());

    /* 没存过就不设，让导航层保持它自己算的 FAB_DEFAULT_Y。 */
    int y = pk_fab_y_pct();
    if (y >= 0) pk_ui_nav_set_fab_y_pct(y);
}
