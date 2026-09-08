# CC1312R 固件（WP-E T4 骨架）

978 MHz UAT 接收协处理器固件。链路层实现已冻结的 SPI 协议 v1.0
（`../../PROTOCOL_RP2040_CC1312R_SPI.md`）；RF/PHY 为结构占位——
**不宣称任何射频性能**（协议 §1 / PLAN §5.2：参数待台架实测）。

## 构建

```sh
export SIMPLELINK_SDK_ROOT=/absolute/path/to/simplelink_cc13xx_cc26xx_sdk_8_33_00_16
make            # 产物 build/adsb978_cc13.bin（ELF: build/adsb978_cc13.elf）
```

环境事实（SDK 版本/下载/ABI/坑）见
`docs/internal/firmware-v3v4/CC1312R-TOOLCHAIN.md`（仓库内，git-ignored）。
`SIMPLELINK_SDK_ROOT` 必须是**绝对路径**（make 不展开 `~`）。

## 结构

- `spi_slave.c/.h` — 协议 slave 状态机（纯逻辑，host 全测：装载链
  §2.3 / 交付沿清源 §1 / WAIT_HELLO 策略 §6.2 / RF 配置存储 §4.6）
- `main.c` — 器件胶水：SSI0 slave mode0 + IOC 映射（pinmap_978.md）
  + IRQ GPIO + 轮询事务循环（预载/收发/交付节奏）
- `firmware/test/test_cc13_slave.c` — 单测 + **master↔slave 全链互通**
  （与 RP2040 侧 spi_master 对跑：握手/事件流/队满重填/异步竞争/
  分裂脑恢复端到端可执行）

## 烧录方式：RP2040 代刷（cJTAG 位脉冲）

**不需要外部调试器、不需要飞线**——用扩展板自己的 USB-C 口即可。

```sh
# 1. 确认 RP2040 固件已包含代刷功能（含 cjtag.c 模块）
# 2. 连接扩展板 USB-C 口到电脑
# 3. 运行：
python3 tools/cc13_flash.py /dev/ttyACM0 firmware/cc1312r/build/adsb978_cc13.bin
```

详见 `docs/firmware_update.md` CC1312R 段（完整的故障排查表）。

## 骨架边界（诚实声明）

- RF Core / 978 PHY：未实现（台架期任务）
- SSI：轮询流式（无 DMA/中断——4 MHz 事务 ≈1 ms，48 MHz CPU 轮询
  余量充分；优化留后续）
- 看门狗：未开启（CC13x2 复位后默认不使能；台架前必须补）
- §6.6 slave 侧 5 s 无事务回 WAIT_HELLO 看护：**未实现**——纯逻辑
  spi_slave 无时间基，需目标端定时器胶水（台架期任务）；主循环
  不退出、master 侧 3 s RECOVERY 兜底单向覆盖
- 烧录：**RP2040 代刷**（cJTAG 位脉冲，见上方"烧录方式"段——不再需要外部 JTAG 调试器）
