#
# Pilot Kit Box — PFD PC 模拟器
#
# 直接编译 firmware/main 里的绘制模块（不复制、不 fork），配 compat/ 下
# 的两个 ESP-IDF 头文件桩，让同一份源码在 macOS/Linux 上跑起来。
# 固件改了绘制代码，这边重新编译就同步，不存在两份实现走偏的问题。
#
cmake_minimum_required(VERSION 3.16)
project(pkbox_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# SDL2 探测。
#
# 优先用 sdl2-config 而不是 pkg-config：本机装了 ServBay，它的
# /Applications/ServBay/bin/pkg-config 会抢在 homebrew 前面被找到，
# 而那份 pkg-config 的搜索路径里没有 homebrew 的 sdl2.pc，直接报
# "No package 'sdl2' found"。sdl2-config 由 SDL 自带，不受影响。
find_program(SDL2_CONFIG sdl2-config
    HINTS /opt/homebrew/bin /usr/local/bin /usr/bin)

if(SDL2_CONFIG)
    execute_process(COMMAND ${SDL2_CONFIG} --cflags
        OUTPUT_VARIABLE SDL2_CFLAGS_RAW OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${SDL2_CONFIG} --libs
        OUTPUT_VARIABLE SDL2_LIBS_RAW   OUTPUT_STRIP_TRAILING_WHITESPACE)
    separate_arguments(SDL2_CFLAGS_LIST NATIVE_COMMAND "${SDL2_CFLAGS_RAW}")
    separate_arguments(SDL2_LIBS_LIST   NATIVE_COMMAND "${SDL2_LIBS_RAW}")
    message(STATUS "SDL2 via sdl2-config: ${SDL2_LIBS_RAW}")
else()
    # 回退：标准 pkg-config 流程（Linux / CI 上通常可用）
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    set(SDL2_CFLAGS_LIST ${SDL2_CFLAGS_OTHER})
    set(SDL2_LIBS_LIST   ${SDL2_LIBRARIES})
    message(STATUS "SDL2 via pkg-config")
endif()

# ── LVGL ────────────────────────────────────────────────────────────
#
# 固定到 v9.5.0 而不是跟 master：布局评审要能复现，LVGL 的绘制细节在小版本
# 之间也会变。固件侧将来经 IDF 组件管理器引入同一版本。
#
# 关键配置在 sim/lv_conf.h，其中最要紧的一条是
# LV_DRAW_SW_SUPPORT_RGB565_SWAPPED —— 本项目的 pk_rgb565() 在源头就把颜色
# 转成了大端（ST7789 的 SPI 线序），而 LVGL 默认按主机序解释 RGB565。v9.5
# 原生支持 swapped 格式并带专门的混合路径，因此不必去动 pk_rgb565 的约定，
# 也就避免了一次会波及现役固件的字节序改动。
include(FetchContent)
set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h CACHE FILEPATH "" FORCE)
# 不编 examples / demos：省编译时间，也免掉「找不到 lv_conf.h」以及链接期
# 「ignoring duplicate libraries」两类噪音——LVGL 把 lvgl_demos / lvgl_examples
# 以 PUBLIC 方式挂在 lvgl target 上，只要它们存在就会被一并链进来。
# 选项名以 v9.5 的 env_support/cmake/os_desktop.cmake 为准。
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS    OFF CACHE BOOL "" FORCE)
# ThorVG 是 LVGL 的矢量图形引擎，本项目用不到：PFD 全部自绘，控件只用基本
# 图形。它是一大坨 C++，编译慢且会刷出上千条 pragma 警告。
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG        v9.5.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(lvgl)

set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main)

