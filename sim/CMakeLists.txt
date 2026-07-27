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

set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/main)

# 可移植的绘制模块。判定标准：只依赖标准库 + 项目内部头文件，
# 对 ESP-IDF 的依赖不超过 esp_attr.h / esp_err.h 这类纯宏头。
#
# 尚未纳入（依赖运行时状态，需要先抽出数据接口）：
#   pfd_hsi_traffic.c  → aircraft_state / own_ship / imu_task / baro / mag_var
#   traffic_page.c     → 同上
#   adsb_list.c        → aircraft_db / i18n
set(FW_DRAW_SRC
    ${FW}/pfd_draw.c
    ${FW}/pfd_font.c
    ${FW}/pfd_font_aa.c
    ${FW}/pfd_attitude.c
    ${FW}/pfd_statusbar.c
    ${FW}/pfd_tape.c
    ${FW}/pfd_speed_tape.c
    ${FW}/pfd_hsi.c
)

add_executable(pkbox_sim main.c ${FW_DRAW_SRC})

target_include_directories(pkbox_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/compat   # 必须排在 FW 前面，桩头优先
    ${FW}
)

target_compile_options(pkbox_sim PRIVATE
    ${SDL2_CFLAGS_LIST}
    -Wall -Wextra
    -Wno-unused-parameter
)

target_link_libraries(pkbox_sim PRIVATE ${SDL2_LIBS_LIST} m)
