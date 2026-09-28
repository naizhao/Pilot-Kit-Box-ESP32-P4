# CC1312R 978 MHz RX-only 固件

本固件是扩展板的 UAT 接收协处理器：CC1312R 用 TI RF Driver/NoRTOS 在
978 MHz 连续接收，验证完整 36-bit sync，使用 partial data entry 与
`CMD_PROP_SET_LEN` 区分 30/48/552 B，然后经版本化 SPI descriptor/分片交给
RP2040。命令图只含 RADIO_DIV_SETUP、FS、RX_ADV 与 RX immediate SET_LEN；
源码门禁禁止任何 TX 命令。

## 构建

```sh
export SIMPLELINK_SDK_ROOT=/Users/samwu/ti/simplelink_cc13xx_cc26xx_sdk_8_33_00_16
make
```

该主机的 SDK 解包目录只有 device/kernel；匹配版本的官方完整归档位于
`/Users/samwu/ti/simplelink_sdk_win.zip`。`prepare_ti_sdk.py` 只把 RF/driver
头文件、两份官方预编译库和官方 NoRTOS linker script 释放到
`build/ti-sdk/`，不向仓库复制整套 SDK。可用 `SDK_ARCHIVE=/absolute/path.zip`
覆盖归档位置。

产物：

- `build/adsb978_cc13.elf`
- `build/adsb978_cc13.bin`
- `build/adsb978_cc13.map`

构建末尾的 `check_ccfg.py` 是硬门禁：ROM bootloader 和 DIO13 高有效 backdoor
必须同时保持开启，否则构建失败，RP2040 代刷通道会被封死。
`check_stack.py` 同时消费编译器的 `-fstack-usage` 报告：1 KiB NoRTOS 栈下，
主 SPI 编码调用链上限为 768 B。大型事务、消息和分片临时区放在单实例 BSS，
不再把两个 512 B 缓冲或完整消息对象压到主栈。

## 实现边界

- `uat_radio_config.c`：SmartRF Studio 2.32 量化参数和 RX-only command objects。
- `uat_rf.c`：TI RF Driver、双 partial entry、36-bit sync/长度/status 校验、
  RF 命令恢复、只读 RSSI 活性探针与状态计数。
- `uat_phy.c`：host 可测的分类、完整帧门和恢复裁决。
- `spi_slave.c`：30/48/552 可变队列与 SPI v1.1 descriptor/chunk 状态机。
- `main.c`：NoRTOS、SSI0 slave mode3、IRQ、RF→SPI 回调和硬件 watchdog。

SSI 为 1 MHz、mode 3 和 uDMA（512 B 事务约 4.1 ms）；主循环轮询 FSS 边沿并
重装 DMA 描述符，RF 回调运行在 TI SWI；共享队列访问用 HwiP 临界区保护；
RF 收包回调在同一临界区内同步更新 SUBG_IRQ，
不会等主循环下一拍才通知 RP。硬件 watchdog 首超时为 2 s，再次超时复位，
覆盖 FSS 被长期拉低或主循环卡死。RF 软件活性每秒用 RX-only `RF_getRssi`
探测命令路径；连续 5 s 无成功探针才重开 RF，因此“空中暂时没有 UAT 包”
不会被当成射频故障。

host 门禁：

```sh
python3 firmware/test/run_host_tests.py -k uat
python3 firmware/scripts/test_cc13_rx_only.py
python3 firmware/scripts/test_cc13_uat_sync.py
```

这只证明参数、协议、队列和构建闭环，不等于宣称实机灵敏度或已收到空中 UAT。
仍需正确连接 978 MHz 天线后，用 RF 状态、RSSI/噪声底和真实 30/48/552 捕获
完成现场验收；不得用 TX 测试替代。

## 烧录

RP2040 ROM SSI0 代刷：

```sh
python3 tools/cc13_flash.py /dev/ttyACM0 firmware/cc1312r/build/adsb978_cc13.bin
```

必须先核对目标端口与芯片身份。当前发布镜像的实际代刷记录见
`docs/internal/2026-09-28-airband-uat-final-acceptance.md`。