# 可移植的绘制模块。判定标准：只依赖标准库 + 项目内部头文件，
# 对 ESP-IDF 的依赖不超过 esp_attr.h / esp_err.h 这类纯宏头。
#
# pfd_hsi_traffic.c 依赖运行时状态（aircraft_state / own_ship / imu_task /
# baro），这些由 compat/mock_runtime.c 桩掉 —— 桩的是**数据接口**，渲染代码
# 本身原样编译，不存在两套实现走偏的问题。mag_var.c / traffic_geom.c / geo.c
# 是纯计算，直接编译。
#
set(FW_DRAW_SRC
    ${FW}/pfd_draw.c
    ${FW}/pfd_font.c
    ${FW}/pfd_aa_font.c
    ${FW}/pfd_icon_font.c
    ${FW}/pfd_aa_text.c
    ${FW}/pfd_attitude.c
    ${FW}/pfd_statusbar.c
    ${FW}/pfd_statusbar_icons.c
    ${FW}/pfd_tape.c
    ${FW}/pfd_speed_tape.c
    ${FW}/pfd_hsi.c
    ${FW}/pfd_infobox.c
    ${FW}/lv_font_zh.c
    ${FW}/i18n_catalog.c
    ${FW}/pk_ui_nav.c
    # 全屏导航网格（点 FAB 打开的主菜单，取代了原来的横向 dock）。版面、
    # 分页、置灰占位、亮度快调都要在模拟器上评审，所以渲染与命中判定原样
    # 编进来；host 上跑不起来的只有「切到哪一页 / 亮度档位」这两个数据接口，
    # 由 compat/page_stub.c 桩掉。图标位图是纯数据表，直接编。
    ${FW}/nav_grid_page.c
    ${FW}/nav_icon_font.c
    ${FW}/pfd_hsi_traffic.c
    # 演示模式的合成数据。它同时是模拟器的 mock 源——两边共用一份，模拟器上
    # 压出来的极值真机演示时才压得到同一批（见 demo_data.h 的文件头）。
    ${FW}/demo_data.c
    ${FW}/demo_track.c
    ${FW}/demo_track_data.c
    ${FW}/traffic_geom.c
    ${FW}/mag_var.c
    ${FW}/geo.c
    # 整页视图（关于/设置/诊断…）。它们只把运行时状态排版成像素，数据来源
    # 由 compat/page_stub.c 桩掉，排版逻辑原样编译。
    ${FW}/about_page.c
    # 磁力计校准向导。它是**唯一一个用户自己点不进去**的页面（acc=0 满 10 s
    # 才自动弹），真机上想看一眼它长什么样得先把 IMU 弄失准——所以更要能在
    # 模拟器里截。渲染只依赖 pk_ui_cal_wizard_last_accuracy()（page_stub.c
    # 里由 PK_SIM_CAL_ACC 摆），命中判定只依赖 pk_ui_cal_wizard_dismiss()。
    ${FW}/cal_wizard.c
    ${FW}/boot_splash.c
    ${FW}/traffic_page.c
    ${FW}/adsb_list.c
    ${FW}/diag_page.c
    ${FW}/settings_draw.c
    # 键盘编辑器同样是纯排版 + 命中判定，唯一的外部依赖是 config_devname 的
    # 长度常量（头文件里的宏）与 pk_ui_nav 的 FAB 显隐，两者都已在这里。
    ${FW}/keyboard_page.c
    # 航空数据搜索页。版面、分桶排序、空态文案都要在模拟器上评审，所以原样
    # 编进来；host 上跑不起来的只有后台任务与 NVS 历史，那两段在 PK_SIM_BUILD
    # 下退化成"同步查 + 内存里的历史"（见 search_page.c 尾部）。数据源同样
    # 由 compat/pk_aero_layer_sim.c 桩掉。
    ${FW}/search_page.c
    # 机场详情页。跑道/频率的版面、业务序排序、缺字段的降级都要在模拟器上
    # 评审，所以原样编进来；它没有后台任务也没有 NVS，唯一的外部依赖是
    # pk_aero_db_*，同样由 compat/pk_aero_layer_sim.c 桩掉。
    ${FW}/apt_detail_page.c
    # 航司 / 国籍两张纯数据表：零 IDF 依赖，直接编进来比写桩真实
    ${FW}/airline_codes.c
    ${FW}/icao_country.c
    # 呼号规范化 / 显示回退：纯逻辑零依赖，三个页面都要用（见 pk_callsign.h）
    ${FW}/pk_callsign.c
    # 2026-08-03 删除：text.c / text_font_cjk*.c。见 firmware/main/CMakeLists.txt
    # 同一处的说明——最后一个调用者（校准向导）已改走 pfd_aa_text。
    # SD 离线地图页。map_page.c 本身只依赖 pk_tile_loader.h 这个纯函数式
    # 接口（route/try_blit/request/bump_view），不直接碰 pk_map_store /
    # pk_pmtiles——那两个是 pk_tile_loader 的实现细节。真机的
    # pk_tile_loader.c 依赖 FreeRTOS 任务+队列，host 没有，换成
    # compat/pk_tile_loader_sim.c（同步实现，见该文件文件头）。
    # pk_map_store.c / pk_pmtiles.c / third_party/pk_tinfl.c 三个是纯 I/O +
    # 计算，无 FreeRTOS 依赖，host 单测已经在编（firmware/test/），直接编
    # 进来，与固件是同一份实现。
    ${FW}/map_page.c
    # 地图页的航空叠加层（机场/导航台/FIX）。它的 LOD 过滤、快照结构、配色与
    # 标签避让都要在模拟器上评审，所以原样编进来；host 上跑不起来的只有数据源
    # （SD + 加密 bin + FreeRTOS 加载任务），由 compat/pk_aero_layer_sim.c 把
    # pk_aero_db_* 换成内存常量表。-DPK_AERO_LAYER_SIM_IMPL 见下方 defs。
    ${FW}/pk_aero_layer.c
    ${FW}/pk_map_store.c
    ${FW}/pk_pmtiles.c
    ${FW}/third_party/pk_tinfl.c
)

