/*
 * uat_decode.c —— 978 MHz UAT 解码纯单元（WP-E Task 1）。
 *
 * 每个参数/判据的出处都在行内以「事实卡 §N」引用
 * docs/internal/firmware-v3v4/UAT-PROTOCOL-FACTS.md；[MUT]/[FA]/[STX]
 * 代号同该卡 §0。实现为本仓独立编写（上游 dump978 为 GPL-2.0，事实卡
 * §0 许可审计——只取其公开事实与 DO-282B 标准向量，不移植其代码）；
 * 正确性锚点是 DO-282B Table 2-104/2-105 符合性向量（host 测试内嵌）。
 *
 * RS 解码采用教科书 Berlekamp-Massey + Chien + Forney（缩短码在块内
 * 直接解码，前导零不参与伴随式，事实卡 §2）。与 dump978 的行为差异仅
 * 一处且有意为之：全零输入拒绝（事实卡 §8——SPI §2.3 规则 5 的全零
 * 事务是合法"无帧"，而全零恰是 RS 合法码字，闸门必须在 RS 之前）。
 *
 * 栈占用：上行路径峰值约 0.6 KB（work[432] + block[92] + BM 局部），
 * 无堆、无静态可写状态（除 uat_fec_init 一次性构建的常量表）。
 */

#include <string.h>

#include "uat_decode.h"

/* ── GF(256)（本原多项式 0x187，事实卡 §2）────────────────────── */

static uint8_t gf_alpha[256]; /* 指数→元素；alpha^255 = 1（见 init）   */
static uint8_t gf_log[256];   /* 元素→指数；gf_log[0]=0xFF 即 -inf   */

static uint8_t gmul(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0)
        return 0;
    return gf_alpha[(unsigned)(gf_log[a] + gf_log[b]) % 255];
}

static uint8_t ginv(uint8_t a)
{
    /* ginv(1) 必须经过 gf_alpha[255]（log[1]=0 → 255-0=255）——审计
     * round-WP-E-1 实测反例：表未填满 255 号槽时 ginv(1)=0，末块
     * 最后符号的单字节错被误判不可纠（6/552）。alpha^255 = 1（本原
     * 多项式循环周期 255）。 */
    return gf_alpha[255 - gf_log[a]];
}

static int fec_ready;

void uat_fec_init(void)
{
    if (fec_ready)
        return;

    unsigned v = 1;
    for (int i = 0; i < 255; ++i) {
        gf_alpha[i] = (uint8_t)v;
        gf_log[(uint8_t)v] = (uint8_t)i;
        v <<= 1;
        if (v & 0x100)
            v ^= 0x187;
    }
    /* v 现在回到 1：alpha^255 = alpha^0 = 1——填入第 255 槽，ginv(1)
     * 的取径才闭合（见 ginv 注释）。 */
    gf_alpha[255] = 1;
    fec_ready = 1;
}

/* ── RS 单块解码（in-place 纠错，返回纠正字节数，-1=不可纠）──────
 *
 * block[0..n-1]：n=30/48/92，data/parity 正序（事实卡 §2 字节序）。
 * 约定（对齐 dump978 fec.c 依赖的行为）：
 *   - 伴随式全零 → 0，不改数据；
 *   - λ 次数 ≠ Chien 根数、纠正数 > nroots/2、或纠后复核伴随式非零
 *     → 返回 -1。纠后复核失败时数据可能已被改写——调用方对"失败后
 *     还要重试"的场景（下行长→短）必须换干净副本重试。
 */
