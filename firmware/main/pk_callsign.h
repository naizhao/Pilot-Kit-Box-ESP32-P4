/* pk_callsign.h — ADS-B 呼号规范化（纯逻辑，无平台依赖）。
 *
 * 为什么单独一个文件：aircraft_state.c 拖着 FreeRTOS/esp_log/demo_data，
 * host 测试没法整份 include（sim 也不编它，compat 里连 esp_log.h 都没有）。
 * 拆法照 pk_rec_store.c(纯逻辑) / pk_rec_store_fs.c(FS 胶水) 那一对。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 8 个字符 + NUL。与 aircraft_state.h 的 AIRCRAFT_CALLSIGN_LEN 同值，
 * 那边用 static_assert 钉住两者一致。 */
#define PK_CALLSIGN_LEN 9

/*
 * 把 mode-s 解出来的 flight 字段规范化成可显示的呼号。
 * 成功写入 out（保证 NUL 结尾）并返回 true；失败返回 false，此时 out 的
 * 内容未定义，调用方应保留上一次的好值。
 *
 * 为什么需要这一层：mode-s.c 的 ais_charset 把 6-bit 码位翻译成字符时，
 * 64 个码位里有 27 个是 ADS-B 保留位（0、27..31、33..47、58..63），表里
 * 一律写成 '?'。CRC 过了不代表这 8 个字符合法——发射端填了保留码位照样
 * 过 CRC。以前 aircraft_state.c 无条件 memcpy，屏上就出现 "CE?9?82"。
 *
 * 遇到非法字符**整条丢弃**，不做"剔掉问号再显示"：那会把 CE?9?82 变成
 * CE982，一个看起来完全合法、实际是错的呼号——比显示问号更危险。ident
 * 报文是重复播发的，丢一条等下一条即可。
 *
 * 同理，结果为空（整条填充空格，即对方没播呼号）也返回 false，避免一条
 * 空报文抹掉之前收到的好呼号。
 *
 * 合法字符集：A-Z、0-9、空格。尾部填充空格会被剥掉（ADS-B 右侧补空格是
 * 正常填充，不是错误）；中间的空格原样保留。
 */
bool pk_callsign_sanitize(const char *raw, char out[PK_CALLSIGN_LEN]);

/*
 * 取一架飞机的**可显示**呼号，没有呼号就退回 ICAO 十六进制。
 * out 保证非空且 NUL 结尾——留空会被看成渲染坏了，比退回 ICAO 更糟。
 *
 * 不收 aircraft_t 而是散参数，为的是这个文件零依赖、能直接 include 进
 * host 测试；aircraft_state.h 会拖 mode-s.h 进来。
 *
 * 2026-08-04 收敛自三份各写各的实现：traffic_page/adsb_list 各有一份
 * callsign_of()，map_page 则是内联手写的第三份。三份行为还不一致——
 * adsb_list 那份剔掉**所有**空格（"N1 2AB" 会变成 "N12AB"，而中间的空格
 * 是合法呼号内容），另两份只剔尾部。这里统一成只剔尾部填充。
 *
 * map_page 那份手写的还漏了初始化：`char cs[10];` 之后只在
 * have_callsign 为真时才写，接着就 `if (!cs[0])` 去读——false 分支下整个
 * 缓冲从没被写过，读到的是栈垃圾。垃圾首字节非零就跳过 ICAO 回退，把栈
 * 内容当字符串打到屏上，且没有 NUL 保证会一路读到越界。真机现象是呼号
 * 每帧乱变、混进 '\' 这类 ADS-B 字符集里根本不存在的字符。
 */
void pk_callsign_display(bool have_callsign, const char *callsign,
                         uint32_t icao24, char *out, size_t cap);
