/* Settings GNSS 行的可见结构合同：
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -I firmware/main \
 *      -o /tmp/test_settings_gnss_row firmware/test/test_settings_gnss_row.c \
 *      && /tmp/test_settings_gnss_row
 */
#include "settings_page.h"

#include <stdio.h>

int main(void)
{
    const pk_settings_antenna_row_layout_t row =
        pk_settings_gnss_row_layout();
    int fail = 0;
    if (row.segment_count != 2) {
        printf("FAIL: GNSS segment count=%u\n", row.segment_count);
        fail++;
    }
    if (row.text_line_count != 1) {
        printf("FAIL: GNSS row still exposes secondary hint line=%u\n",
               row.text_line_count);
        fail++;
    }
    if (!pk_ant_seg_is_onboard(PK_ANT_SEG_ONBOARD) ||
        pk_ant_seg_is_onboard(PK_ANT_SEG_EXTERNAL)) {
        puts("FAIL: GNSS segment mapping regressed");
        fail++;
    }
    puts(fail ? "FAILED" : "OK");
    return fail ? 1 : 0;
}
