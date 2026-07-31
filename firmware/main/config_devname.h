/*
 * config_devname.h — 用户自定义设备名，NVS 持久化（IMPLEMENTATION_PLAN 的 P2-5）。
 *
 * 存的是**用户串**。用户设过名字，广播出去的就是这一串，一个字符都不加：
 *
 *     未设置：  "Pilot Kit Box-AABBCC"      出厂前缀 + MAC 后三字节，20 字节
 *     已设置：  "<用户串>"                  原样广播，最长 26 字节
 *
 * 为什么默认名保留 MAC 后缀、自定义名不保留（不是漏了）
 * ----------------------------------------------------
 * 后缀解决的是「一个机库停着好几台盒子，扫描列表里分不出哪台是自己的」。这个
 * 问题只在**所有设备叫同一个名字**时才存在——出厂默认名恰恰如此，所以默认名
 * 必须带后缀。用户一旦自己取了名，重名与否是他自己的选择，固件不该替他决定；
 * 而且 BLE 连接认的是设备地址不是名字，App 扫到的每条结果里都带着 MAC，去掉
 * 后缀既不影响可连接性也不影响可区分性。名字**只是给人看的显示串**（这一点
 * docs/ble_protocol.md「Filtering」一节已经写死：客户端必须按 Service UUID
 * 过滤，不许按名字前缀）。留个 "-AABBCC" 小尾巴只会让「我明明改成 N123AB，
 * 手机上却显示 N123AB-0B5A8A」。
 *
 * 长度上限 26 的来历（三条约束里最紧的那条就是它）：
 *   1. adv 预算：广播包共 31 字节，flags AD 占 3、名字这条 AD 结构自带 2 字节
 *      头，留给名字本身 31−3−2 = **26**。自定义名不再拼后缀，26 个字符正好
 *      顶满，多一个就会让 ble_gap_adv_set_fields() 回 BLE_HS_EINVAL（见
 *      ble_gatt.c 里 UUID 曾经塞进 adv 那段注释）。ble_gatt.c 有编译期断言。
 *   2. 设置页值框：那一行显示的是完整广播名，框宽按 26 字符 × S 档 11 px
 *      + 左右各 10 = 306 px 定（settings_draw.c），不截断、不降档。
 *   3. 键盘输入框：M 档 15 px/字符，一行画得下 40 多个字符，不是瓶颈；但
 *      编辑器自己的缓冲 PK_KBD_TEXT_MAX 必须 ≥ 这个数，否则屏上敲得进、
 *      存的时候被静默夹掉（settings_page.c 有编译期断言）。
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

/* 用户串的最大字符数（不含结尾 NUL）。= adv 留给名字的 31−3−2 字节，见上。 */
#define PK_DEVNAME_MAX_LEN   26
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
 * name 为 NULL 或过滤后为空即视为「恢复默认」（连同 MAC 后缀一起回来），
 * NVS 里对应的键被抹掉。
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