static int rs_decode_block(uint8_t *block, int n, int nroots)
{
    uint8_t s[20];
    int syn_error = 0;

    /* 伴随式 S_i = r(α^(120+i))，r(x)=Σ block[p]·x^(n-1-p)（Horner） */
    for (int i = 0; i < nroots; ++i) {
        uint8_t beta = gf_alpha[120 + i];
        uint8_t acc = 0;
        for (int p = 0; p < n; ++p)
            acc = acc ? (uint8_t)(gmul(acc, beta) ^ block[p])
                      : (uint8_t)(0 ^ block[p]);
        s[i] = acc;
        syn_error |= acc;
    }
    if (!syn_error)
        return 0;

    /* Berlekamp-Massey：求最小次数错误定位多项式 λ(x)（升幂） */
    uint8_t lambda[21] = { 1 }, b[21] = { 1 };
    int l = 0, m = 1;
    uint8_t b_scale = 1; /* b(x) 保留的缩放，除法用 ginv 收掉       */
    for (int r = 0; r < nroots; ++r) {
        uint8_t d = s[r];
        for (int i = 1; i <= l; ++i)
            d ^= gmul(lambda[i], s[r - i]);
        if (d == 0) {
            ++m;
        } else {
            uint8_t coef = gmul(d, ginv(b_scale));
            if (2 * l <= r) {
                uint8_t t[21];
                memcpy(t, lambda, sizeof lambda);
                for (int i = 0; i + m <= nroots; ++i)
                    if (b[i])
                        lambda[i + m] ^= gmul(coef, b[i]);
                l = r + 1 - l;
                memcpy(b, t, sizeof b);
                b_scale = d;
                m = 1;
            } else {
                for (int i = 0; i + m <= nroots; ++i)
                    if (b[i])
                        lambda[i + m] ^= gmul(coef, b[i]);
                ++m;
            }
        }
    }
    int deg_lambda = 0;
    for (int i = 0; i <= nroots; ++i)
        if (lambda[i])
            deg_lambda = i;
    if (deg_lambda == 0 || deg_lambda > nroots / 2)
        return -1; /* 无错码字不会到这里（伴随式非零）；超纠错力拒绝 */

    /* Chien：位置 p 的定位元 X=α^(n-1-p)，错位 ⇔ λ(X^{-1})=0 */
    int pos[10]; /* deg_lambda ≤ nroots/2 = 10（三个码族的最大值）  */
    int count = 0;
    for (int p = 0; p < n && count < deg_lambda; ++p) {
        unsigned e = (unsigned)(n - 1 - p) % 255; /* X 的指数          */
        uint8_t acc = 1;
        for (int j = 1; j <= deg_lambda; ++j)
            acc ^= gmul(lambda[j], gf_alpha[(255 - (unsigned)(e * j % 255)) % 255]);
        if (acc == 0)
            pos[count++] = p;
    }
    if (count != deg_lambda)
        return -1;

    /* ω(x) = S(x)·λ(x) mod x^nroots（S(x)=Σ S_i x^i，升幂） */
    uint8_t omega[20] = { 0 };
    for (int i = 0; i < nroots; ++i) {
        uint8_t acc = 0;
        for (int j = 0; j <= i && j <= deg_lambda; ++j)
            acc ^= gmul(s[i - j], lambda[j]);
        omega[i] = acc;
    }

    /* Forney：e_p = X^(1-fcr) · ω(X^{-1}) / λ'(X^{-1})，fcr=120     */
    for (int k = 0; k < count; ++k) {
        int p = pos[k];
        unsigned e = (unsigned)(n - 1 - p) % 255;        /* X 指数    */
        unsigned einy = (255 - e) % 255;                 /* X^{-1}    */
        uint8_t num1 = 0, den = 0;
        for (int i = 0; i < nroots; ++i)
            if (omega[i])
                num1 ^= gmul(omega[i], gf_alpha[(einy * (unsigned)i) % 255]);
        for (int i = 1; i <= deg_lambda; i += 2)         /* 形式导数  */
            if (lambda[i])
                den ^= gmul(lambda[i], gf_alpha[(einy * (unsigned)(i - 1)) % 255]);
        if (den == 0 || num1 == 0)
            return -1;
        uint8_t xk1mfc = gf_alpha[((e * (255 + 1 - 120)) ) % 255]; /* X^(1-120) */
        uint8_t mag = gmul(xk1mfc, gmul(num1, ginv(den)));
        block[p] ^= mag;
    }

    /* 纠后复核：伴随式必须归零（拦截 >t 错误的伪纠） */
    for (int i = 0; i < nroots; ++i) {
        uint8_t beta = gf_alpha[120 + i];
        uint8_t acc = 0;
        for (int p = 0; p < n; ++p)
            acc = acc ? (uint8_t)(gmul(acc, beta) ^ block[p]) : block[p];
        if (acc)
            return -1;
    }
    return count;
}

/* ── 上行 ──────────────────────────────────────────────────────── */

static bool all_zero(const uint8_t *p, size_t n)
{
    uint8_t v = 0;
    for (size_t i = 0; i < n; ++i)
        v |= p[i];
    return v == 0;
}

