/* pk_callsign.c — 实现见 pk_callsign.h 的头注。 */
#include "pk_callsign.h"

#include <stdio.h>
#include <string.h>

bool pk_callsign_sanitize(const char *raw, char out[PK_CALLSIGN_LEN])
{
    if (raw == NULL || out == NULL) return false;

    size_t n = 0;
    for (; n < PK_CALLSIGN_LEN - 1 && raw[n] != '\0'; n++) {
        const char c = raw[n];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ';
        if (!ok) return false;
        out[n] = c;
    }
    out[n] = '\0';
    while (n > 0 && out[n - 1] == ' ') out[--n] = '\0';
    return n > 0;
}

void pk_callsign_display(bool have_callsign, const char *callsign,
                         uint32_t icao24, char *out, size_t cap)
{
    if (out == NULL || cap == 0) return;

    if (have_callsign && callsign != NULL && callsign[0] != '\0') {
        size_t n = 0;
        for (; n + 1 < cap && callsign[n] != '\0'; n++) out[n] = callsign[n];
        out[n] = '\0';
        while (n > 0 && out[n - 1] == ' ') out[--n] = '\0';
        if (n > 0) return;
    }
    /* 落到这里说明没有可用呼号。**无论走哪条分支 out 都会被写满**——
     * 调用方全都是把 out 直接交给 pk_aa_puts 的，留一条未初始化路径就是
     * 往屏上打栈内存。见头注里 map_page.c 那个真机 bug。 */
    snprintf(out, cap, "%06lX", (unsigned long)icao24);
}
