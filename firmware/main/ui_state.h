/*
 * ui_state.h — single source of truth for which view the LCD is showing.
 *
 * Top-level LCD views:
 *   PK_UI_MODE_PFD        — primary flight display (default at boot)
 *   PK_UI_MODE_ADSB_LIST  — scrollable list of currently-tracked ADS-B
 *                            aircraft + detail pane of the selected row
 *   PK_UI_MODE_SETTINGS   — user settings, including active language
 *   PK_UI_MODE_ABOUT      — project name, version, build time, hardware
 *                            summary, calibration quality
 *   PK_UI_MODE_CAL_WIZARD — figure-8 calibration overlay (auto-entered
 *                            when the BNO085 magnetometer fusion has
 *                            been stuck at acc=0 for too long; auto-
 *                            exits when acc reaches 2 and stays there)
 *
 * The render task (pfd_task in firmware/main/pfd.c) checks
 * pk_ui_get_mode() once per frame and dispatches to the correct
 * renderer. Mode transitions happen in O(1) — just a flag flip — so
 * the next frame already shows the new view.
 *
 * Mode is cycled by the touch UI (e.g. a long-press gesture on the
 * screen) through the USER-visible modes:
 *     PFD → TRAFFIC → MAP → ADSB_LIST → SETTINGS → ABOUT → DIAG → PFD …
 * CAL_WIZARD is not in the cycle — it's auto-entered/auto-exited
 * based on IMU calibration state (see pk_ui_cal_wizard_tick below)
 * and the user can also dismiss it manually via touch.
 *
 * Threading
 * ---------
 * Multiple producers/consumers (touch UI + render task + IMU
 * task + future BLE task) read and update mode + selection.
 * Everything goes through a small mutex; calls are short and
 * non-blocking so it's never meaningfully contended.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "i18n_catalog.h"     /* pk_tr_id_t — toast 提示按翻译条目 id 记录 */
#include "pk_flight_phase.h"  /* pk_flight_phase_t — 校准提示的相位门控 */

typedef enum {
    PK_UI_MODE_PFD = 0,
    PK_UI_MODE_TRAFFIC,     /* 360° 交通雷达页(本机居中,目标按方位/距离) */
    PK_UI_MODE_MAP,         /* SD 离线地图页(PMTiles 栅格底图 + ADS-B 叠加) */
    PK_UI_MODE_ADSB_LIST,
    PK_UI_MODE_SETTINGS,
    PK_UI_MODE_ABOUT,
    PK_UI_MODE_DIAG,        /* 硬件诊断:各子系统在线状态 + 实时数值 */
    PK_UI_MODE_CAL_WIZARD,
} pk_ui_mode_t;

/* Initialise the UI state mutex. Must be called once before the
 * render task starts. */
esp_err_t pk_ui_init(void);

/* Get the current top-level view. Safe from any task. */
pk_ui_mode_t pk_ui_get_mode(void);

/* Cycle the user-visible mode forward:
 *     PFD → TRAFFIC → ADSB_LIST → SETTINGS → ABOUT → DIAG → PFD …
 * Selection is preserved across toggles (re-entering list mode
 * lands on the same highlight). If the current mode is
 * CAL_WIZARD, the cycle returns to PFD and dismisses the wizard. */
void pk_ui_toggle_mode(void);

/* Jump directly to a specific view (bypasses the cycle). Used when an
 * action implies a destination — e.g. binding own-ship in the ADS-B
 * list returns to PFD so the pilot immediately sees the caged horizon
 * sourced from the freshly-bound transponder. Safe from any task. */
void pk_ui_set_mode(pk_ui_mode_t mode);

