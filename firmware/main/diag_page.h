#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PK_DIAG_CARD_COLS 2
#define PK_DIAG_CARD_N 15
#define PK_DIAG_CARD_AIRBAND 14

#define PK_DIAG_AIRBAND_BTN_X0 48
#define PK_DIAG_AIRBAND_BTN_W 340
#define PK_DIAG_AIRBAND_BTN_GAP 24
#define PK_DIAG_DETAIL_LINE_H 38

typedef enum {
    PK_DIAG_AIRBAND_ACT_NONE = 0,
    PK_DIAG_AIRBAND_ACT_FREQ_GUARD,
    PK_DIAG_AIRBAND_ACT_FREQ_AIR_AIR,
    PK_DIAG_AIRBAND_ACT_SCAN_TOGGLE,
    PK_DIAG_AIRBAND_ACT_SQUELCH_TOGGLE,
    PK_DIAG_AIRBAND_ACT_SQUELCH_DEC,
    PK_DIAG_AIRBAND_ACT_SQUELCH_INC,
    PK_DIAG_AIRBAND_ACT_SPEAKER_TEST,
} pk_diag_airband_action_t;

typedef enum {
    PK_DIAG_AIRBAND_CMD_NONE = 0,
    PK_DIAG_AIRBAND_CMD_SET_FREQUENCY,
    PK_DIAG_AIRBAND_CMD_START_SCAN,
    PK_DIAG_AIRBAND_CMD_STOP_SCAN,
    PK_DIAG_AIRBAND_CMD_SET_SQUELCH,
    PK_DIAG_AIRBAND_CMD_SPEAKER_TEST,
} pk_diag_airband_cmd_kind_t;

typedef struct {
    bool scanning;
    bool squelch_enabled;
    int16_t squelch_open_dbm_x2;
} pk_diag_airband_control_state_t;

typedef struct {
    pk_diag_airband_cmd_kind_t kind;
    uint32_t frequency_hz;
    uint32_t scan_start_hz;
    uint32_t scan_end_hz;
    uint32_t scan_step_hz;
    uint32_t scan_dwell_ms;
    bool squelch_enabled;
    int16_t squelch_open_dbm_x2;
} pk_diag_airband_command_t;

/* 总览卡片与 AIRBAND 详情控制的纯命中/命令模型，供真机与 host 测试共用。 */
int pk_diag_card_index_for_cell(int row, int col);
pk_diag_airband_action_t pk_diag_airband_action_at(int x, int content_y);
bool pk_diag_airband_make_command(pk_diag_airband_action_t action,
                                  const pk_diag_airband_control_state_t *state,
                                  pk_diag_airband_command_t *out);
void pk_diag_page_render(uint16_t *fb);

/* 触摸：拖动滚动卡片区。约定同 pk_adsb_list_*——返回 true 表示这一下被本页
 * 消费，调用方不应再转给别的控件。dock 展开时由 read_cb 统一让路。 */
bool pk_diag_page_touch(int x, int y);
bool pk_diag_page_drag(int x, int y);
void pk_diag_page_touch_up(void);
void pk_diag_page_touch_cancel(void);

/* 退出子系统详情回到总览。三条退路（顶栏返回 / FAB / 右滑）共用。 */
void pk_diag_page_leave_detail(void);
bool pk_diag_page_in_detail(void);
