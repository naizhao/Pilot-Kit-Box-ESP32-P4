# CC1312R 预构建镜像（随发布包分发）

## 为什么这里会有一个二进制

CC1312R 的镜像**无法在 CI 上构建**：它要 TI SimpleLink SDK（体积大、需接受
许可、不适合放进公共 CI），而本仓库的 workflow 里既没有 SDK 也没有 CC1312R
构建步骤。所以这个已构建产物直接提交进仓库，由发布流程原样带走。

同样的做法见 PantsForBirds/adsbee：他们也把 CC1312 的 bin 提交进仓库，供
RP2040 侧编译时链接进去。本板将来做「镜像编进 RP2040 固件」时也需要它在版本库里。

## 当前镜像范围

本目录镜像于 2026-09-28 用 TI SimpleLink SDK 8.33.00.16 构建，SHA-256：

    50904737ad46fb71d87f67ae75688e0f07852e9e19a215be4d0b7814f4c07307

它包含实际 RX-only TI RF Driver/NoRTOS 命令链、32+4 bit 完整 UAT sync 校验、
30/48/552 B 可变帧、RF→CC SPI→RP→P4 v1.4 传输、IRQ 即时通知，以及终止
错误/连续 probe 失败后的 cancel→termination→close→reopen 恢复。每秒只读
`RF_getRssi()` 活性探针、idle/noise RSSI、原始错误和重初始化计数会沿协议进入
P4 诊断页。host 115/115、CC/RP/P4 构建、RX-only/栈/CCFG 门禁均通过。

本镜像的实际代刷、连续运行计数和目标身份记录见
`docs/internal/2026-09-28-airband-uat-final-acceptance.md`。未连接 978 MHz 天线时
零空中帧不是硬件故障；真实灵敏度和空中 30/48/552 B 捕获仍须接天线现场验收。

## 代刷通路与历史证据

2026-09-27 在 V4.10 实板上通过 RP2040 的 ROM SSI 代理通道完成了目标身份读取、
整片擦除、360448 字节写入、设备端 CRC32 校验与复位；`GET_CHIP_ID` 为
`0x30828000`，最终输出 `BSL-DONE 360448 字节`。写入本目录镜像后，RP2040 与
CC1312R 的应用 SPI（mode 3、1 MHz）持续收到合法 HELLO，连续诊断快照为
`state=1 peer=0.1 legal=110→114 magic=0 crc=0`。

以上是 2026-09-27 的历史通路证据；本目录当前镜像的 2026-09-28 代刷与运行
原始记录以最终验收文档为准，二者不得混用。

实机记录见 `docs/internal/2026-09-27-v410-new-board-bringup.md`。

## CCFG

    BL_CONFIG = 0xC5FF0DC5
    BOOTLOADER_ENABLE=0xC5  BL_ENABLE=0xC5  BL_PIN_NUMBER=13(DIO13)  BL_LEVEL=1(高有效)

这保证烧进去之后可以用 RP2040 经 ROM 串行 bootloader 升级（CDC `U`），
不必再动仿真器。详见 `docs/firmware_update.md`。

## 怎么更新这个文件

    export SIMPLELINK_SDK_ROOT=~/ti/simplelink_cc13xx_cc26xx_sdk_8_33_00_16
    cd firmware/cc1312r && make
    cp build/adsb978_cc13.bin release/
    shasum -a 256 release/adsb978_cc13.bin > release/adsb978_cc13.bin.sha256

`make` 里挂着 `check_ccfg.py` 门禁，CCFG 不对构建就会失败，不会产出错的镜像。
