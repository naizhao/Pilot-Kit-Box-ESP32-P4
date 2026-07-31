/*
 * config_devname.h — 用户自定义设备名，NVS 持久化（IMPLEMENTATION_PLAN 的 P2-5）。
 *
 * 存的是**名字的前半段**，不是最终广播出去的那串。ble_gatt.c 拼装时会在后面
 * 接上 MAC 后三字节：
 *
 *     未设置：  "Pilot Kit Box-AABBCC"      20 字节
 *     已设置：  "<用户串>-AABBCC"           最长 10+1+6 = 17 字节
 *
 * MAC 后缀不能省。它是为「同一机库停着好几台盒子」准备的——两个人都把名字
 * 改成 N123AB，扫描列表里就分不出哪台是自己的。
 *
 * 长度上限 10 的来历：BLE 广播包总共 31 字节，flags 占 3、名字这条 AD 结构
 * 自带 2 字节头，留给名字本身的是 26。10 + 1 + 6 = 17 < 26，**溢出不可能
 * 发生**，因此 start_advertising() 那条 BLE_HS_EINVAL 的老路（见 ble_gatt.c
 * 里 UUID 曾经塞进 adv 那段注释）不会被这个功能重新踩开。
 *
 * 字符集限制在 A-Z 0-9 - _：
 *   - 航空语境下用户真正想填的是 N123AB / B-1234 / 机位号，全在这个集合里；
 *   - 中文**物理上做不到**：CJK 字形是 i18n catalog 驱动的子集（生成脚本按
 *     catalog 里出现过的字做字模），任意汉字这台盒子自己画不出来，屏上只会
 *     是空白。这条同时否掉了「让手机 App 传一个中文名下来」。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 用户串的最大字符数（不含结尾 NUL）。 */
#define PK_DEVNAME_MAX_LEN   10
/* 建议的缓冲区大小，调用方直接用它开数组。 */
#define PK_DEVNAME_BUF_SIZE  (PK_DEVNAME_MAX_LEN + 1)

/* 单个字符是否可入名。键盘页与 setter 共用同一个判据——两处各写一份，
 * 迟早出现「键盘上点得出、setter 里被吃掉」的字符。 */
bool pk_devname_char_ok(char c);

/*
 * 取当前用户串，拷进 out（总是 NUL 结尾）。**空串 = 从没设置过**，
 * 此时调用方应按出厂默认处理。
 *
 * 拷贝而不是回指针：写发生在触摸任务、读发生在渲染任务与 BLE 初始化，
 * 直接把内部缓冲的地址交出去就是一条撕裂读。
 */
void pk_devname_get(char *out, size_t out_size);

/*
 * 设置用户串并落 NVS。非法字符直接丢弃、超长截断到 PK_DEVNAME_MAX_LEN；
 * name 为 NULL 或过滤后为空即视为「恢复默认」，NVS 里对应的键被抹掉。
 *
 * 只改存储，不碰广播。改完名字要让手机扫得到新名字，另调
 * pk_ble_device_name_apply()（ble_gatt.h）——本模块不该知道 NimBLE 存在。
 */
void pk_devname_set(const char *name);

/* 开机时从 NVS 读取。**必须排在 ble_gatt_init() 之前**：广播名在 on_sync()
 * 里一次拼好，那之后再 load 就晚了。 */
void pk_config_devname_load(void);

#ifdef __cplusplus
}
#endif