/*
 * 驱动校准向导的自动进入/退出。渲染任务每帧调一次（pfd.c），传最近一份 IMU
 * 样本的 accuracy(0..3)、样本是否有效、本机当前飞行相位
 * （pk_own_sampler_get_phase()），以及 IMU 振动强度 vib_level
 * （pk_imu_sample_t.vib_level，pk_vib.c 的加速度模长 RMS）。
 *
 * 判定本身全部在 pk_cal_advisor（纯状态机，host 有单测），本函数只把它的结论
 * 落成切页。**五段判据**，配套缺一不可，缺一条就会退回骚扰循环：
 *
 *   1. 重新武装滞回 —— 「稍后再说」关上的闸门，要等 accuracy **连续保持**在
 *      阈值之上足够久才解除。旧实现是单帧 acc≥2 就解除，而机坪上这个信号本
 *      来就在 0↔2 之间抖，等于一帧噪声作废用户的意图。
 *   2. 飞行相位门控 —— 只有地面静止（含 UNKNOWN，覆盖"刚开机 GPS 还没定位"）
 *      才允许抢页面；滑行/起飞/空中/落地滑跑一律降级成状态栏图标。飞行中把
 *      PFD 换成校准向导是安全问题，不是骚扰问题。
 *   3. 磁干扰识别 —— accuracy 在滑动窗口内反复跨阈说明是磁环境在变，这种地方
 *      画 8 字物理上救不回来，一律闭嘴（连图标都不给，见 pk_ui_cal_jammed）。
 *   4. 静止门控 —— 磁力计校准物理上需要转动，vib_level 低于阈值 = 设备没在动
 *      → 画 8 字没用，降级 HINT（图标照给）。vib_level==0 是「不可用」不是
 *      「零振动」（pk_vib.h 明确警告过），不可用时不抑制弹页。
 *   5. 冷启动宽限 —— 本次开机 acc 从未到过 EXIT_ACCURACY → ENTER 窗口临时拉长
 *      到 120 s（实测冷启动自然收敛要 29~30 s，20 s 窗口会在它自己好之前弹窗）。
 *      收敛过一次后永久走 20 s 快通道。
 *
 * IMU 断流（valid=false）时不抢页面：读不到姿态的板子弹出一页让人画 8 字毫无
 * 意义。这一条挡在 ui_state.c 的胶水层，理由写在那边。
 *
 * 阈值取值与每个数的依据见 pk_cal_advisor.h。
 */
void pk_ui_cal_wizard_tick(bool valid, uint8_t accuracy, pk_flight_phase_t phase,
                           uint8_t vib_level);

/*
 * 用户主动关掉校准页（页内那枚「稍后再说」）。切回 PFD，并关上自动重弹的
 * 闸门。4.3″ 板上没有 MODE 键，这是页内唯一的退路；FAB → 导航网格那条走
 * pk_ui_set_mode()，同样会关闸门。
 */
void pk_ui_cal_wizard_dismiss(void);

/*
 * 用户主动**打开**校准页（设置页那一行「罗盘校准」）。
 *
 * 与直接 pk_ui_set_mode(PK_UI_MODE_CAL_WIZARD) 的差别是它顺手把自动重弹的
 * 闸门重新武装。上面那个 dismiss 关掉闸门之后，本次开机内自动弹窗就不再出现
 * （要等 accuracy 真的上到 ≥2 才复位），主动来校准的人必须能把它开回去，
 * 否则"关一次就永远进不来"。策略与取舍见 ui_state.c 里这个函数的注释。
 */
void pk_ui_cal_wizard_enter(void);

/* Read the current target accuracy bar used by the wizard renderer.
 * Returns 0..3. Stable across reads — updated only when the render task
 * calls pk_ui_cal_wizard_tick(). */
uint8_t pk_ui_cal_wizard_last_accuracy(void);

/*
 * 当前该不该在状态栏画那枚罗盘告警图标（阶段 C3）。
 *
 * true 只出现在「精度确实不够，但这一刻不该抢页面」的时候：闸门关着（用户按
 * 过「稍后再说」）、或不在地面静止。它表达的是"信息还在，只是不打扰你"——
 * 用户按下「稍后再说」说的是"别抢我的页面"，不是"别再告诉我"。
 *
 * 判定为磁干扰环境时恒为 false：那种地方画 8 字救不回来，图标只会让用户以为
 * 设备坏了而反复去转它（想知道"为什么什么都不提示"的人看 pk_ui_cal_jammed）。
 */
bool pk_ui_cal_hint_active(void);

/*
 * 当前是否判定为强磁干扰环境（诊断页用，阶段 C4）。
 *
 * jammed 时设备对校准这件事**什么都不提示**——既不弹页也不给图标。若这个结论
 * 无处可查，一台在机坪上安静的盒子看起来就像坏了，而排查线索一条都没有。
 */
bool pk_ui_cal_jammed(void);

/* Move the list selection by `delta` rows (negative = up, positive =
 * down). The scroll intent is buffered as a pending delta and applied
 * by the next pk_ui_list_resolve_row() call, which knows the live
 * snapshot. Saturates the pending delta in the range [-999, +999] so
 * holding UP/DOWN forever can't overflow. */
void pk_ui_list_scroll(int delta);

/* Scroll the About page by one coarse page step (negative = up,
 * positive = down). The renderer reads the pixel offset via
 * pk_ui_about_scroll_y(). */
void pk_ui_about_scroll(int delta);
int  pk_ui_about_scroll_y(void);

/* Scroll the Diagnostics page by one coarse page step (mirrors the
 * About-page scroll; renderer reads the pixel offset via
 * pk_ui_diag_scroll_y()). */
void pk_ui_diag_scroll(int delta);
int  pk_ui_diag_scroll_y(void);

