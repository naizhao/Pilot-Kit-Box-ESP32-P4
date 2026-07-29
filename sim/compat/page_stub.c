/*
 * page_stub.c — 让「关于 / 设置 / 诊断」这类整页渲染能在模拟器里跑起来。
 *
 * 这些页面本身只是把运行时状态排版成像素，排版逻辑与数据来源无关。固件侧
 * 那些数据来自 NVS、芯片寄存器、各任务的实时状态，PC 上都不存在——于是这里
 * 给一组**固定但真实感的**值，让版面按真实字符串长度铺开。
 *
 * 与 mock_runtime.c 的分工：那边桩的是 PFD 要的飞行数据（姿态/交通/气压），
 * 这边桩的是整页视图要的设备信息。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"

const esp_app_desc_t *esp_app_get_description(void)
{
    /* 按**最糟情况**填，不是按好看的值填。
     *
     * version 是 git describe 的产物，编译安装后长这样：标签 + 提交数 +
     * 短 sha + dirty 标记。之前桩成 "0.9.3-4.3in" 这种短值，模拟器上排得
     * 好好的，真机上一溢出就露馅。 */
    static const esp_app_desc_t d = {
        .project_name = "pilot_kit_box",
        .version      = "0.9.3-4.3in-127-g1a2b3c4d-dirty",
        .date         = "Jul 28 2026",
        .time         = "21:40:00",
    };
    return &d;
}

void esp_chip_info(esp_chip_info_t *out)
{
    if (!out) return;
    /* 与手上这块 Waveshare 板一致：ESP32-P4 v1.3（revision 编码 = 主*100+次）。 */
    out->model = 0; out->features = 0; out->cores = 2; out->revision = 103;
}

/* ── ui_state 的只读部分 ────────────────────────────────────────
 * 滚动位置在模拟器里恒为 0：截图要的是「页面顶端长什么样」。 */
int     pk_ui_about_scroll_y(void)          { return 0; }
int     pk_ui_diag_scroll_y(void)           { return 0; }
uint8_t pk_ui_cal_wizard_last_accuracy(void){ return 3; }   /* 3 = 已校准 */

/* ── logo ───────────────────────────────────────────────────────
 * 固件里这张图由 EMBED_FILES 链进 .rodata，模拟器没有那套机制，直接从源文件
 * 读同一份数据——保证两边显示的是同一张图，而不是各画各的占位。 */
#define LOGO_W 160
#define LOGO_H 160

const uint16_t *pk_logo_bitmap(int *w, int *h)
{
    static uint16_t px[LOGO_W * LOGO_H];
    static int loaded;          /* 0 未试过，1 成功，-1 失败 */

    if (w) *w = LOGO_W;
    if (h) *h = LOGO_H;
    if (loaded) return loaded > 0 ? px : NULL;

    /* 相对源码树定位：模拟器总是从仓库里跑起来的。 */
    const char *paths[] = {
        "firmware/main/pk_logo.rgb565",
        "../firmware/main/pk_logo.rgb565",
        "../../firmware/main/pk_logo.rgb565",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) continue;
        size_t n = fread(px, 1, sizeof(px), f);
        fclose(f);
        if (n == sizeof(px)) { loaded = 1; return px; }
    }
    fprintf(stderr, "page_stub: 找不到 pk_logo.rgb565，关于页将不画 logo\n");
    loaded = -1;
    return NULL;
}

/* ── 交通页要的那几样 ────────────────────────────────────────────
 * 飞行数据本身由 mock_runtime.c 提供（与 PFD 的交通叠加同源），这里补的是
 * 机型数据库与用户设置——前者固件里是 8 MB 的离线库，后者存在 NVS。 */
#include "aircraft_db.h"
#include "config_traffic.h"
#include "own_ship.h"

const char *pk_aircraft_type_code(uint32_t icao24)
{
    /* 给几种常见机型轮换，好让列表看起来像真的。 */
    static const char *kTypes[] = { "A320", "B738", "A359", "B77W", "E190" };
    return kTypes[icao24 % (sizeof(kTypes) / sizeof(kTypes[0]))];
}

/* 完整机型名。抽屉里最长的一格，正好用来压「值超出格宽要降档」那条路径。 */
const char *pk_aircraft_type_model(uint32_t icao24)
{
    static const char *kModel[] = {
        "Airbus A320-214", "Boeing 737-89P", "Airbus A350-941",
        "Boeing 777-39L(ER)", "Embraer ERJ-190",
    };
    return kModel[icao24 % (sizeof(kModel) / sizeof(kModel[0]))];
}

const char *pk_aircraft_type_desc(uint32_t icao24)
{
    static const char *kDesc[] = {
        "Airbus A320", "Boeing 737-800", "Airbus A350-900",
        "Boeing 777-300ER", "Embraer E190",
    };
    return kDesc[icao24 % (sizeof(kDesc) / sizeof(kDesc[0]))];
}

const char *pk_aircraft_registration(uint32_t icao24)
{
    static char reg[12];
    snprintf(reg, sizeof(reg), "B-%04X", (unsigned)(icao24 & 0xFFFF));
    return reg;
}

