# Pilot Kit Box — 用户指南

英文版：[`user_guide.md`](user_guide.md)

本文面向使用四按钮 Pilot Kit Box 硬件的日常用户。工程侧 GPIO、接线和 SH-2 细节请看 [`docs/hardware/board_pinout-zh_CN.md`](hardware/board_pinout-zh_CN.md)。

## 安全边界

Pilot Kit Box 是态势感知和开发设备，不是经过适航认证的主飞行仪表、备用仪表、导航源或防撞系统。所有飞行决策必须以认证机载仪表、批准程序、目视观察和适用法规为准。

## 1. 按键总览

硬件有 **4 个物理按钮** 和 1 个组合手势。TARE / UP / DOWN 在右排 header；MODE 在左排 GPIO5，因为 deep sleep 唤醒需要 LP_IO。

```text
   left header      right header
       MODE         TARE   UP    DOWN
       ( 5)         (26)  (22)   (23)
        |            |     |      |
        ●            ●     ●      ●
```

| 按键 | 短按（< 3 s） | 长按（>= 3 s） | 超长按（>= 10 s） |
|---|---|---|---|
| **TARE** | 按页面决定：PFD / ABOUT 执行 IMU live tare；SETTINGS 切换语言；ADS-B LIST 把高亮飞机绑定为 own-ship | 把当前 IMU tare 保存到 NVS，重启后仍生效 | 工厂重置 IMU：清除 NVS tare、BNO085 持久 DCD 并重新初始化 |
| **MODE** | 页面循环：**PFD -> ADS-B LIST -> SETTINGS -> ABOUT -> PFD** | 软关机：关闭背光并进入 ESP32-P4 deep sleep；再按 MODE 唤醒 / 冷启动 | 无 |
| **UP** | ADS-B LIST / ABOUT 向上滚动 | 抑制，保留给组合手势 | 无 |
| **DOWN** | ADS-B LIST / ABOUT 向下滚动 | 抑制，保留给组合手势 | 无 |

组合手势：

| 手势 | 动作 |
|---|---|
| **UP + DOWN 同时按住 >= 5 s** | 打开 BLE pairing window。当前固件会记录该请求；移动端 UI 处理尚未实现。 |

## 2. 首次使用：航向完全不对怎么办

如果屏幕航向漂移、卡在错误数值、或者旋转设备没有反应，通常是 BNO085 磁力计融合尚未校准，或者芯片 flash 中保存了坏的校准数据。

推荐恢复流程：

### 步骤 1：工厂重置 IMU

```text
TARE 超长按 10 秒
```

串口会看到类似日志：

```text
imu: factory reset: wipe SW tare + NVS + BNO persisted state + reinit chip
imu: BNO: clearing persisted DCD
```

### 步骤 2：画 8 字动作约 15 秒

把设备拿在手里，在空中缓慢画 8 字，同时让设备经过多个不同方向。不要只围绕单一轴旋转。这与手机指南针校准动作类似。

可以观察 1 Hz 串口日志：

```text
imu: rpy = ... (acc=0 ...)   # 刚开始，无可信校准
imu: rpy = ... (acc=1 ...)   # 收敛中
imu: rpy = ... (acc=2 ...)   # 足够可用
imu: rpy = ... (acc=3 ...)   # 高置信度
```

如果设备屏幕已接好，固件在 `acc=0` 持续一段时间后会自动进入罗盘校准向导，显示画 8 字提示和当前质量。`acc >= 2` 稳定后自动退出。

### 步骤 3：保存你想要的零位

让设备保持水平，并让 BNO085 芯片 +X 指向设备的“前方”。先短按 TARE：

```text
TARE 短按
```

这会把当前姿态临时设为零位。然后长按 TARE 3 秒：

```text
TARE 长按 3 秒
```

这会把软件 tare quaternion 写入 ESP32 NVS，重启后仍保留。BNO085 的磁力计 DCD 是另一套内部状态，融合引擎会在收敛后自动保存，不需要用户手动触发。

## 3. 日常航向复位

校准完成后，不需要每次都工厂重置。如果只是想把当前方向重新设为“正前方”，在 PFD 或 ABOUT 页面短按：

```text
TARE 短按
```

这会把当前修正后的姿态设为临时软件 tare 参考。它适用于移动或重新安装设备后的归零，但不会改变上面的安全边界。长按 TARE 才会把它持久化到 NVS。

## 4. 自动磁力计校准

BNO085 的 9-DOF 融合会持续自校准。它会用陀螺仪、加速度计和磁力计估计 hard-iron / soft-iron 偏移。

注意事项：

1. **只有设备在旋转时才会学习。** 静止放着不会让 `acc` 上升。
2. **远离磁干扰。** 校准时避开扬声器、电机、笔记本电脑、手机和大金属桌面。
3. **DCD 会自动保存。** TARE 长按保存的是 ESP32 侧软件零位，不是 BNO085 的 DCD。

如果移动到磁环境差异很大的地方，BNO085 会在后台逐渐更新校准。只有它明显卡住时才需要 TARE 超长按工厂重置。

## 5. 屏幕模式

MODE 短按循环以下用户可见页面：

| 模式 | 内容 |
|---|---|
| **PFD**（默认） | 主飞行显示器：天空/地平线、pitch ladder、bank arc、heading / HSI、高度带、GS / VS、ADS-B 数量。 |
| **ADS-B LIST** | 最近 60 秒内追踪到的飞机列表。上半部分显示 ICAO、呼号、国家、ALT、SPD、HDG、VS、SQK、TYPE；下半部分显示高亮飞机详情。短按 TARE 把高亮飞机绑定为 own-ship，PFD 的 ALT / GS / VS 可来自该机 ADS-B 数据。 |
| **SETTINGS** | 语言设置页。短按 TARE 在 English / 中文之间切换，选择保存到 NVS。 |
| **ABOUT** | 项目版本、构建时间、硬件摘要、校准状态。UP / DOWN 可滚动。 |
| **COMPASS CAL**（自动覆盖层） | BNO085 `acc=0` 持续过久时自动出现的画 8 字校准向导；收敛后自动退出，也可按 MODE 跳过。 |

## 6. 常见问题

| 现象 | 可能原因 | 处理 |
|---|---|---|
| 航向卡住，不随设备旋转变化 | 磁力计融合未校准，`acc=0` | 画 8 字约 15 秒 |
| 重启后航向仍明显错误 | BNO085 DCD 坏了，或 NVS 中保存了旧 tare | TARE 超长按 10 秒，然后重新校准 |
| 首次启动屏幕均匀淡蓝 | LCD 接线有信号短路到 GND | 检查 [`hardware/board_pinout-zh_CN.md`](hardware/board_pinout-zh_CN.md) 的 LCD 接线 |
| `acc` 很久都不超过 1 | 磁干扰太强 | 换到远离金属和电子设备的位置 |
| 航向几分钟内缓慢漂移 | 正常融合微调或安装方向变化 | 短按 TARE 重新归零 |
| MODE 长按后立刻又开机 | 按键未释放就进入唤醒条件 | 固件已等待 MODE 释放；若仍复现，检查 MODE 接线是否被短到 GND |
