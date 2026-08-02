/* 由 firmware/scripts/gen_nav_icons.py 生成，请勿手改。 */
#pragma once

#include <stdint.h>

#define PK_NAVICON_W  68
#define PK_NAVICON_H  68

typedef enum {
    PK_NAVICON_PFD = 0,   /* flight  U+E539 */
    PK_NAVICON_TRF = 1,   /* radar  U+F04E */
    PK_NAVICON_MAP = 2,   /* map  U+E55B */
    PK_NAVICON_LIST = 3,   /* format_list_bulleted  U+E241 */
    PK_NAVICON_SEARCH = 4,   /* search  U+E8B6 */
    PK_NAVICON_REC = 5,   /* history  U+E28E */
    PK_NAVICON_TOOL = 6,   /* handyman  U+F10B */
    PK_NAVICON_DIAG = 7,   /* monitor_heart  U+EAA2 */
    PK_NAVICON_SET = 8,   /* settings  U+E8B8 */
    PK_NAVICON_ABOUT = 9,   /* info  U+E88E */
    PK_NAVICON_LEVEL = 10,   /* straighten  U+E41C */
    PK_NAVICON_COUNT = 11
} pk_navicon_id_t;

extern const uint8_t pk_navicon_bitmap[];