/*
 * Resolve the highlighted row against the current aircraft snapshot.
 *
 * The list renderer calls this once per frame, passing the sorted
 * ICAO array from aircraft_state_snapshot(). The function:
 *   1. finds the row currently occupied by the previously-selected
 *      ICAO (0 if no prior selection or the aircraft has expired),
 *   2. adds any pending scroll delta accumulated by pk_ui_list_scroll
 *      (the delta is cleared atomically inside the call),
 *   3. clamps to [0, n-1],
 *   4. commits the ICAO at the new row as the new selection so a
 *      future call after a snapshot reshuffle still tracks the same
 *      aircraft,
 *   5. returns the new row index.
 *
 * For n == 0 the call returns 0, leaves the pending delta intact, and
 * does not touch the saved ICAO — so an empty-list refresh doesn't
 * silently swallow a press the user made while no aircraft was tracked.
 */
int pk_ui_list_resolve_row(const uint32_t *icaos, size_t n);

/*
 * Traffic 雷达页专用的选中解析。与列表选中(s_list_selected_icao)完全独立,
 * 只共用 pending 滚动量。返回选中行索引,或 **-1 表示"当前无选中"**(用户从
 * 没滚动过,或之前选中的飞机已离开列表)——绝不像列表版那样 fallback 到 row 0,
 * 因此本机被排除出目标列表也不会引起每帧乱跳。
 */
int pk_ui_traffic_resolve(const uint32_t *icaos, size_t n);

/* The ICAO of the currently-highlighted aircraft, or 0 if none has
 * been committed yet (no aircraft seen since boot, or the user hasn't
 * scrolled). Used by the TARE handler to bind own-ship by ICAO
 * directly, sidestepping any race against a re-snapshot. */
uint32_t pk_ui_list_get_selected_icao(void);

/*
 * Runtime own-ship binding — which ADS-B aircraft drives the PFD's
 * ALT / VS / GS readouts. Volatile: lives in RAM only, cleared on
 * reboot (no NVS write). The PFD reads via pk_ui_get_own_icao(); when
 * the runtime value has never been set (or is 0), the getter falls
 * back to the compile-time CONFIG_PK_OWN_ICAO default.
 *
 * Set this from the TARE short-press handler when the user is in
 * PK_UI_MODE_ADSB_LIST — that's the gesture the kit exposes for
 * "this highlighted aircraft is me". Re-pressing TARE on another
 * aircraft replaces the binding; a power cycle wipes it.
 */
void     pk_ui_set_own_icao(uint32_t icao24);
uint32_t pk_ui_get_own_icao(void);

/* Clear the runtime own-ship binding (de-select). Equivalent to binding
 * 0 — pk_ui_get_own_icao() returns 0 and the PFD's ALT/VS/GS revert to
 * "--". Symmetric with pk_ui_set_own_icao(); the gesture is re-pressing
 * TARE on the already-bound aircraft in the ADS-B list. */
void     pk_ui_clear_own_icao(void);

/*
 * Transient on-screen toast. The UI calls pk_ui_toast_show()
 * with the translation id to display (localised at render time, so it
 * follows the active language) and an error flag (true → red banner,
 * false → green). The PFD render loop polls pk_ui_toast_get() once per
 * frame and overlays the banner on top of whichever page is showing
 * until the ~1.5 s window elapses.
 */
void pk_ui_toast_show(pk_tr_id_t id, bool is_error);
bool pk_ui_toast_get(pk_tr_id_t *out_id, bool *out_error);

/*
 * 同 pk_ui_toast_show()，多一个"闪 N 次"的强调模式（SD 写失败 / 降级告警
 * 用，阶段 5b：ADS-B 数据持久化设计（内部文档）
 * 「告警呈现」节，评审要求"复用现有实现、闪 3 次、不阻断飞行"）。
 *
 * blink_times<=0 等价于 pk_ui_toast_show()：不闪、固定 1.5 s。blink_times>0
 * 时时长改为 blink_times × 800 ms（400 ms 一拍，一亮一灭算一次"闪"）——
 * 不能沿用固定 1.5 s：3 次闪需要 2.4 s，toast 会在闪完前就被 1.5 s 的老
 * 生命周期收起。闪烁相位由 pk_ui_toast_blink_visible() 逐帧给出，
 * pk_ui_toast_get() 本身只管"整体是否还没过期"，不管闪烁——这样闪烁开关
 * 出 bug 最坏是"常亮"而不是"提前消失"。
 */
void pk_ui_toast_show_blink(pk_tr_id_t id, bool is_error, int blink_times);

/* 本帧闪烁相位是否该"亮"。非闪烁 toast（blink_times<=0）恒真；toast 未激活
 * 时也返回 true（调用方应先用 pk_ui_toast_get() 判断是否激活，两者是
 * "与"的关系，不是这个函数自己判断是否显示）。渲染层每帧调用一次，与
 * pk_ui_toast_get() 配合决定这一帧要不要画 toast。 */
bool pk_ui_toast_blink_visible(void);
