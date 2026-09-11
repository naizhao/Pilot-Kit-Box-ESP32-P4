/* cc13_bsl_proto.c — 见 cc13_bsl_proto.h。纯 C，无平台依赖。 */
#include "cc13_bsl_proto.h"

size_t cc13_bsl_pkt_build(uint8_t *out, size_t cap, uint8_t cmd,
                          const uint8_t *params, size_t nparams)
{
    if (!out || (!params && nparams)) return 0;
    /* 整包 = size(1) + checksum(1) + cmd(1) + params。size 字段是单字节，
     * 所以整包不能超过 255。 */
    const size_t total = 3u + nparams;
    if (total > CC13_BSL_MAX_PACKET) return 0;
    if (cap < total) return 0;                  /* 失败不写输出 */

    uint32_t sum = cmd;
    for (size_t i = 0; i < nparams; i++) sum += params[i];

    out[0] = (uint8_t)total;
    out[1] = (uint8_t)(sum & 0xFFu);            /* checksum **不含**两个头字节 */
    out[2] = cmd;
    for (size_t i = 0; i < nparams; i++) out[3 + i] = params[i];
    return total;
}

bool cc13_bsl_pkt_valid(const uint8_t *pkt, size_t len)
{
    /* 最短的合法包是 3 字节（size+checksum+1 字节数据），
     * 且声明长度必须与实际收到的长度严格相符——多一个字节都不认，
     * 否则就是拿不确定的字节去当状态码解释。 */
    if (!pkt || len < 3u) return false;
    if (pkt[0] != len) return false;

    uint32_t sum = 0;
    for (size_t i = 2; i < len; i++) sum += pkt[i];
    return (uint8_t)(sum & 0xFFu) == pkt[1];
}

void cc13_bsl_put_be32(uint8_t *out, uint32_t v)
{
    out[0] = (uint8_t)(v >> 24);
    out[1] = (uint8_t)(v >> 16);
    out[2] = (uint8_t)(v >> 8);
    out[3] = (uint8_t)v;
}

size_t cc13_bsl_chunk_len(size_t remaining)
{
    return (remaining > CC13_BSL_MAX_DATA) ? (size_t)CC13_BSL_MAX_DATA : remaining;
}

/* 标准 IEEE 802.3 CRC-32。不建表：352 KB 镜像上多跑几万次移位在 RP2040 上
 * 完全无所谓，而一张 1 KB 的表要常驻 RAM——这颗片子上 RAM 比时间金贵。 */
uint32_t cc13_bsl_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc;
}

uint32_t cc13_bsl_crc32_final(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

uint32_t cc13_bsl_crc32(const uint8_t *data, size_t len)
{
    return cc13_bsl_crc32_final(
        cc13_bsl_crc32_update(0xFFFFFFFFu, data, len));
}
