# ESP32-P4 固件发布与网页刷写

英文版：[`firmware_update.md`](firmware_update.md)

本文档说明如何发布 Pilot Kit Box 的 ESP32-P4 固件，以及普通用户如何通过网页更新 ESP32-P4 主固件与 RP2040 1090 接收机固件（UF2）。

## 适用范围

- 适用于已经出厂预刷 ESP32-C6 hosted slave 固件的设备。
- 更新 ESP32-P4 主固件与 RP2040 1090 接收机固件（UF2）。
- 不更新 ESP32-C6 协处理器固件。
- 不要求用户安装 ESP-IDF、Python、CMake 或 Ninja。

## 发布流程

1. 确认 `firmware/` 可以本地构建通过。
2. 发正式版本前，把 `firmware/version.txt` 更新为同一个产品版本号，例如 `v0.8.0`。
   普通本地构建会显示 `v0.8.0-<git短哈希>`；发布 CI 显式传入
   `PROJECT_VER=v0.8.0`，启动页、ABOUT 页和发布产物统一显示正式版本号。
3. 创建并推送同名 tag，例如：

   ```bash
   git tag v0.8.0
   git push origin v0.8.0
   ```

4. GitHub Actions 会运行 `.github/workflows/release-esp32p4-firmware.yml`。
5. CI 使用 tag / 手动输入的 release 版本覆盖 `PROJECT_VER` 构建 `firmware/`，然后生成 release assets。
6. CI 会创建或更新 GitHub Release，并部署 GitHub Pages 刷写页面。

首次使用 GitHub Pages 前，在仓库设置里确认：

- Settings → Pages → Build and deployment → Source 选择 `GitHub Actions`
- Actions 权限允许 workflow 写入 Releases 和 Pages

## 版本号来源

- 默认产品版本写在 `firmware/version.txt`，当前为 `v0.9.0`。
- 普通本地构建以 `firmware/version.txt` 为基础并追加当前 git 短哈希，方便定位具体构建。
- CI 打包时会显式传入 `-DPROJECT_VER="$RELEASE_VERSION"`，让固件内嵌版本、manifest 版本和产物文件名保持一致。
- 如果使用板型前缀 tag（例如 `esp32p4-v0.8.0`），发布脚本会把它归一为产品版本 `v0.8.0`，避免产物名里重复出现 `esp32p4`。

## 产物命名

所有 ESP32-P4 产物文件名都包含 `esp32p4`，为未来其他板型预留空间。

### 一次发布产出两套：v3 与 v4

v3 与 v4 两个板系把 IMU 贴在相差 90° 的角度上，固件按构建时选定的板系换算姿态，
所以**每次发布都同时构建两套**，产物文件名里带 `v3` / `v4`。

⚠️ **刷错板型不会报错**：地平仪照样有姿态、照样跟着动，只是横滚整体偏 90°，
桌上不容易看出来。所以刷机页面不提供"默认按钮"，必须自己选板型；开机串口日志
`imu: board profile v3|v4` 那一行可以事后确认。

以 `v1.2.3` 的 v4 那一套为例（v3 把 `v4` 换成 `v3`）：

| 文件 | 用途 |
|---|---|
| `pilot-kit-box-esp32p4-v4-v1.2.3-factory.bin` | 网页刷写使用的 merged bin，写入 offset `0x0` |
| `pilot-kit-box-esp32p4-v4-v1.2.3-bootloader.bin` | 维护者排障用，写入 offset `0x2000` |
| `pilot-kit-box-esp32p4-v4-v1.2.3-partition-table.bin` | 维护者排障用，写入 offset `0x8000` |
| `pilot-kit-box-esp32p4-v4-v1.2.3-app.bin` | 维护者排障用，写入 offset `0x10000` |
| `manifest-esp32p4-v4.json` | ESP Web Tools 刷写清单 |
| `SHA256SUMS-esp32p4-v4.txt` | 校验和 |
| `pilot-kit-box-esp32p4-v4-v1.2.3.zip` | 面向下载的完整包 |

### 只编一版（验证 / 排障）

Actions → **Release ESP32-P4 firmware** → *Run workflow*，`board_profile` 选
`v3` 或 `v4`（默认 `both`）。

单板型运行**只产出 workflow artifact 供下载**，不发 GitHub Release 资产、也不发布
Pages 站点——它的 `dist/site` 里只有一半 manifest，发出去会让另一版用户在刷机页上
点到 404。Pages 部署工作流查不到站点产物时会打一条 notice 安静跳过，不会报红。

