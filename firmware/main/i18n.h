/*
 * i18n.h — built-in UI language selection and text lookup.
 *
 * The catalog is generated from firmware/scripts/i18n_catalog.py. This
 * wrapper owns the active language and NVS persistence so render code only
 * asks for pk_i18n_text(id).
 */
#pragma once

#include "esp_err.h"
#include "i18n_catalog.h"

esp_err_t pk_i18n_init(void);

pk_lang_t pk_i18n_get_lang(void);
esp_err_t pk_i18n_set_lang(pk_lang_t lang);
esp_err_t pk_i18n_toggle_lang(void);

const char *pk_i18n_text(pk_tr_id_t id);
const char *pk_i18n_lang_name(pk_lang_t lang);
