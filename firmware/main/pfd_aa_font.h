/* 由 firmware/scripts/gen_pfd_aa_font.py 生成，请勿手改。 */
#pragma once

#include <stdint.h>

#define PK_AA_FIRST_CODE  0x20

#define PK_AA_XS_W     15
#define PK_AA_XS_H     26
#define PK_AA_XS_LAST  0x3F
#define PK_AA_S_W     18
#define PK_AA_S_H     30
#define PK_AA_S_LAST  0x7F
#define PK_AA_M_W     24
#define PK_AA_M_H     40
#define PK_AA_M_LAST  0x7F
#define PK_AA_XL_W     37
#define PK_AA_XL_H     64
#define PK_AA_XL_LAST  0x3F

extern const uint8_t pk_aa_xs_regular[];
extern const uint8_t pk_aa_xs_bold[];
extern const uint8_t pk_aa_s_regular[];
extern const uint8_t pk_aa_s_bold[];
extern const uint8_t pk_aa_m_regular[];
extern const uint8_t pk_aa_m_bold[];
extern const uint8_t pk_aa_xl_regular[];
extern const uint8_t pk_aa_xl_bold[];
