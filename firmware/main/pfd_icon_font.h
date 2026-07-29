/* 由 firmware/scripts/gen_pfd_icons.py 生成，请勿手改。
 *
 * 图标取自 Material Symbols Rounded（Google，Apache-2.0）。
 * 4bpp 灰度，固定 cell，与文字共用同一套 alpha 混合渲染。
 */
#pragma once

#include <stdint.h>

#define PK_ICON_W  30
#define PK_ICON_H  30

typedef enum {
    PK_ICON_SAT = 0,   /* satellite_alt  U+EB3A  FILL=1 */
    PK_ICON_REC = 1,   /* fiber_manual_record  U+E061  FILL=1 */
    PK_ICON_WARN = 2,   /* warning  U+E002  FILL=1 */
    PK_ICON_TEMP = 3,   /* thermometer_alert  U+FFFFB  FILL=1 */
    PK_ICON_BLE = 4,   /* bluetooth  U+E1A7  FILL=1 */
    PK_ICON_SD = 5,   /* sd_card  U+E1C2  FILL=1 */
    PK_ICON_ADSB = 6,   /* connecting_airports  U+E7C9  FILL=1 */
    PK_ICON_OWNSHIP = 7,   /* flight  U+E539  FILL=1 */
    PK_ICON_NAV_HDG = 8,   /* near_me  U+E569  FILL=1 */
    PK_ICON_NAV_NORTH = 9,   /* navigation  U+E55D  FILL=1 */
    PK_ICON_BATT_ALERT = 10,   /* battery_android_alert  U+F306  FILL=0 */
    PK_ICON_BATT_0 = 11,   /* battery_android_0  U+F30D  FILL=0 */
    PK_ICON_BATT_1 = 12,   /* battery_android_1  U+F30C  FILL=0 */
    PK_ICON_BATT_2 = 13,   /* battery_android_2  U+F30B  FILL=0 */
    PK_ICON_BATT_3 = 14,   /* battery_android_3  U+F30A  FILL=0 */
    PK_ICON_BATT_4 = 15,   /* battery_android_4  U+F309  FILL=0 */
    PK_ICON_BATT_5 = 16,   /* battery_android_5  U+F308  FILL=0 */
    PK_ICON_BATT_6 = 17,   /* battery_android_6  U+F307  FILL=0 */
    PK_ICON_BATT_FULL = 18,   /* battery_android_full  U+F304  FILL=0 */
    PK_ICON_BATT_CHG_1 = 19,   /* battery_android_frame_1  U+F257  FILL=0 */
    PK_ICON_BATT_CHG_2 = 20,   /* battery_android_frame_2  U+F256  FILL=0 */
    PK_ICON_BATT_CHG_3 = 21,   /* battery_android_frame_3  U+F255  FILL=0 */
    PK_ICON_BATT_CHG_4 = 22,   /* battery_android_frame_4  U+F254  FILL=0 */
    PK_ICON_BATT_CHG_5 = 23,   /* battery_android_frame_5  U+F253  FILL=0 */
    PK_ICON_BATT_CHG_6 = 24,   /* battery_android_frame_6  U+F252  FILL=0 */
    PK_ICON_BATT_CHG_FULL = 25,   /* battery_android_frame_full  U+F24F  FILL=0 */
    PK_ICON_COUNT = 26
} pk_icon_id_t;

/* [0] = regular, [1] = bold —— 与文字字重联动。 */
extern const uint8_t *const pk_icon_bitmap[2];
