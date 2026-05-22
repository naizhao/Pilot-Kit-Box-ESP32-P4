/*
 * settings_page.c — language/settings screen.
 */

#include "settings_page.h"

#include <stdint.h>

#include "display.h"
#include "i18n.h"
#include "pfd_draw.h"
#include "text.h"

#define COL_BG          pk_rgb565( 12,  12,  16)
#define COL_HEADER      pk_rgb565(180, 235, 255)
#define COL_ROW         pk_rgb565( 30,  36,  44)
#define COL_ROW_EDGE    pk_rgb565( 80, 220, 240)
#define COL_KEY         pk_rgb565(180, 235, 255)
#define COL_VAL         pk_rgb565(255, 255, 255)
#define COL_DIM         pk_rgb565(255, 255, 255)
#define COL_DIVIDER     pk_rgb565( 60,  60,  70)

#define SETTINGS_HEADER_TITLE_Y 4
#define SETTINGS_HEADER_UI_Y    6
#define SETTINGS_ROW_TOP       48
#define SETTINGS_ROW_H         38
#define SETTINGS_ROW_TEXT_Y    59
#define SETTINGS_ROW_UI_TEXT_Y 60

void pk_settings_page_render(uint16_t *fb)
{
    pk_lang_t lang = pk_i18n_get_lang();

    pk_pfd_fill_rect(fb, 0, 0, PK_DISPLAY_W, PK_DISPLAY_H, COL_BG);

    if (lang == PK_LANG_ZH) {
        pk_text_puts_page_title(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                6, SETTINGS_HEADER_TITLE_Y,
                                pk_i18n_text(PK_TR_SETTINGS_TITLE),
                                COL_HEADER);
    } else {
        pk_text_puts_ui(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                        6, SETTINGS_HEADER_UI_Y,
                        pk_i18n_text(PK_TR_SETTINGS_TITLE),
                        COL_HEADER);
    }
    pk_pfd_fill_rect(fb, 0, 24, PK_DISPLAY_W, 26, COL_DIVIDER);

    pk_pfd_fill_rect(fb, 10, SETTINGS_ROW_TOP,
                     PK_DISPLAY_W - 10, SETTINGS_ROW_TOP + SETTINGS_ROW_H,
                     COL_ROW);
    pk_pfd_fill_rect(fb, 10, SETTINGS_ROW_TOP,
                     13, SETTINGS_ROW_TOP + SETTINGS_ROW_H,
                     COL_ROW_EDGE);

    if (lang == PK_LANG_ZH) {
        pk_text_puts_page_title(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                22, SETTINGS_ROW_TEXT_Y,
                                pk_i18n_text(PK_TR_SETTINGS_LANGUAGE), COL_KEY);
        pk_text_puts_page_title(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                                182, SETTINGS_ROW_TEXT_Y,
                                pk_i18n_lang_name(lang), COL_VAL);
    } else {
        pk_text_puts_ui(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                        22, SETTINGS_ROW_UI_TEXT_Y,
                        pk_i18n_text(PK_TR_SETTINGS_LANGUAGE), COL_KEY);
        pk_text_puts_ui(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                        182, SETTINGS_ROW_UI_TEXT_Y,
                        pk_i18n_lang_name(lang), COL_VAL);
    }

    pk_pfd_fill_rect(fb, 0, PK_DISPLAY_H - 18, PK_DISPLAY_W, PK_DISPLAY_H - 17,
                     COL_DIVIDER);
    pk_text_puts_page_body(fb, PK_DISPLAY_W, PK_DISPLAY_H,
                           6, PK_DISPLAY_H - 16,
                           pk_i18n_text(PK_TR_SETTINGS_FOOTER), COL_DIM);
}
