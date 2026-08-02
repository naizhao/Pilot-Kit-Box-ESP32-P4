/*
 * nav_grid_page.h — 全屏导航网格（原横向 dock 的替代品）。
 *
 * 为什么要换掉 dock
 * ------------------
 * 原来的横向导航 dock 在 4.3 寸屏（800×480）上按 8 个页签排开会溢出 41 px
 * （见 pk_ui_nav.c:255 的注释），且触摸目标越挤越窄。方案 A/C 两版全屏网格
 * 原型经罩哥 2026-08-02 评审，方案 C（4×2 网格 + 底部动作条 + 分页）拍板——
 * 版面真源是 `sim/proto-navgrid/proto.c` 的 `render_c()` / `draw_bright_pop()`，
 * 本文件的常量必须与它保持一致，改版面先改那份原型再抄回来。
 *
 * 为什么是模态层、为什么自绘不用 LVGL
 * ------------------------------------
 * 与 search_page / apt_detail_page 同一套理由（见两者头文件）：dock 的第 8
 * 个页签已经溢出屏幕，做成 pk_ui_mode_t 的新页签只会让问题更大；模态层只需
 * 改 render/touch 两处分派 + 新增本文件，代价小一个数量级。
 * 但网格本身**不能用 LVGL 对象树**画：全屏网格需要一整块 800×480×2 =
 * 768 KB 的绘制表面，而 `CONFIG_LV_MEM_SIZE_KILOBYTES=64`——LVGL 堆总共只有
 * 64 KB，分配不出这么大的一块 layer，失败即死机（同
 * project_lvgl_transform_layer_hang 那次教训）。所以网格与 search_page /
 * apt_detail_page 一样，直接在 RGB565 framebuffer 上手绘。
 *
 * 分页切分点：按常用度，不按塞满切
 * ---------------------------------
 * 第 1 页放 7 项飞行中会用的（PFD/交通/地图/列表/搜索/记录/工具），第 8 格
 * 留空作余量；第 2 页放 3 项地面才碰的（诊断/设置/关于）。诊断/设置/关于
 * 挪去第 2 页不是为了凑数，是"飞行中最常按的那几个不该因为要塞满一页而往
 * 后挪"（proto.c 里 PAGE1_CNT 上方那段论证）。
 *
 * 格子位置钉死，不随项数居中
 * ---------------------------
 * 第 2 页只坐 3 项，也是从左上角起排、不居中——位置一旦随项数浮动，同一个
 * 功能在两页上就会落在不同坐标，手指记不住位置，而"记得住位置"正是网格
 * 相对列表的全部优势。产品负责人 2026-08-02 明确否掉了"末页居中"这条路。
 * 详见 test_nav_grid_page.c 的 test_cell_position_stable。
 *
 * 本文件范围：Task 2 只做纯函数区（分页切分 + 命中判定）+ host 单测。
 * 渲染（draw_*）与触摸状态机（打开/关闭/拖动分页）是后续任务，平台区先留
 * 好 `#ifndef PK_NAV_GRID_HOST_TEST` 的结构占位。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 版面常量（800×480）。
 *
 * 不引用 display.h 的 PK_DISPLAY_W/PK_DISPLAY_H：display.h 会 include
 * esp_err.h，这是 ESP-IDF 的头，host 单测的裸 cc 编译不过。本文件的
 * host 单测（test_nav_grid_page.c）要求纯 C11 + 无 IDF 依赖直接编译，
 * 所以这里用字面量 800/480，仅在下面这两处出现，改动时对照
 * proto.c 的 PK_DISPLAY_W/PK_DISPLAY_H 同步。
 *
 * y=0    顶栏 48 px（保留，菜单期间电量/GPS 仍可见）
 * y=48   网格区：4 列 × 2 行，格 200 × 156
 * y=360  网格区下沿
 * y=368  分页点（12 px，仅状态指示，不可点）
 * y=392  动作条 88 px：调平 | 亮度 | 关闭
 * y=480
 */
