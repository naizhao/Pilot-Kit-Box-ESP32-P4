# pkb_adsb1090_rp2040 — 扩展板 RP2040 固件（1090 解调 + P4 链路）。
# pico-sdk 经 URL tarball FetchContent（不跑 git、不要求预装 SDK）；
# tinyusb 同样 tarball 取回：SDK tarball 不含 submodule、SDK 2.1.1 亦不会自取，
# 必须在 SDK 之前显式指定 PICO_TINYUSB_PATH（pico_stdio_usb 依赖）。
# 所用提交即 pico-sdk 2.1.1 官方 submodule pin（该提交无根 CMakeLists.txt，
# MakeAvailable 仅解包取源，不会引入多余目标）。
cmake_minimum_required(VERSION 3.16)

include(FetchContent)
FetchContent_Declare(
    tinyusb
    URL https://github.com/hathach/tinyusb/archive/86ad6e56c1700e85f1c5678607a762cfe3aa2f47.tar.gz
)
FetchContent_MakeAvailable(tinyusb)
set(PICO_TINYUSB_PATH ${tinyusb_SOURCE_DIR})
FetchContent_Declare(
    pico_sdk
    URL https://github.com/raspberrypi/pico-sdk/archive/refs/tags/2.1.1.tar.gz
)
FetchContent_MakeAvailable(pico_sdk)

project(pkb_adsb1090_rp2040 C CXX ASM)
set(CMAKE_C_STANDARD 11)

add_executable(adsb1090
    adsb1090.c
    # ── 临时状态（Task 7 脚手架）：其余源文件尚未创建，暂注释，Task 8 起逐个放回 ──
    rf_safety.c
    modes_edge.c
    # edge_cap.c
    # edge_cap.pio
    # selftest_gen.c
    # threshold_ctl.c
    # p4_link.c
    # ../components/adsb_link_codec/adsb_link_codec.c   # 文件已存在，随链路任务一起放回
)
target_include_directories(adsb1090 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../components/adsb_link_codec
)
# SDK 2.1.1 会取 picotool develop 分支，其按扩展名识别输入文件；
# 无后缀的默认产物名会被拒（"no recognized file type"），显式用 .elf 后缀。
set_target_properties(adsb1090 PROPERTIES SUFFIX ".elf")
target_link_libraries(adsb1090 PRIVATE
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_pwm
    hardware_adc
    pico_stdio_usb
)
pico_enable_stdio_usb(adsb1090 1)
pico_enable_stdio_uart(adsb1090 0)
pico_add_extra_outputs(adsb1090)   # 产出 .uf2（BOOTSEL 拖拽烧录）
