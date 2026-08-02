/* 由 firmware/scripts/gen_pfd_aa_font.py 生成，请勿手改。 */
#pragma once

#include <stdint.h>

#define PK_AA_FIRST_CODE  0x20

#define PK_AA_XS_W     10
#define PK_AA_XS_H     17
#define PK_AA_XS_LAST  0x7F
#define PK_AA_S_W     11
#define PK_AA_S_H     20
#define PK_AA_S_LAST  0x7F
#define PK_AA_M_W     15
#define PK_AA_M_H     26
#define PK_AA_M_LAST  0x7F
#define PK_AA_L_W     21
#define PK_AA_L_H     37
#define PK_AA_L_LAST  0x7F
#define PK_AA_XL_W     37
#define PK_AA_XL_H     64
#define PK_AA_XL_LAST  0x3F

extern const uint8_t pk_aa_xs[];
extern const uint8_t pk_aa_s[];
extern const uint8_t pk_aa_m[];
extern const uint8_t pk_aa_l[];
extern const uint8_t pk_aa_xl[];

#define PK_AA_CJK_COUNT  324
#define PK_AA_XS_CJK_W  15
#define PK_AA_XS_CJK_H  17
#define PK_AA_S_CJK_W  17
#define PK_AA_S_CJK_H  20
#define PK_AA_M_CJK_W  22
#define PK_AA_M_CJK_H  26
#define PK_AA_L_CJK_W  32
#define PK_AA_L_CJK_H  37

extern const uint16_t pk_aa_cjk_codes[];
extern const uint8_t pk_aa_xs_cjk[];
extern const uint8_t pk_aa_s_cjk[];
extern const uint8_t pk_aa_m_cjk[];
extern const uint8_t pk_aa_l_cjk[];
