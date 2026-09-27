#include "keyboard_numeric.h"

#define GRID_TOP 152
#define COLS 4
#define ROWS 4
#define DISPLAY_W 800
#define DISPLAY_H 480
#define KEY_W (DISPLAY_W / COLS)
#define KEY_H ((DISPLAY_H - GRID_TOP) / ROWS)

static const char s_keys[ROWS][COLS] = {
    {'7', '8', '9', PK_KBD_NUM_DELETE},
    {'4', '5', '6', PK_KBD_NUM_NONE},
    {'1', '2', '3', PK_KBD_NUM_NONE},
    {'.', '0', PK_KBD_NUM_NONE, PK_KBD_NUM_OK},
};

char pk_keyboard_numeric_key_at(int x, int y)
{
    if (x < 0 || x >= DISPLAY_W || y < GRID_TOP || y >= DISPLAY_H)
        return PK_KBD_NUM_NONE;
    const int row = (y - GRID_TOP) / KEY_H;
    const int col = x / KEY_W;
    if (row < 0 || row >= ROWS || col < 0 || col >= COLS)
        return PK_KBD_NUM_NONE;
    return s_keys[row][col];
}
