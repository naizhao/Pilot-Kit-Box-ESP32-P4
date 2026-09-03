# ESP32-P4 固件发布与网页刷写

英文版：[`firmware_update.md`](firmware_update.md)

本文档说明如何发布 Pilot Kit Box 的 ESP32-P4 主固件，以及普通用户如何通过网页完成更新。

## 适用范围

- 适用于已经出厂预刷 ESP32-C6 hosted slave 固件的设备。
- 只更新 ESP32-P4 主固件。
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

- 默认产品版本写在 `firmware/version.txt`，当前为 `v0.8.0`。
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

## 限制

- iPhone / iPad Safari 不支持 Web Serial，不能直接刷写。
- Android 浏览器支持情况不稳定，不作为主要路径。
- 这条路径不处理 ESP32-C6 首次烧录；C6 必须在出厂时预刷好。
- 如果未来增加其他板型，需要新增对应 workflow、manifest 路径和文件名前缀，不要复用 `esp32p4` 产物名。
