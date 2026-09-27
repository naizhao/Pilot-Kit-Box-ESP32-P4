# CC1312R 预构建镜像（随发布包分发）

## 为什么这里会有一个二进制

CC1312R 的镜像**无法在 CI 上构建**：它要 TI SimpleLink SDK（体积大、需接受
许可、不适合放进公共 CI），而本仓库的 workflow 里既没有 SDK 也没有 CC1312R
构建步骤。所以这个已构建产物直接提交进仓库，由发布流程原样带走。

同样的做法见 PantsForBirds/adsbee：他们也把 CC1312 的 bin 提交进仓库，供
RP2040 侧编译时链接进去。本板将来做「镜像编进 RP2040 固件」时也需要它在版本库里。

## 实板验证范围

2026-09-27 在 V4.10 实板上通过 RP2040 的 ROM SSI 代理通道完成了目标身份读取、
整片擦除、360448 字节写入、设备端 CRC32 校验与复位；`GET_CHIP_ID` 为
`0x30828000`，最终输出 `BSL-DONE 360448 字节`。写入本目录镜像后，RP2040 与
CC1312R 的应用 SPI（mode 3、1 MHz）持续收到合法 HELLO，连续诊断快照为
`state=1 peer=0.1 legal=110→114 magic=0 crc=0`。

以上只证明**代理升级通路、镜像启动和应用 SPI 控制链路**。当前
`firmware/cc1312r/main.c` 中的 RF Core / 978 MHz PHY 仍是骨架，因此下列功能
仍未实现、不得作为发布能力宣传：

- 978 MHz 射频接收；
- UAT 物理层解调；
- 真实 UAT 帧接收与解码。

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
