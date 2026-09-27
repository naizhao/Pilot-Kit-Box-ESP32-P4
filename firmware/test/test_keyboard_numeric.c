/* 数字频率键盘的纯命中模型：
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 *      -o /tmp/test_keyboard_numeric \
 *      firmware/test/test_keyboard_numeric.c firmware/main/keyboard_numeric.c \
 *      && /tmp/test_keyboard_numeric
 */
#include "keyboard_numeric.h"

#include <stdio.h>

static int fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); fail++; } } while (0)

static void test_large_numeric_grid_exposes_frequency_characters_and_actions(void)
{
    CHECK(pk_keyboard_numeric_key_at(100, 170) == '7', "top-left is not 7\n");
    CHECK(pk_keyboard_numeric_key_at(300, 170) == '8', "top-middle is not 8\n");
    CHECK(pk_keyboard_numeric_key_at(700, 170) == PK_KBD_NUM_DELETE,
          "top-right is not delete\n");
    CHECK(pk_keyboard_numeric_key_at(100, 420) == '.', "bottom-left is not dot\n");
    CHECK(pk_keyboard_numeric_key_at(300, 420) == '0', "bottom-middle is not 0\n");
    CHECK(pk_keyboard_numeric_key_at(700, 420) == PK_KBD_NUM_OK,
          "bottom-right is not OK\n");
}

static void test_header_and_outside_grid_do_not_create_digits(void)
{
    CHECK(pk_keyboard_numeric_key_at(100, 100) == PK_KBD_NUM_NONE,
          "input/header area generated a key\n");
    CHECK(pk_keyboard_numeric_key_at(-1, 200) == PK_KBD_NUM_NONE,
          "negative coordinate generated a key\n");
    CHECK(pk_keyboard_numeric_key_at(800, 200) == PK_KBD_NUM_NONE,
          "right edge generated a key\n");
}

int main(void)
{
    test_large_numeric_grid_exposes_frequency_characters_and_actions();
    test_header_and_outside_grid_do_not_create_digits();
    puts(fail ? "FAILED" : "OK");
    return fail ? 1 : 0;
}
