# ESP32-P4 固件发布与网页刷写

本文档说明如何发布 Pilot Kit Box 的 ESP32-P4 主固件，以及普通用户如何通过网页完成更新。

## 适用范围

- 适用于已经出厂预刷 ESP32-C6 hosted slave 固件的设备。
- 只更新 ESP32-P4 主固件。
- 不更新 ESP32-C6 协处理器固件。
- 不要求用户安装 ESP-IDF、Python、CMake 或 Ninja。

## 发布流程

1. 确认 `firmware/` 可以本地构建通过。
2. 发正式版本前，把 `firmware/version.txt` 更新为同一个产品版本号，例如 `v0.4.0`。
   本地构建会把这个值写入 ESP-IDF 的 `PROJECT_VER`，启动页和 ABOUT 页都会显示它。
3. 创建并推送同名 tag，例如：

   ```bash
   git tag v0.4.0
   git push origin v0.4.0
   ```

4. GitHub Actions 会运行 `.github/workflows/release-esp32p4-firmware.yml`。
5. CI 使用 tag / 手动输入的 release 版本覆盖 `PROJECT_VER` 构建 `firmware/`，然后生成 release assets。
6. CI 会创建或更新 GitHub Release，并部署 GitHub Pages 刷写页面。

首次使用 GitHub Pages 前，在仓库设置里确认：

- Settings → Pages → Build and deployment → Source 选择 `GitHub Actions`
- Actions 权限允许 workflow 写入 Releases 和 Pages

## 版本号来源

- 默认产品版本写在 `firmware/version.txt`，当前为 `v0.4.0`。
- ESP-IDF 会优先读取 `firmware/version.txt` 作为 `PROJECT_VER`，所以本地构建不再退回到 commit id。
- CI 打包时会显式传入 `-DPROJECT_VER="$RELEASE_VERSION"`，让固件内嵌版本、manifest 版本和产物文件名保持一致。
- 如果使用板型前缀 tag（例如 `esp32p4-v0.4.0`），发布脚本会把它归一为产品版本 `v0.4.0`，避免产物名里重复出现 `esp32p4`。

## 产物命名

所有 ESP32-P4 产物文件名都包含 `esp32p4`，为未来其他板型预留空间。

以 `v1.2.3` 为例：

| 文件 | 用途 |
|---|---|
| `pilot-kit-box-esp32p4-v1.2.3-factory.bin` | 网页刷写使用的 merged bin，写入 offset `0x0` |
| `pilot-kit-box-esp32p4-v1.2.3-bootloader.bin` | 维护者排障用，写入 offset `0x2000` |
| `pilot-kit-box-esp32p4-v1.2.3-partition-table.bin` | 维护者排障用，写入 offset `0x8000` |
| `pilot-kit-box-esp32p4-v1.2.3-app.bin` | 维护者排障用，写入 offset `0x10000` |
| `manifest-esp32p4.json` | ESP Web Tools 刷写清单 |
| `SHA256SUMS-esp32p4.txt` | 校验和 |
| `pilot-kit-box-esp32p4-v1.2.3.zip` | 面向下载的完整包 |

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