/* 朝向与量程在模拟器里也要可变——按钮改的就是它们，写死就验证不了交互。 */
/* PK_SIM_ORIENT=north 起手就用正北朝上——要验证「本机符号跟着航向转」
 * 只能在这个模式下看，机头朝上模式里它恒指屏幕上方。 */
static pk_map_orient_t s_orient_init(void)
{
    const char *e = getenv("PK_SIM_ORIENT");
    return (e && (e[0] == 'n' || e[0] == 'N')) ? PK_MAP_NORTH_UP
                                               : PK_MAP_HEADING_UP;
}
static pk_map_orient_t s_orient = (pk_map_orient_t)-1;
static int             s_range_idx = 3;               /* 20 NM，同真机默认 */

pk_map_orient_t pk_map_orient_get(void)
{
    if ((int)s_orient < 0) s_orient = s_orient_init();
    return s_orient;
}
void pk_map_orient_set(pk_map_orient_t m)      { s_orient = m; }
int  pk_traffic_range_idx_get(void)            { return s_range_idx; }
void pk_traffic_range_idx_set(int idx)         { s_range_idx = idx < 0 ? 0 : (idx > 3 ? 3 : idx); }

/* 航向解析：模拟器固定给一个朝向，够验证版面与旋转方向。 */
bool pk_own_heading_resolve(bool own_valid, pk_own_src_t own_src,
                            const aircraft_t *own,
                            bool imu_valid, float imu_yaw_deg,
                            float *out_deg, pk_hdg_src_t *out_src)
{
    (void)own_valid; (void)own_src; (void)own; (void)imu_valid;
    if (out_deg) *out_deg = imu_yaw_deg;
    if (out_src) *out_src = PK_HDG_SRC_IMU;
    return true;
}

int pk_traffic_range_nm(int idx)
{
    static const int kNm[4] = { 2, 5, 10, 20 };
    if (idx < 0) idx = 0;
    if (idx > 3) idx = 3;
    return kNm[idx];
}

/* 选中第一架，好让详情卡片有内容可画；-1 表示无选中。 */
int pk_ui_traffic_resolve(const uint32_t *icaos, size_t n)
{
    (void)icaos;
    return n > 0 ? 0 : -1;
}

/* 看板页的选中行。默认取中间那架而不是第 0 行：滚动窗口要把选中行钉在中央，
 * 选 0 行则永远滚不起来，「超出一屏」这个最糟情况就压不到。
 * PK_SIM_LIST_SEL=<row> 可指定，用来截图验证首/末行的边界处理。 */
int pk_ui_list_resolve_row(const uint32_t *icaos, size_t n)
{
    (void)icaos;
    if (n == 0) return -1;
    const char *e = getenv("PK_SIM_LIST_SEL");
    int row = e ? atoi(e) : (int)n / 2;
    if (row < 0) row = 0;
    if (row >= (int)n) row = (int)n - 1;
    return row;
}

void pk_ui_list_scroll(int delta) { (void)delta; }

uint32_t pk_ui_list_get_selected_icao(void) { return 0; }

/* ── 设置页要的那几样 ──────────────────────────────────────────────
 * 全是 getter：设置页的绘制层只读状态。写操作留在 settings_page.c，
 * 那个文件依赖 FreeRTOS，不进模拟器。
 *
 * 默认值按**最糟情况**给，不给"一切正常"的理想值：SD 未挂载（格式化按钮
 * 该置灰）、格式化处于已 ARM（"TAP AGAIN 5s" 那个态平时截不到）、亮度取
 * 中档。PK_SIM_SET_* 可逐项覆盖，用来把各状态都截一遍。 */
#include "config_qnh.h"
#include "config_storage.h"

static int sim_env(const char *k, int dflt)
{
    const char *e = getenv(k);
    return e ? atoi(e) : dflt;
}

float pk_qnh_get(void) { return sim_env("PK_SIM_SET_QNH", 101325) / 100.0f; }

pk_log_store_t pk_log_store_get(void)
{
    return sim_env("PK_SIM_SET_STORE", 0) ? PK_LOG_STORE_SD : PK_LOG_STORE_FLASH;
}

uint8_t pk_backlight_level_get(void) { return (uint8_t)sim_env("PK_SIM_SET_BL", 1); }

/* 默认 false：无卡时格式化按钮置灰、存储那行也置灰，这是出厂开机的样子，
 * 也是最容易被漏掉的一种版面。 */
bool pk_sdcard_is_mounted(void) { return sim_env("PK_SIM_SET_SD", 0) != 0; }

int pk_settings_format_state(void) { return sim_env("PK_SIM_SET_FMT", 0); }

bool record_sink_file_uses_sd(void) { return sim_env("PK_SIM_SET_LOGSD", 0) != 0; }

/* BLE 开关（config_ble）。PK_SIM_SET_BLE=0 关，默认开。 */
bool pk_ble_enabled_get(void) { return sim_env("PK_SIM_SET_BLE", 1) != 0; }

/* 设置页的写操作在 settings_page.c（依赖 FreeRTOS，不进模拟器）。
 * 桩成空实现：模拟器只验证版面与命中几何，不改状态。 */
void pk_settings_apply(int row, int v) { (void)row; (void)v; }