static void decode_info_frames(const uint8_t *app /*424 B*/,
                               uat_uplink_t *up)
{
    /* 信息帧游走判据全部来自事实卡 §4（[MUT] uat_decode.c:1068-1090）。 */
    int pos = 0;
    up->num_info_frames = 0;
    while (up->num_info_frames < UAT_UPLINK_MAX_INFO_FRAMES &&
           pos + 2 <= (int)UAT_UPLINK_APP_BYTES) {
        uat_info_frame_t *fr = &up->info[up->num_info_frames];
        fr->length = (uint16_t)((app[pos] << 1) | (app[pos + 1] >> 7));
        fr->type = (uint8_t)(app[pos + 1] & 0x0f);
        if (fr->length == 0 && fr->type == 0)
            break;                                    /* 终止符          */
        if (pos + fr->length + 2 > (int)UAT_UPLINK_APP_BYTES)
            break;                                    /* 越界即停        */
        fr->data = app + pos + 2;

        /* FIS-B APDU 头（type==0 且 length≥4，事实卡 §4） */
        fr->is_fisb = false;
        if (fr->type == 0 && fr->length >= 4) {
            const uint8_t *d = fr->data;
            uat_fisb_t *f = &fr->fisb;
            f->flags = (uint8_t)(((d[0] & 0x80) ? 8 : 0) |
                                 ((d[0] & 0x40) ? 4 : 0) |
                                 ((d[0] & 0x20) ? 2 : 0) |
                                 ((d[1] & 0x02) ? 1 : 0));
            f->product_id =
                (uint16_t)(((d[0] & 0x1f) << 6) | (d[1] >> 2));
            f->monthday_valid = false;
            f->seconds_valid = false;
            f->month = f->day = f->hours = f->minutes = f->seconds = 0;
            unsigned t_opt = (unsigned)((d[1] & 0x01) << 1) | (d[2] >> 7);
            const uint8_t *pl = d;
            uint16_t plen = 0;
            switch (t_opt) {
            case 0: /* 时分（头 4 B）——入口已保证 length≥4 */
                f->hours = (d[2] & 0x7c) >> 2;
                f->minutes = (uint8_t)((d[2] & 0x03) << 4) | (d[3] >> 4);
                pl = d + 4;
                plen = (uint16_t)(fr->length - 4);
                break;
            case 1: /* 时分秒（5 B） */
                if (fr->length < 5)
                    break;
                f->seconds_valid = true;
                f->hours = (d[2] & 0x7c) >> 2;
                f->minutes = (uint8_t)((d[2] & 0x03) << 4) | (d[3] >> 4);
                f->seconds = (uint8_t)((d[3] & 0x0f) << 2) | (d[4] >> 6);
                pl = d + 5;
                plen = (uint16_t)(fr->length - 5);
                break;
            case 2: /* 月日时分（5 B） */
                if (fr->length < 5)
                    break;
                f->monthday_valid = true;
                f->month = (d[2] & 0x78) >> 3;
                f->day = (uint8_t)((d[2] & 0x07) << 2) | (d[3] >> 6);
                f->hours = (d[3] & 0x3e) >> 1;
                f->minutes =
                    (uint8_t)((d[3] & 0x01) << 5) | (d[4] >> 3);
                pl = d + 5;
                plen = (uint16_t)(fr->length - 5);
                break;
            case 3: /* 月日时分秒（6 B） */
                if (fr->length < 6)
                    break;
                f->monthday_valid = true;
                f->seconds_valid = true;
                f->month = (d[2] & 0x78) >> 3;
                f->day = (uint8_t)((d[2] & 0x07) << 2) | (d[3] >> 6);
                f->hours = (d[3] & 0x3e) >> 1;
                f->minutes =
                    (uint8_t)((d[3] & 0x01) << 5) | (d[4] >> 3);
                f->seconds = (uint8_t)((d[4] & 0x03) << 3) | (d[5] >> 5);
                pl = d + 6;
                plen = (uint16_t)(fr->length - 6);
                break;
            default: /* t_opt 只有可能 0..3（2 bit），不会到这 */
                break;
            }
            if (pl != d) {
                fr->is_fisb = true;
                f->payload = pl;
                f->payload_len = plen;
            }
        }
        ++up->num_info_frames;
        pos += fr->length + 2;
    }
}