**tag 推送恒为两版**，`board_profile` 只对手动触发生效。

Pages 站点上两套各占一个目录：
`firmware/esp32p4/v3/latest/` 与 `firmware/esp32p4/v4/latest/`。
不带板型的旧路径 `firmware/esp32p4/latest/` **已不再生成**——它等于给刷机页留一个
"默认刷某一版"的大按钮，另一版的用户点下去不会有任何提示。

网页刷写采用 merged bin，是因为 ESP Web Tools 对 ESP-IDF v4+ 固件推荐使用合并后的单个二进制，由 `esptool merge-bin` 在 CI 中生成。

## 用户更新流程

1. 用 Chrome 或 Edge 打开 GitHub Pages 刷写页。
2. 用 USB-C 数据线连接 Pilot Kit Box 靠近 BOOT 按键的 Type-C 口。
3. 点击“连接并刷入 ESP32-P4 固件”。
4. 浏览器弹出串口选择框后，选择 Pilot Kit Box 对应的 USB 串口。
5. 如果网页询问是否擦除数据，普通固件升级选择保留数据。
6. 等待刷写完成，设备会自动重启。

如果连接失败：

1. 按住 BOOT。
2. 短按 RESET。
3. 松开 BOOT。
4. 回到网页重新连接。

## RP2040 协处理器固件（扩展板 1090 MHz）

v3/v4 扩展板上的 RP2040 负责 1090 MHz Mode-S 解码，跑自己的固件
（`adsb1090`）。它是独立的芯片：网页刷写页碰不到它（没有 Web Serial），
也不在上面那批 ESP32-P4 产物的范围内。镜像与 P4 板型无关，没有 v3/v4
板型要选。

### 日常更新 —— BOOTSEL 拖放

1. 拿到 UF2：刷机页的 RP2040 下载卡直链最新 `adsb1090.uf2`；每次 Release
   也会附带 `pilot-kit-box-rp2040-<版本>.uf2` 资产。
2. 按住扩展板上的 **BOOTSEL** 键，把 RP2040 的 USB 口插到电脑。RP2040 会
   枚举成一个 U 盘（`RPI-RP2`）。
3. 把 `.uf2` 拖进该盘，RP2040 自动重启进入新固件。

想从源码构建，工具链要求、依赖 pin 与 `./build.sh` 见
[`../firmware/rp2040/README.md`](../firmware/rp2040/README.md)。

### 坏固件恢复

重复同样的 BOOTSEL 流程即可。RP2040 的 USB mass-storage 属于 boot ROM
能力、不依赖已烧固件，固件坏了也锁不死：重新进 BOOTSEL、再拖一次 UF2。
（ROM 能力，设计保证；实板验证待台架。）注意 v4 板已删除 RP2040 的
SWD 测试点，BOOTSEL 是该板上现实的恢复路径；调试器需飞线到芯片的
SWCLK/SWDIO 引脚。

### CC1312R 固件（978 MHz 前端）

**有两条不同的路径**，走哪条取决于这颗芯片有没有被烧录过。搞混会浪费很多时间，
先看清区别：

| 情况 | 路径 | 要外部硬件吗 |
|---|---|---|
| 出厂空片（从没烧过） | cJTAG 仿真器 | **要** —— 仅此一次 |
| 之后的任何一次升级 | RP2040 经 ROM 串行 bootloader 代刷 | 不要 |

#### 为什么空片必须上仿真器

CC13x2 的 ROM 里有串行 bootloader，但它被 CCFG 管着：`BOOTLOADER_ENABLE`
**只有**读到 `0xC5` 才算使能（TRM SWCU185G 表 11-15）。出厂空片的 CCFG 是擦除态
全 `0xFF`，所以 bootloader 是**关**的。它是**现场升级**机制，不是**首次烧录**
机制 —— 这也是每块 TI LaunchPad 都焊着 XDS110 的原因。

`firmware/rp2040/cjtag.c` 里有一套位脉冲 cJTAG 引擎，正是为这种情况写的，但
**实测没有打通**（见内部交接文档）。不要围绕它安排计划。

#### 路径 A —— 首次烧录，用 cJTAG 仿真器

只要支持 **cJTAG（2 线 IEEE 1149.7）** 的仿真器都行。普通 SWD/JTAG 仿真器
**不行**：CC13x2 没有 SWD，所以 DAPLink / ST-Link / 普通 CMSIS-DAP 无论多贵都用不了。

