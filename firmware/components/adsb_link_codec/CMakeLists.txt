# 纯 C99 共享编解码器：P4 (ESP-IDF) 与 RP2040 (Pico SDK) 编同一份源。
# RP2040 侧不走本清单——firmware/rp2040/CMakeLists.txt 直接列 .c 路径，
# 所以本文件只服务 ESP-IDF。
idf_component_register(
    SRCS "adsb_link_codec.c"
    INCLUDE_DIRS "."
)