#define PK_NAV_BAR_BOT     48
#define PK_NAV_ACT_H       88
#define PK_NAV_ACT_TOP     (480 - PK_NAV_ACT_H)          /* 392 */
#define PK_NAV_DOT_Y       (PK_NAV_ACT_TOP - 24)         /* 368 */
#define PK_NAV_COLS        4
#define PK_NAV_ROWS        2
#define PK_NAV_CELL_W      (800 / PK_NAV_COLS)           /* 200 */
/* 网格区下沿留 8 px 给分页点呼吸——CELL_H 是表达式而不是写死的 156，
 * 动作条高度（PK_NAV_ACT_H）一改，格高必须跟着走，不能有第二个地方要改。 */
#define PK_NAV_CELL_H      ((PK_NAV_DOT_Y - PK_NAV_BAR_BOT - 8) / PK_NAV_ROWS)  /* 156 */
#define PK_NAV_PAGE_CAP    (PK_NAV_COLS * PK_NAV_ROWS)   /* 8 */

/* 第 1 页实放 7 项（PFD/交通/地图/列表/搜索/记录/工具），第 8 格留作余量。
 * 切分点是常用度，不是塞满页面——见文件头。 */
#define PK_NAV_PAGE1_CNT   7
/* 项目总数：第 1 页 7 项 + 第 2 页 3 项（诊断/设置/关于）。 */
#define PK_NAV_ITEM_CNT    10
#define PK_NAV_PAGES       2

/* 命中判定的结果种类。BRIGHT_STEP 的 index 对应 display.h 的
 * PK_BL_STEP_LOW/MID/HIGH（0/1/2）——本头文件不 include display.h（同上，
 * host 编译问题），调用方自己把 index 转成那三个枚举。 */
typedef enum {
    PK_NAV_HIT_NONE = 0,
    PK_NAV_HIT_CELL,        /* index = 全局项序号（已含翻页偏移） */
    PK_NAV_HIT_LEVEL,
    PK_NAV_HIT_BRIGHT,
    PK_NAV_HIT_CLOSE,
    PK_NAV_HIT_BRIGHT_STEP, /* index = 0..2，对应 display.h 的 PK_BL_STEP_* */
} pk_nav_hit_kind_t;

typedef struct {
    pk_nav_hit_kind_t kind;
    int               index;
} pk_nav_hit_t;

/* ── 纯函数区：无 OS / 无全局状态，host 单测直接把本模块的 .c 拉进翻译单元
 * 编译（firmware/test/test_nav_grid_page.c）。同 search_page.h /
 * apt_detail_page.h 的分区惯例。 ── */

/* 某一页第一项的全局序号（page 从 0 起）。 */
int pk_nav_page_first(int page);

/* 某一页实放几项（第 2 页只有 3 项，不是塞满的 8 项）。 */
int pk_nav_page_count(int page);

/* 某一项能不能点。未实现的页面（记录/工具）先置灰占位而不是从版面拿掉——
 * 格子位置要永久钉死，将来补上这两个页面时版面一个数都不用改（产品负责人
 * 2026-08-02 定）。越界 index 一律当不可用。 */
bool pk_nav_item_enabled(int index);

/*
 * 命中判定：屏幕坐标 (x, y) 落在哪个可点区域。
 *
 * pop_open=true 时网格整层视为不可点——亮度快调面板已经把网格压暗，
 * "看得见的不是点得中的"会破坏触摸的基本约定，所以 pop 打开期间只测三个
 * 快调按钮，其余一律 PK_NAV_HIT_NONE（点击面板外 = 收起 pop，由调用方
 * 结合 PK_NAV_HIT_NONE 这个返回值自己处理，本函数不关心"收起"这个动作）。
 */
pk_nav_hit_t pk_nav_hit_test(int x, int y, int page, bool pop_open);

#ifdef __cplusplus
}
#endif