已知可用：TI XDS110（独立款，或任何一块 TI LaunchPad 板载的那颗）、支持 cJTAG
的 SEGGER J-Link（`-if cJTAG`）。

本板这三根网络上**既没有调试座、也没有测试点**，只能直接焊引脚
（下表已按 PCB 焊盘/网络数据核对）：

| 信号 | 焊点 | 备选 |
|---|---|---|
| TMSC | U10 第 24 脚（CC1312R） | U8 第 27 脚（RP2040） |
| TCKC | U10 第 25 脚 | U8 第 28 脚 |
| RESET_N | R47 靠芯片那一侧焊盘（0402，比 QFN 脚好焊太多） | U10 第 35 脚 |
| GND / VTref | 任意接地 / 3V3 | |

接仿真器**之前**先在 RP2040 控制台按 `Z`：它把 GPIO16/17/18 全部置高阻并暂停
SPI master。不做这一步，RP2040 会和仿真器对顶，而且它的恢复逻辑会周期性拉低
RESET_N、正好打断仿真器的会话。恢复靠复位 RP2040。

然后用仿真器自带的工具链（OpenOCD / UniFlash / J-Link）把
`firmware/cc1312r/build/adsb978_cc13.bin` 烧到地址 `0x0`。

#### 路径 B —— 之后的所有升级，经 RP2040

```sh
python3 tools/cc13_flash.py /dev/ttyACM0 firmware/cc1312r/build/adsb978_cc13.bin
```

它走 CC1312R 的 ROM 串行 bootloader（SSI0），经 backdoor 引脚进入
（DIO13 = `SUBG_SYNC`，由 RP2040 的 GPIO15 驱动）。传输期间 1090 解码暂停，
完成后自动恢复。

工具会**拒绝**那些会把 bootloader 或 backdoor 关掉的镜像，见下面「防砖闸门」。
确实要烧用 `--force`。

串口终端里的等价操作：按 `U`，然后送 4 字节小端长度 + 镜像本体。成功打印
`BSL-DONE`，失败打印 `BSL-FAIL: <原因>` 并点名卡在哪一步。

#### 防砖闸门

`tools/cc13_flash.py` 在**动手擦除之前**先解析镜像自带的 CCFG，如果结果不可恢复
就拒绝烧录：

- `BOOTLOADER_ENABLE` 不是 `0xC5` —— 升级通道会永久消失
- `BL_ENABLE` 不是 `0xC5` —— backdoor 没了，以后烧进一个起不来的固件就换不掉
- `BL_PIN_NUMBER` 不是 13，或 `BL_LEVEL` 不是高有效 —— backdoor 挂错脚，
  或者每次上电都会触发
- 镜像**短到根本不含 CCFG** —— 烧录会整片擦除，CCFG 区随之变成 `0xFF`，
  后果与显式关掉一模一样

以上任何一种，恢复手段都是退回路径 A：拆机、焊 QFN 引脚。所以闸门默认拒绝。

本仓库编出来的镜像满足闸门要求，并且有构建期检查
（`firmware/cc1312r/check_ccfg.py`）盯着，防止它静默退回 SDK 默认值 ——
SDK 默认是**禁用** bootloader 的。

#### 排错

| 现象 | 可能原因 |
|---|---|
| `BSL-FAIL: 无应答` | 芯片从没被烧录过（bootloader 是关的），或当前镜像的 CCFG 把 bootloader 关掉了 → 走路径 A |
| `BSL-FAIL: CRC32 不匹配` | 数据全写完了但校验和对不上。TRM 没写 CRC32 用的多项式，我们按标准 IEEE 802.3 实现 —— **先怀疑这个假设**，别急着怀疑 flash |
| `BSL-FAIL: 擦除被拒` | CCFG 里 `BANK_ERASE_DIS` 被置上了 |
| 工具还没碰串口就拒绝了 | 防砖闸门 —— 读它的提示，它会点名具体是哪个字段 |
| 没有串口 | 先刷 RP2040：按住 BOOTSEL 插 USB，把 UF2 拖进去 |

## 限制

- iPhone / iPad Safari 不支持 Web Serial，不能直接刷写。
- Android 浏览器支持情况不稳定，不作为主要路径。
- 这条路径不处理 ESP32-C6 首次烧录；C6 必须在出厂时预刷好。
- 如果未来增加其他板型，需要新增对应 workflow、manifest 路径和文件名前缀，不要复用 `esp32p4` 产物名。
