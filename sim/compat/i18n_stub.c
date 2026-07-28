/*
 * i18n_stub.c — 模拟器侧的 i18n 桩。
 *
 * 固件的 i18n.c 依赖 NVS（语言选择要持久化），PC 上没有。但 i18n_catalog.c
 * 是纯数据表，可以直接编译——于是这里只补一个 pk_i18n_text()，语言由环境
 * 变量 PK_SIM_LANG 决定（默认中文）。
 *
 * 这样 pk_ui_nav.c 无需为模拟器留任何分支：它照常调 pk_i18n_text()，两边
 * 编的是同一份源码。
 */

#include <stdbool.h>
#include <stdlib.h>

#include "i18n.h"
#include "i18n_catalog.h"

const char *pk_i18n_text(pk_tr_id_t id)
{
    const char *lang = getenv("PK_SIM_LANG");
    bool en = lang && (lang[0] == 'e' || lang[0] == 'E');
    return pk_i18n_catalog_text(en ? PK_LANG_EN : PK_LANG_ZH, id);
}

const char *pk_i18n_lang_name(pk_lang_t lang)
{
    return pk_i18n_catalog_text(lang, PK_TR_LANG_ENGLISH);
}
