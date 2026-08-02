# UI 帧率分析（4.3″ 800×480）

实测日期 2026-08-02，真机 Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3。

## 结论先说

**当前 PFD 页 9 FPS，每帧 116 ms。瓶颈不是屏幕，是所有像素操作都压在 PSRAM 上、
且其中最贵的两块是 CPU 逐像素做的。**

面板本身能跑 60 Hz，DSI 带宽有 2.7 倍余量，两者都不构成限制（下面有算式）。
116 ms 里有 70 ms（60%）花在两件事上：姿态仪全屏填充、canvas → framebuffer 的
整屏拷贝。这两块都不随「画面内容复杂度」变化，所以列表页画得再简单也一样是
9 FPS。

## 怎么复现这组测量

埋点常驻在 `pfd.c`，走 `ESP_LOGD`，被默认 INFO 级别挡住；
`CONFIG_LOG_MAXIMUM_LEVEL=4` 已经把它们编译进固件，**不必改构建配置**。

在 `pfd_task()` 的循环之前临时加一行，测完删掉（别提交，每秒三行会把串口刷满）：

```c
esp_log_level_set(TAG, ESP_LOG_DEBUG);
```

烧录后抓串口，每秒会吐一组：

```
PERF2: att=<姿态仪> bar=<状态栏> tape=<高度+速度带> hsi=<罗盘+交通> us/frame
PERF3: ppa=<PPA 旋转> vsync_wait=<等 VSYNC> us per flush
PERF:  draw=<页面绘制> lvgl=<LVGL 合成+flush> (flush=<其中 flush> x<每秒次数>) per frame
```

注意 `lvgl` **包含** `flush`，`draw` 是页面绘制的总和（`PERF2` 那几项之和还差
几毫秒，是 infobox/leftbox/toast 同步等零碎）。

## 实测数据（PFD 页，演示模式开启）

连续多秒稳定在同一水平，取其中一组：

```
PERF2: att=33959 bar=1981 tape=8030 hsi=8155 us/frame
PERF3: ppa=15542us vsync_wait=6018us per flush
PERF:  draw=58610us lvgl=57745us (flush=21605us x9) per frame
```

`draw + lvgl = 58.6 + 57.7 = 116.3 ms` → 8.6 FPS，与日志里报的 `PFD 9 FPS` 一致。

| 项 | 耗时 | 占比 | 性质 |
|---|---:|---:|---|
| 姿态仪 `att` | 34.0 ms | 29% | CPU 逐像素，全屏填充 |
| LVGL 合成 blit | 36.1 ms | 31% | CPU 逐像素，`lvgl − flush` |
| PPA 旋转 | 15.5 ms | 13% | 硬件 DMA |
| 高度/速度带 `tape` | 8.0 ms | 7% | CPU，局部矩形 |
| 罗盘 + 交通 `hsi` | 8.2 ms | 7% | CPU，局部矩形 |
| 等 VSYNC | 6.0 ms | 5% | 空等 |
| 状态栏 + infobox 等 | ~8 ms | 7% | CPU，局部 |
| **合计** | **116 ms** | | **8.6 FPS** |

## 为什么慢：换算成带宽就很清楚

三块缓冲全在 PSRAM：

| 缓冲 | 大小 | 分配处 |
|---|---|---|
| canvas（PFD 画布） | 768 KB | `lv_port.c` `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` |
| 逻辑 framebuffer | 768 KB | `display.c` `heap_caps_aligned_alloc(..., MALLOC_CAP_SPIRAM)` |
| DPI 扫描双缓冲 ×2 | 768 KB ×2 | `num_fbs = 2` |

把耗时换算成有效带宽，CPU 与硬件的差距是 4 倍：

| 操作 | 数据量 | 耗时 | 等效带宽 |
|---|---|---:|---:|
| 姿态仪填充（CPU 逐像素写） | 768 KB 写 | 34.0 ms | **23 MB/s** |
| LVGL blit（CPU 软件拷贝） | 768 KB 读 + 768 KB 写 | 36.1 ms | **42 MB/s** |
| PPA 旋转（硬件 DMA） | 768 KB 读 + 768 KB 写 | 15.5 ms | **97 MB/s** |

姿态仪之所以是最大的单个绘制项，不是因为它画得复杂 —— 而是**它是唯一每帧铺满
整屏的**（天空、地面两个大色块共 384,000 像素）。`tape` / `hsi` 只画各自的局部
矩形，所以只要 8 ms。

## 屏幕已排除

按 `display.c` 的 `video_timing` 与 `display.h` 的常量算：

