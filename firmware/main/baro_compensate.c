/*
 * baro_compensate.c — BMP388 补偿数学的纯单元实现。
 *
 * 公式从 baro_task.c 原样搬移（WP-B Task 5，行为保持）：出处
 * BMP388 数据手册 BST-BMP388-DS001-07 §9.1–§9.3 附录参考实现。
 * 本文件不得 include 任何 ESP-IDF 头——host 单测
 * firmware/test/test_baro_compensate.c 直接编它。
 */
#include "baro_compensate.h"

/* ─────────────────────────────────────────────────────────────────────── */
/*  Bosch BMP3_FLOAT 补偿公式(照原文一字不改)                               */
/* ─────────────────────────────────────────────────────────────────────── */

float baro_compensate_temperature(baro_calib_t *calib, uint32_t uncomp_temp)
{
    float pd1 = (float)uncomp_temp - calib->t1;
    float pd2 = pd1 * calib->t2;
    calib->t_lin = pd2 + (pd1 * pd1) * calib->t3;
    return calib->t_lin;
}

float baro_compensate_pressure(const baro_calib_t *calib, uint32_t uncomp_press)
{
    float t   = calib->t_lin;
    float po1 = calib->p5 + calib->p6 * t + calib->p7 * (t * t) + calib->p8 * (t * t * t);
    float po2 = (float)uncomp_press * (calib->p1 + calib->p2 * t + calib->p3 * (t * t) + calib->p4 * (t * t * t));
    float up2 = (float)uncomp_press * (float)uncomp_press;
    float po3 = up2 * (calib->p9 + calib->p10 * t) + (up2 * (float)uncomp_press) * calib->p11;
    return po1 + po2 + po3;   /* Pa */
}

void baro_calib_parse(const uint8_t c[21], baro_calib_t *out)
{
    uint16_t T1 = (uint16_t)((c[1] << 8) | c[0]);
    uint16_t T2 = (uint16_t)((c[3] << 8) | c[2]);
    int8_t   T3 = (int8_t)c[4];
    int16_t  P1 = (int16_t)((c[6] << 8) | c[5]);
    int16_t  P2 = (int16_t)((c[8] << 8) | c[7]);
    int8_t   P3 = (int8_t)c[9];
    int8_t   P4 = (int8_t)c[10];
    uint16_t P5 = (uint16_t)((c[12] << 8) | c[11]);
    uint16_t P6 = (uint16_t)((c[14] << 8) | c[13]);
    int8_t   P7 = (int8_t)c[15];
    int8_t   P8 = (int8_t)c[16];
    int16_t  P9 = (int16_t)((c[18] << 8) | c[17]);
    int8_t   P10 = (int8_t)c[19];
    int8_t   P11 = (int8_t)c[20];

    out->t1  = (float)T1  / 0.00390625f;              /* 2^-8  */
    out->t2  = (float)T2  / 1073741824.0f;             /* 2^30  */
    out->t3  = (float)T3  / 281474976710656.0f;         /* 2^48  */
    out->p1  = ((float)P1  - 16384.0f) / 1048576.0f;   /* 2^20  */
    out->p2  = ((float)P2  - 16384.0f) / 536870912.0f; /* 2^29  */
    out->p3  = (float)P3  / 4294967296.0f;              /* 2^32  */
    out->p4  = (float)P4  / 137438953472.0f;            /* 2^37  */
    out->p5  = (float)P5  / 0.125f;                     /* 2^-3  */
    out->p6  = (float)P6  / 64.0f;                      /* 2^6   */
    out->p7  = (float)P7  / 256.0f;                     /* 2^8   */
    out->p8  = (float)P8  / 32768.0f;                   /* 2^15  */
    out->p9  = (float)P9  / 281474976710656.0f;          /* 2^48  */
    out->p10 = (float)P10 / 281474976710656.0f;          /* 2^48  */
    out->p11 = (float)P11 / 36893488147419103232.0f;    /* 2^65  */
    out->t_lin = 0.0f;
}