bool uat_uplink_decode(const uint8_t frame[UAT_UPLINK_FRAME_BYTES],
                       uint8_t data_out[UAT_UPLINK_DATA_BYTES],
                       uat_uplink_t *out, uint8_t *rs_corrected)
{
    if (!frame || !data_out)
        return false;
    /* 全零拒绝先于 RS：全零是合法 RS 码字，闸门必须在 FEC 之前
     * （事实卡 §8；SPI §2.3 规则 5 全零事务=合法"无帧"）。 */
    if (all_zero(frame, UAT_UPLINK_FRAME_BYTES))
        return false;

    /* 解交织（事实卡 §3：block[b][i] = frame[i*6+b]）+ 逐块 RS。
     * 全部块通过才落盘 data_out——失败路径不改写输出（合同 4）。 */
    uint8_t work[UAT_UPLINK_DATA_BYTES];
    unsigned total = 0;
    for (int b = 0; b < 6; ++b) {
        uint8_t block[92];
        for (int i = 0; i < 92; ++i)
            block[i] = frame[i * 6 + b];
        int corrected = rs_decode_block(block, 92, 20);
        if (corrected < 0 || corrected > 10) /* dump978 闸门，事实卡 §2 */
            return false;
        total += (unsigned)corrected;
        memcpy(work + b * 72, block, 72);
    }
    memcpy(data_out, work, UAT_UPLINK_DATA_BYTES);
    if (rs_corrected)
        *rs_corrected = (uint8_t)total; /* ≤ 60，u8 放得下 */

    if (out) {
        uint32_t raw_lat = ((uint32_t)data_out[0] << 15) |
                           ((uint32_t)data_out[1] << 7) |
                           (data_out[2] >> 1);
        uint32_t raw_lon = (((uint32_t)data_out[2] & 1) << 23) |
                           ((uint32_t)data_out[3] << 15) |
                           ((uint32_t)data_out[4] << 7) |
                           (data_out[5] >> 1);
        out->raw_lat = raw_lat;
        out->raw_lon = raw_lon;
        /* deg_e6 = floor(raw·360e6 / 2²⁴)；纬 >90° 减 180°、经 >180°
         * 减 360°（事实卡 §4 的换算+回绕，整数化）。 */
        int32_t lat = (int32_t)(((uint64_t)raw_lat * 360000000u) >> 24);
        int32_t lon = (int32_t)(((uint64_t)raw_lon * 360000000u) >> 24);
        if (lat > 90000000)
            lat -= 180000000;
        if (lon > 180000000)
            lon -= 360000000;
        out->lat_deg_e6 = lat;
        out->lon_deg_e6 = lon;
        out->position_valid = (data_out[5] & 0x01) != 0;
        out->utc_coupled = (data_out[6] & 0x80) != 0;
        out->app_data_valid = (data_out[6] & 0x20) != 0;
        out->slot_id = (uint8_t)(data_out[6] & 0x1f);
        out->tisb_site_id = (uint8_t)(data_out[7] >> 4);
        if (out->app_data_valid)
            decode_info_frames(data_out + 8, out);
        else
            out->num_info_frames = 0;
    }
    return true;
}

/* ── 下行 ──────────────────────────────────────────────────────── */

static void dl_hdr_from(const uint8_t *d, uat_dl_hdr_t *hdr)
{
    hdr->mdb_type = (uint8_t)((d[0] >> 3) & 0x1f);
    hdr->addr_qualifier = (uint8_t)(d[0] & 0x07);
    hdr->address = ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
}

int uat_downlink_decode(const uint8_t frame[UAT_DL_LONG_FRAME_BYTES],
                        uint8_t data_out[UAT_DL_LONG_DATA_BYTES],
                        uat_dl_hdr_t *hdr, uint8_t *rs_corrected)
{
    if (!frame || !data_out)
        return 0;
    if (all_zero(frame, UAT_DL_LONG_FRAME_BYTES))
        return 0; /* 同上行：先于 RS 的全零闸（事实卡 §8） */

    /* 先长后短（事实卡 §2 接受判据）。两次尝试各用独立副本——
     * rs_decode_block 纠后复核失败时数据可能已被部分改写，不能
     * 依赖"失败不改数据"（dump978 fec.c:34-35 依赖其解码器保证，
     * 本实现改用显式重拷获得同一语义）。 */
    uint8_t block[UAT_DL_LONG_FRAME_BYTES];

    memcpy(block, frame, sizeof block);
    int corrected = rs_decode_block(block, 48, 14);
    if (corrected >= 0 && corrected <= 7 && (block[0] >> 3) != 0) {
        memcpy(data_out, block, UAT_DL_LONG_DATA_BYTES);
        if (hdr)
            dl_hdr_from(block, hdr);
        if (rs_corrected)
            *rs_corrected = (uint8_t)corrected;
        return 2;
    }

    memcpy(block, frame, sizeof block);
    corrected = rs_decode_block(block, 30, 12);
    if (corrected >= 0 && corrected <= 6 && (block[0] >> 3) == 0) {
        memcpy(data_out, block, UAT_DL_SHORT_DATA_BYTES);
        if (hdr)
            dl_hdr_from(block, hdr);
        if (rs_corrected)
            *rs_corrected = (uint8_t)corrected;
        return 1;
    }
    return 0;
}
