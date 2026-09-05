#pragma once

/* gps_nmea.h —— 纯 NMEA 句法解析（WP-B Task 1 自 gps_task.c 抽出）。
 *
 * 职责只有一件事：一行 → 「可信吗 + 字段 + 消息类型」。不依赖 ESP-IDF /
 * FreeRTOS，host 测试 firmware/test/test_gps_nmea.c 直接编译本模块。
 *
 * 合同（除 checksum 门这一处行为升级外，与老 gps_task.c 的 handle_line 一致）：
 *   - *hh 校验和 = '$' 与 '*' 之间全部字节的 XOR，与 '*' 后两位 hex 逐位
 *     比较（大小写不敏感）；错则整句丢弃；
 *   - type = addr（f[0]）的末 3 字符——老的「按后缀匹配」语义，原样保留
 *     大小写；真实模块只发大写，调用方区分大小写匹配即可；
 *   - *hh 从句尾剥掉：切出来的字段是干净 CSV（TXT 的 strstr 等都受益）；
 *   - f[0] 仍是带 talker 的原始 addr（如 "GNRMC"）——parse_gsv 靠它映射星座；
 *   - 长度 ≥ GPS_NMEA_LINE_MAX 的行整句拒收，不截半句（老 UART 组行在
 *     溢出时丢整行，解析层把同一道闸补齐）；
 *   - 未知消息类型（如 GSA）同样返回 1、type 照填，由调用方按 type 取舍。
 *
 * 非重入：字段指向模块内部缓冲，生命周期到下一次 gps_nmea_feed_line()。
 * gps 任务单消费者足够；将来多消费者须加锁或先拷出。
 */

#define GPS_NMEA_LINE_MAX   128  /* 内部行缓冲（含 NUL）；句长 ≥ 128 整句拒收 */
#define GPS_NMEA_MAX_FIELDS 24   /* 字段数上限；超出部分留在最后一个字段里 */

typedef struct {
    char        type[6];                /* 去 talker 的消息类型，如 "RMC"（含 '\0'） */
    const char *f[GPS_NMEA_MAX_FIELDS]; /* 字段指针；f[0] 为原始 addr；到下次 parse 前有效 */
    int         n;                      /* 字段数（空字段也计数） */
} gps_nmea_msg_t;

/* 解析一行已组装好的 NMEA 句子（不含 \r\n）。
 * 返回 1 = 已解析（out->type / out->f / out->n 可用）；
 * 返回 0 = 忽略（非 '$' 开头、空行、缺 *hh、* 后非 hex 或位数不对、
 *          checksum 错、超长行）；失败时 out->n 清零、out->type 置空。 */
int gps_nmea_feed_line(gps_nmea_msg_t *out, const char *line);
