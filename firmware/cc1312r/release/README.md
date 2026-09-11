# CC1312R 预构建镜像（随发布包分发）

## 为什么这里会有一个二进制

CC1312R 的镜像**无法在 CI 上构建**：它要 TI SimpleLink SDK（体积大、需接受
许可、不适合放进公共 CI），而本仓库的 workflow 里既没有 SDK 也没有 CC1312R
构建步骤。所以这个已构建产物直接提交进仓库，由发布流程原样带走。

同样的做法见 PantsForBirds/adsbee：他们也把 CC1312 的 bin 提交进仓库，供
RP2040 侧编译时链接进去。本板将来做「镜像编进 RP2040 固件」时也需要它在版本库里。

## ⚠ 未经实板验证

截至 2026-09-12，这份镜像**从未在真实 CC1312R 上烧录或运行过**。板上那颗是
出厂空片，而空片的 ROM bootloader 是关的（CCFG 全 0xFF ≠ 0xC5），首次烧录必须
用外部 cJTAG 仿真器，仿真器尚未到位。

因此以下全部**未经验证**：
- 镜像能否正常启动
- SPI 从机与 RP2040 的握手
- 978 UAT 接收与解码

已经验证的只有：能构建、CCFG 字段正确（`check_ccfg.py` 构建期门禁）。

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