- 总行宽 = 480 + 42(hbp) + 12(hpw) + 42(hfp) = **576**
- 总行数 = 800 + 2(vbp) + 8(vpw) + 60(vfp) = **870**
- 每帧 576 × 870 = 501,120 clk，`PK_LCD_DPI_CLOCK_MHZ = 30`
- 刷新率 = 30 MHz ÷ 501,120 = **59.9 Hz**

DSI 链路：2 lane × 500 Mbps = 125 MB/s；60 FPS 只需 768 KB × 60 = 46 MB/s。

**面板和链路都有充裕余量，把屏幕换小不会提升帧率。**

## 为什么每一页都是 9 FPS

`lvgl + flush = 57.7 ms` 与页面画什么**完全无关**：

1. `lv_port.c` 的 `pk_lv_port_invalidate()` 每帧 `lv_obj_invalidate(s_canvas)`
   把整块 canvas 标脏
2. 于是 LVGL 每帧合成整屏 → 768 KB blit
3. PPA 旋转整屏
4. 等 VSYNC

所以列表页（只画几十个矩形和文本）与 PFD 页（姿态仪 + 两条 tape + 罗盘 + 交通）
帧率相同。**任何只优化"画什么"的努力，上限都被这 58 ms 卡死。**

`pfd.c` 循环末尾的 `vTaskDelayUntil(pdMS_TO_TICKS(33))`（30 FPS 目标）早已不起
作用 —— 一帧远超 33 ms，每次都是立即返回。

## 优化方向（按收益排序，均未实施）

### 1. 砍掉 canvas → framebuffer 的 blit —— 省约 36 ms

`lv_port.c` 里已经记着一次**失败的**尝试，动手前必读：试过让 PFD 直接画进
display 缓冲、屏幕背景设 `LV_OPA_TRANSP`、每帧只标脏 FAB/dock。真机结果是 FAB
带一圈白底且与 dock 持续频闪，两个硬伤：

- RGB565 没有 alpha 通道，DIRECT 模式下 LVGL 重绘控件脏区需要一个不透明的底，
  屏幕透明就拿不到，圆角与半透明处填出未定义的颜色；
- PFD 每帧覆盖整屏，而 LVGL 有自己的刷新周期（`LV_DEF_REFR_PERIOD`），并非每次
  `lv_timer_handler` 都真的重绘 —— 有的帧控件画上去、有的帧被 PFD 盖掉。

该文件给出的结论是：**要省掉这次 blit，得让控件与 PFD 在同一次遍历里合成**
（例如放弃 LVGL 图层、自己叠控件像素），改标脏范围解决不了。这是最大的一块，
也是改动最大的一块。

### 2. 姿态仪全屏填充交给硬件 —— 省约 26 ms

天空、地面是两个规则区域，目前由 CPU 逐像素写 PSRAM（23 MB/s）。PPA 实测能跑到
97 MB/s，同样的 768 KB 用硬件 fill 约 8 ms，**省下约 26 ms**。

注意天地线是斜的（随 roll 倾斜），不是两个正矩形；可行的做法是先用硬件把两个
色块按水平分割填好，再由 CPU 只处理倾斜边界那条带，而不是整屏逐像素。

### 3. PPA 旋转 15.5 ms 偏慢 —— 可能有几毫秒

对 1.5 MB 的搬运来说 97 MB/s 不算快。值得查：`PPA_TRANS_MODE_BLOCKING`（同步等
待，期间 CPU 空转）、color mode 与内存对齐是否走在最优路径上。改成异步 + 双缓冲
可以让 CPU 在 PPA 工作时继续画下一帧。

### 4. VSYNC 空等 6 ms

`display.c` 的 flush 提交后死等刷新计数变化。帧率上去之后这一项会变得更显眼
（60 Hz 下一帧 16.7 ms）。同样可以靠异步化把这段等待与绘制重叠。

**只做 1 和 2，116 ms 可压到约 54 ms（≈18 FPS）；四项都做有望接近 30 FPS。**

## 已排除的方向，别重走

- **屏幕太大 / 换小屏**：面板 60 Hz、DSI 余量 2.7 倍，都不是限制。
- **提高 `vTaskDelayUntil` 的目标帧率**：它已经不起作用，改小没有任何影响。
- **只优化某一页的绘制**：58 ms 的固定开销与页面内容无关。

## 测量条件

- 固件为 `feat/lcd-4.3-touch` 分支 2026-08-02 的构建，包含当时工作树里的全部改动。
- 演示模式开启（合成数据）。演示数据只影响数值，不改变绘制路径与像素量。
- 页面为 PFD 主页。列表页当时观测到同为 9 FPS，但未逐项分解 —— 如需其它页面的
  分解表，按上面的方法重测一次即可。
