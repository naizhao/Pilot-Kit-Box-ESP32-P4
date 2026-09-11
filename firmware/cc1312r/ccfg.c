/*
 * ccfg.c — CC1312R 的 CCFG 覆盖：把 ROM bootloader 和它的 backdoor 打开。
 *
 * 为什么必须有这个文件
 * --------------------
 * SimpleLink SDK 自带的 startup_files/ccfg.c **默认是把 bootloader 关掉的**：
 *     #define SET_CCFG_BL_CONFIG_BOOTLOADER_ENABLE  0x00  // Disable ROM boot loader
 * 照默认编出来的镜像，CCFG 里 BL_CONFIG = 0x00FFFFFF。而 TRM 表 11-15 写得很死：
 * BOOTLOADER_ENABLE **只有 0xC5 是 enabled，其余任何值一律 disabled**。
 *
 * 后果是：镜像烧进去之后，这颗片子**永远**只能靠 cJTAG 仿真器更新。本板的
 * RP2040 代刷通道（firmware/rp2040/cc13_bsl.c，走 ROM 的 SSI0 串行 bootloader）
 * 会彻底用不上——而那条通道正是这块板当初的设计意图（PINMAP 里 SUBG_SYNC
 * 就标着「sync/bootloader trigger」）。
 *
 * 2026-09-11 的处境更说明这一点：这颗是空片，CCFG 是擦除态全 0xFF，
 * BOOTLOADER_ENABLE=0xFF≠0xC5，所以 bootloader 也是关的，首刷只能上仿真器。
 * **这一次是没办法；但烧进去的这一版必须保证以后不用再来一次。**
 *
 * 取值依据
 * --------
 * BL_PIN_NUMBER = 13：CC1312R 的 DIO13 = 本板 SUBG_SYNC ← RP2040 GPIO15
 *   （hardware/expansion-board-v4/PINMAP.md，网表 U10/U8 焊盘已核）。
 *
 * BL_LEVEL = 1（高有效）：**方向不能反**。RP2040 的 GPIO15 复位后是输入 +
 *   内部下拉，也就是说本板 DIO13 在上电那一刻天然是低。若设成低有效，
 *   CC1312R 每次上电都会跳进 bootloader 而不去跑应用固件——等于把产品做死。
 *   设成高有效则：平时低 → 正常启动；RP2040 主动拉高 → 进 bootloader。
 *   （与 PantsForBirds/adsbee 的选择一致，他们也是 active high。）
 *
 * 安全取舍（产品决定，2026-09-11 用户批准）
 * ----------------------------------------
 * 开 backdoor 意味着任何能碰到 DIO13 和 SSI0 的人都能通过 bootloader 读回
 * flash。这颗 CC1312R 里只有 978 UAT 收发固件，不含密钥、证书或用户数据，
 * 读回的风险可接受；换来的是「仿真器一辈子只用那一次」。
 * 如果以后往这颗片子里放了敏感内容，必须重新评估这一行。
 */

/* ⚠ 这四个 #define 必须出现在 include SDK 的 ccfg.c **之前**——那边全是
 * #ifndef 兜底，先定义才覆盖得掉。 */

/* 0xC5 = 使能 ROM bootloader。除 0xC5 外任何值都是 disabled（TRM 表 11-15）。*/
#define SET_CCFG_BL_CONFIG_BOOTLOADER_ENABLE   0xC5

/* 0xC5 = 使能 backdoor：有有效镜像时也能靠引脚强行进 bootloader。 */
#define SET_CCFG_BL_CONFIG_BL_ENABLE           0xC5

/* backdoor 引脚：DIO13 = SUBG_SYNC ← RP2040 GPIO15 */
#define SET_CCFG_BL_CONFIG_BL_PIN_NUMBER       13

/* 高有效。理由见上——设成低有效会让本板每次上电都进 bootloader。 */
#define SET_CCFG_BL_CONFIG_BL_LEVEL            0x1

#include <ti/devices/cc13x2_cc26x2/startup_files/ccfg.c>
