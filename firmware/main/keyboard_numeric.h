#pragma once

/* 数字键盘的纯命中值；普通数字和小数点直接返回 ASCII。 */
#define PK_KBD_NUM_NONE    '\0'
#define PK_KBD_NUM_DELETE  '\b'
#define PK_KBD_NUM_OK      '\n'

char pk_keyboard_numeric_key_at(int x, int y);
