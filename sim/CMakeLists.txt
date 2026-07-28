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
# 尚未纳入：
#   traffic_page.c  → 需要整页的输入状态，等 PFD 这条线定稿后再说
#   adsb_list.c     → aircraft_db / i18n
set(FW_DRAW_SRC
    ${FW}/pfd_draw.c
    ${FW}/pfd_font.c
    ${FW}/pfd_font_aa.c
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
    ${FW}/pfd_hsi_traffic.c
    ${FW}/traffic_geom.c
    ${FW}/mag_var.c
    ${FW}/geo.c
)

# 目标分辨率。默认沿用现役 2.4" 屏，便于验证移植正确性；
# 传 -DPANEL=800x480 即可预览 4.3"/5" 新屏布局。
#     cmake -B build -DPANEL=800x480
set(PANEL "320x240" CACHE STRING "面板分辨率 WxH")
string(REPLACE "x" ";" PANEL_WH ${PANEL})
list(GET PANEL_WH 0 PANEL_W)
list(GET PANEL_WH 1 PANEL_H)
message(STATUS "面板: ${PANEL_W}x${PANEL_H}")

add_executable(pkbox_sim main.c lv_backend.c compat/mock_runtime.c compat/i18n_stub.c ${FW_DRAW_SRC})

target_compile_definitions(pkbox_sim PRIVATE
    PK_DISPLAY_W=${PANEL_W}
    PK_DISPLAY_H=${PANEL_H}
)

target_include_directories(pkbox_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/compat   # 必须排在 FW 前面，桩头优先
    ${FW}
)

target_compile_options(pkbox_sim PRIVATE
    ${SDL2_CFLAGS_LIST}
    -Wall -Wextra
    -Wno-unused-parameter
)

target_link_libraries(pkbox_sim PRIVATE lvgl ${SDL2_LIBS_LIST} m)