# 当前 4.3" 一体板的逻辑横屏分辨率。
#
# 2026-07-30：**小屏（320×240）兼容预览已移除**，PANEL 只保留 800x480 一个有效
# 取值。原因是那条路径的成本远大于收益——它需要一整套并行的字体与版面：
# 12×16 的 cockpit 字形（pfd_font_aa.c 31.5 KB + gen_pfd_cockpit_font.py 及其
# 测试）、pfd_tape / pfd_speed_tape / pfd_hsi 里各一份 #else 版面常量，而设备早已
# 全面转到 800×480，那些分支在真机上一行都不会执行。留着的实际效果是：每改一次
# 版面要同时维护两套数、生成脚本要同时喂两套字库，却没有任何人在看小屏的画面。
# 产品负责人 2026-07-30 明确「删」。
#
# 变量保留只是为了让这条命令行不用改。**它其实早就不管用了**：
# firmware/main/display.h 无条件 #define 了 PK_DISPLAY_W/H，包含它的翻译单元
# 会把这里 -D 进来的值覆盖掉（值不同还会得到一条 -Wmacro-redefined）。真要换屏
# 得改 display.h。写在这里是为了下一个人别再花半小时才发现 -DPANEL=800x360
# 截出来的图仍然是 800×480。
set(PANEL "800x480" CACHE STRING "面板分辨率 WxH（仅 800x480；实际由 display.h 钉死）")
string(REPLACE "x" ";" PANEL_WH ${PANEL})
list(GET PANEL_WH 0 PANEL_W)
list(GET PANEL_WH 1 PANEL_H)
message(STATUS "面板: ${PANEL_W}x${PANEL_H}")

add_executable(pkbox_sim main.c lv_backend.c compat/mock_runtime.c compat/i18n_stub.c compat/page_stub.c
    compat/pk_tile_loader_sim.c
    compat/pk_aero_layer_sim.c
    ${FW_DRAW_SRC})

target_compile_definitions(pkbox_sim PRIVATE
    PK_DISPLAY_W=${PANEL_W}
    PK_DISPLAY_H=${PANEL_H}
    PK_SIM_BUILD=1        # 让页面里少数「只为截图」的钩子编进来（如列表排序覆盖）
    # 航空叠加层：不开这个宏，pk_aero_layer.h 在 PK_SIM_BUILD 下是一组 inline
    # 空实现（模拟器看不到这一层）。开了就走真实实现 + 同步查询，数据由
    # compat/pk_aero_layer_sim.c 提供，运行期还要 PK_SIM_AERO=1 才真出数据。
    PK_AERO_LAYER_SIM_IMPL=1
)

target_include_directories(pkbox_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/compat   # 必须排在 FW 前面，桩头优先
    ${FW}
    # compat/pk_tile_loader_sim.c 要 #include "libs/lodepng/lodepng.h"——
    # 与固件那份完全同一行 include（pk_tile_loader.c 里也是这么写的）。
    # ESP-IDF 那边的 lvgl 组件把 src/ 也加进了 INCLUDE_DIRS（见
    # env_support/cmake/esp.cmake），FetchContent 拉下来的 lvgl target
    # 默认只公开仓库根目录，这里补上 src/ 让两边路径一致。
    ${lvgl_SOURCE_DIR}/src
)

target_compile_options(pkbox_sim PRIVATE
    ${SDL2_CFLAGS_LIST}
    -Wall -Wextra
    -Wno-unused-parameter
)

target_link_libraries(pkbox_sim PRIVATE lvgl ${SDL2_LIBS_LIST} m)
