# firmware/rp2040 — RP2040 扩展板固件（adsb1090）

1090 MHz 解调协处理器固件：PIO 沿捕获 + Mode-S 帧成形 + 与 P4 的 UART
链路。产物 `build/adsb1090.uf2`（实测 82432 B ≈ 80.5 KiB，BOOTSEL 拖放
烧录），烧录步骤见 `docs/firmware_update.md`。

## 构建

```bash
./build.sh
```

无参数全量构建；产物落在 `build/adsb1090.uf2`，末尾打印 sha256。要求：

- cmake >= 3.16、宿主 C/C++ 编译器（编译 picotool、pioasm 等宿主工具）；
- `arm-none-eabi-gcc` 在 PATH，**必须含 newlib**（pico-sdk 硬编码
  `--specs=nosys.specs`）：
  - 本机（macOS）：`brew install --cask gcc-arm-embedded`
    （实测 15.3.Rel1；brew formula 版不含 newlib，必编不过）；
  - CI（ubuntu-latest）：`apt-get install gcc-arm-none-eabi
    libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib`
    （runner 镜像自带 cmake/python3）。
    注意版本偏移：apt 装的是 13.x，本机实测构建用的是 15.3.Rel1——
    首个 tag 构建后核对一次 CI 日志（UF2 大小/sha256 与本地产物对拍）。
- 首次构建需联网（tarball 均带 URL_HASH 完整性校验，取回后缓存在
  `build/` 下）。

## 依赖 pin（全部钉死在 CMakeLists.txt，唯一事实源）

| 依赖 | pin | 原因 |
|---|---|---|
| pico-sdk | 2.1.1 release tarball + URL_HASH | 版本可复现 |
| tinyusb | commit `86ad6e56`（pico-sdk 2.1.1 官方 submodule pin）tarball + URL_HASH | SDK 的 **tarball 不含 submodule**，SDK 2.1.1 亦不会自取；必须显式取回并钉在官方 submodule SHA 上 |
| picotool | 2.1.1 release tarball + URL_HASH | SDK 找不到已装 picotool 时会从 git **develop 分支**自取（GIT_TAG 不可 pin）；提前自建同版本并注册同名目标，把 SDK 的自取短路掉 |

注意：

- 本工程**不读** `PICO_SDK_PATH`（build.sh 会主动 unset 残留值）；
- `set_target_properties(... SUFFIX ".elf")` 是 picotool 2.1.1 的兼容
  quirk：它按扩展名识别输入文件，无后缀的默认产物名会被拒。

## 发布链

`build.sh` 同时服务于本地与 CI
（`.github/workflows/release-esp32p4-firmware.yml`）：tag 发布时把
`build/adsb1090.uf2` 拷到 `dist/site/firmware/rp2040/latest/`（刷机页
"latest" 直链）与 `dist/release/pilot-kit-box-rp2040-<version>.uf2`
（Release 资产）。RP2040 镜像与 P4 板型无关，只在完整构建
（`BUILD_IS_COMPLETE == 'true'`）时产出。
