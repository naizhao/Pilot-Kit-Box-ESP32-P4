/*
 * logo_blob.c — logo 位图的固件侧取数（见 logo_blob.h）。
 */
#include "logo_blob.h"

extern const uint8_t pk_logo_blob_start[] asm("_binary_pk_logo_rgb565_start");
extern const uint8_t pk_logo_blob_end[]   asm("_binary_pk_logo_rgb565_end");

#define PK_LOGO_SRC_W 160
#define PK_LOGO_SRC_H 160

const uint16_t *pk_logo_rgb565(int *w, int *h)
{
    if (w) *w = PK_LOGO_SRC_W;
    if (h) *h = PK_LOGO_SRC_H;

    /* 尺寸对不上就当没有——宁可不画，也不要按错误的 stride 读越界。 */
    if ((pk_logo_blob_end - pk_logo_blob_start) <
        (long)(PK_LOGO_SRC_W * PK_LOGO_SRC_H * 2)) {
        return NULL;
    }
    return (const uint16_t *)pk_logo_blob_start;
}
