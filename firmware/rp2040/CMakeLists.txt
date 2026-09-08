# pkb_adsb1090_rp2040 — 扩展板 RP2040 固件（1090 解调 + P4 链路）。
# pico-sdk 经 URL tarball FetchContent（不跑 git、不要求预装 SDK）；
# tinyusb 同样 tarball 取回：SDK tarball 不含 submodule、SDK 2.1.1 亦不会自取，
# 必须在 SDK 之前显式指定 PICO_TINYUSB_PATH（pico_stdio_usb 依赖）。
# 所用提交即 pico-sdk 2.1.1 官方 submodule pin（该提交无根 CMakeLists.txt，
# MakeAvailable 仅解包取源，不会引入多余目标）。
# picotool 也在此声明 tarball pin（audit P1-7）：SDK 2.1.1 找不到已装
# picotool 时会从 git develop 分支自取（tools/Findpicotool.cmake，GIT_TAG
# develop 不可 pin）。坑在于它调的是带显式参数的 FetchContent_Populate，
# 会**无视**这里 Declared 的细节——所以光声明拦不住，真正拦截靠 project()
# 之后手工构建同一 pin 源并提前注册名为 picotool 的目标，令
# pico_init_picotool() 的 `NOT TARGET picotool` 守卫短路（见下方块）。
# pin 版本 2.1.1 = SDK 2.1.1 的 picotool_VERSION_REQUIRED（tools/CMakeLists.txt:104）。
cmake_minimum_required(VERSION 3.16)

include(FetchContent)
FetchContent_Declare(
    tinyusb
    URL https://github.com/hathach/tinyusb/archive/86ad6e56c1700e85f1c5678607a762cfe3aa2f47.tar.gz
    URL_HASH SHA256=9f43fd2a745e2543a42ac79b99774c24155e6ef1e0d2ae3cb16ed4ffacf5c517
)
FetchContent_MakeAvailable(tinyusb)
set(PICO_TINYUSB_PATH ${tinyusb_SOURCE_DIR})
FetchContent_Declare(
    picotool
    URL https://github.com/raspberrypi/picotool/archive/refs/tags/2.1.1.tar.gz
    URL_HASH SHA256=19200c6dc4be5acd6fb53de3d7f35c826af596c18879d56f214b795300100260
)
FetchContent_Declare(
    pico_sdk
    URL https://github.com/raspberrypi/pico-sdk/archive/refs/tags/2.1.1.tar.gz
    URL_HASH SHA256=179c5531e75ac7f82d0679e70b6e8881f871738dc0ef17baf3b4ff4679291c41
)
FetchContent_MakeAvailable(pico_sdk)

project(pkb_adsb1090_rp2040 C CXX ASM)
set(CMAKE_C_STANDARD 11)

