# CC1312R UAT RX 参数证据

参数来自本机 TI SimpleLink SDK 8.33.00.16 的原厂 RF Driver/NoRTOS 头与库，以及
SmartRF Studio 2.32 对 CC1312R Rev E 的 978 MHz 配置导出值。这里只记录物理
参数与量化结果；实现未复制第三方接收器源码。

| 项 | 请求/协议值 | CC1312R 配置值 |
|---|---:|---:|
| 载频 | 978 MHz | `CMD_FS.frequency=978` |
| 调制 | GFSK | `modType=1` |
| deviation | 312.5 kHz | word `0x4E2`，250 Hz/step，精确 |
| symbol rate | 1.041667 Mbaud | preScale `0x0F`，rateWord `0xA6BFB`，实际 1.042179870 Mbaud |
| rate 量化误差 | — | +512.87 baud，约 +492 ppm |
| RX bandwidth | — | code `0x64`，2.1851 MHz |
| RF Core sync 高 32 bit | ADS-B | `0xEACDDA4E` |
| RF Core sync 高 32 bit | uplink | `0x153225B1` |
| 完整 sync | ADS-B/uplink | `0xEACDDA4E2` / `0x153225B1D`（header 捕获的低 4 bit 分别 2/D） |
| RF 前端 | 原理图差分、外部 bias | `frontEndMode=0`, `biasMode=1` |

RF Core 只匹配完整同步字的高 32 bit。`RX_ADV.hdrConf.numHdrBits=4` 把低 4 bit 存入
entry，软件必须再次验证；仅凭高 32 bit 不得上送。下行在首 payload 字节取得 MDB
type：0=30 B，其余=48 B；uplink=552 B。partial entry 的 16 B 中断时调用
`CMD_PROP_SET_LEN`，完成后再用 entry 自带长度、RSSI、RAT timestamp、status 和
syncWordId 复核，任何不一致都丢弃并计数。

RF RAT 为 4 MHz、模 2^32 回绕。线上 `cc_time` 保留原始 tick，不伪装成 µs。
运行时软件活性计时来自 NoRTOS `ClockP`，与线上 RAT 时间域分开。每秒只执行
RX-only `RF_getRssi` 探针；成功即证明 RF Core 命令路径仍响应（即使频谱静默），
连续 5 秒失败才重开 RF，并累加 `rssi_probe_errors/watchdog_reinits`。
