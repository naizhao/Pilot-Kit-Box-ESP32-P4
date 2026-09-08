# 纯 C99 共享编解码器：RP2040（SPI master）与 CC1312R（SPI slave）编同一份源。
# RP2040 侧不走本清单——firmware/rp2040/CMakeLists.txt 直接列 .c 路径，
# 所以本文件只服务 ESP-IDF。规范：
# firmware/PROTOCOL_RP2040_CC1312R_SPI.md v1.0。
#
# 注意：本组件当前在 P4 主树**没有任何调用点**，但 EXTRA_COMPONENT_DIRS 下的
# 组件不论是否被 REQUIRES 都会编译并归档（build_v4 里存在 librp_cc13xx_codec.a
# 可证）；真正的惰性发生在链接期——无调用点则无符号引用，gc-sections 把代码
# 裁出最终 elf（nm 零命中），主固件零体积/零运行时成本。
idf_component_register(
    SRCS "rp_cc13xx_codec.c"
    INCLUDE_DIRS "."
    REQUIRES adsb_link_codec   # §3.2：CRC-16/CCITT-FALSE 复用其唯一权威实现
)