# ── picotool：按上面的 pin 自建（audit P1-7）────────────────────────────
# 复刻 SDK Findpicotool.cmake 的 ExternalProject 布局与参数，只把源换成
# 已 pin 的 tarball（Populate 只解包不配置，不触碰 PICO_SDK_PATH）。
# 构建期以嵌套 cmake 走宿主工具链（不经本工程的 arm 交叉工具链），
# PICOTOOL_NO_LIBUSB=1 跳过 xip_ram_perms/flash_id 等需要 Pico 工具链的
# 子工程（SDK 自取路径同款参数）。装平到 <build>/picotool-install，
# IMPORTED 目标 `picotool` 指向产物：SDK 的 POST_BUILD `COMMAND picotool
# uf2 convert` 会解析到它，add_dependencies 保证先建好再后处理。
FetchContent_Populate(picotool QUIET
    URL https://github.com/raspberrypi/picotool/archive/refs/tags/2.1.1.tar.gz
    URL_HASH SHA256=19200c6dc4be5acd6fb53de3d7f35c826af596c18879d56f214b795300100260
)
set(PICOTOOL_INSTALL_DIR ${CMAKE_BINARY_DIR}/picotool-install)
include(ExternalProject)
ExternalProject_Add(picotoolBuild
    PREFIX picotool
    SOURCE_DIR ${picotool_SOURCE_DIR}
    BINARY_DIR ${picotool_BINARY_DIR}
    INSTALL_DIR ${PICOTOOL_INSTALL_DIR}
    CMAKE_ARGS
        "--no-warn-unused-cli"
        "-DCMAKE_MAKE_PROGRAM:FILEPATH=${CMAKE_MAKE_PROGRAM}"
        "-DPICO_SDK_PATH:FILEPATH=${pico_sdk_SOURCE_DIR}"
        "-DPICOTOOL_NO_LIBUSB=1"
        "-DPICOTOOL_FLAT_INSTALL=1"
        "-DCMAKE_INSTALL_PREFIX=${PICOTOOL_INSTALL_DIR}"
    BUILD_ALWAYS 1
    EXCLUDE_FROM_ALL TRUE
    TEST_AFTER_INSTALL TRUE
    TEST_COMMAND ${PICOTOOL_INSTALL_DIR}/picotool/picotool version 2.1.1
)
add_executable(picotool IMPORTED GLOBAL)
set_property(TARGET picotool PROPERTY IMPORTED_LOCATION
        ${PICOTOOL_INSTALL_DIR}/picotool/picotool)
add_dependencies(picotool picotoolBuild)

add_executable(adsb1090
    adsb1090.c
    rf_safety.c
    modes_edge.c
    edge_cap.c
    edge_cap_queue.c
    edge_cap.pio
    selftest_gen.c
    selftest_gen.pio
    threshold_ctl.c
    p4_link.c
    spi_master.c
    ../components/adsb_link_codec/adsb_link_codec.c   # 共享 codec：与 P4 侧同一份源
    ../components/rp_cc13xx_codec/rp_cc13xx_codec.c   # CC1312R 链路共享 codec（协议 v1.0）；
                                                      # WP-P0b Task 3 仅无害编译进产物，
                                                      # WP-E 才接数据链路（无调用点）
)
target_include_directories(adsb1090 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}   # pioasm 生成的 edge_cap.pio.h 落在 build 根
    ${CMAKE_CURRENT_LIST_DIR}/../components/adsb_link_codec
    ${CMAKE_CURRENT_LIST_DIR}/../components/rp_cc13xx_codec
)
# pioasm 工具链由 SDK tools/CMakeLists.txt 按需 FetchContent；
# 必须显式声明才会生成头（.pio 仅列进 add_executable 不会触发）。
pico_generate_pio_header(adsb1090 ${CMAKE_CURRENT_LIST_DIR}/edge_cap.pio)
pico_generate_pio_header(adsb1090 ${CMAKE_CURRENT_LIST_DIR}/selftest_gen.pio)
# 栈用量审计（WP-E-2 P1 门禁）：-fstack-usage 生成 .su 文件，
# build.sh 的门禁脚本据此把 UAT 链路各函数栈帧钉在预算内
# （core0 栈 2048 B，最深链 main→spim_poll→digest→uat 回调≈800 B
# + 库/中断余量）。改动这些函数后构建即复核。
target_compile_options(adsb1090 PRIVATE -fstack-usage)
# picotool（含 2.1.1 release）按扩展名识别输入文件；
# 无后缀的默认产物名会被拒（"no recognized file type"），显式用 .elf 后缀。
set_target_properties(adsb1090 PROPERTIES SUFFIX ".elf")
target_link_libraries(adsb1090 PRIVATE
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_pwm
    hardware_adc
    hardware_spi
    pico_stdio_usb
)
pico_enable_stdio_usb(adsb1090 1)
pico_enable_stdio_uart(adsb1090 0)
pico_add_extra_outputs(adsb1090)   # 产出 .uf2（BOOTSEL 拖拽烧录）
