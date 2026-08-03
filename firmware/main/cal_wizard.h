/*
 * cal_wizard.h — 磁力计校准向导（画 8 字提示页）。
 *
 * pk_ui_get_mode() == PK_UI_MODE_CAL_WIZARD 时由 pfd.c 分派渲染。进入/退出
 * 都是自动的：BNO085 报 acc=0 连续 PK_CAL_ENTER_MS 才自动进，acc≥2 连续
 * PK_CAL_EXIT_MS 就自动回 PFD（阈值与依据见 pk_cal_advisor.h）。
 *
 * 「自动进」还要同时过另外两道闸（都在 pk_cal_advisor.c 判定，本页不参与）：
 * 只在**地面静止**时才抢页面（滑行/空中一律降级成顶栏图标），且识别出**磁
 * 干扰环境**时连图标都不给——那种地方画 8 字物理上救不回来。
 *
 * 用户也可以自己关——点右下角那枚「稍后再说」。4.3″ 板上**没有 MODE 键**，
 * 这枚按钮是这一页唯一的自有退路（另一条是 FAB → 导航网格，那条本页不拦）。
 * 关掉之后自动重弹会被抑制，直到精度真的**连续**好起来（PK_CAL_REARM_MS）才
 * 重新武装——只等一个固定时长的话，到期时磁环境多半没变，弹一次关一次，循环
 * 并没有断；策略写在 ui_state.c 的 s_cal_advisor 处。
 *
 * 版面（详细坐标与推导见 cal_wizard.c 的文件头）
 * ----
 * - 顶栏：标题「罗盘校准」，字号/位置/分隔线与其余整屏页同一套（pfd_layout.h）
 * - 上半屏：暗色双纽线轮廓 + 3 s 一圈的亮点，示范该怎么动
 * - 中部：两行说明
 * - 下部：0..3 精度数值 + 进度条（低橙 / 中黄 / 高绿，取自诊断页同一组语义色）
 * - 底行：左边页脚提示，右下角「稍后再说」
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 画一帧到 800×480 的逻辑 framebuffer。动画相位取自 esp_timer_get_time()，
 * 本模块除"按钮是否被按住"外不持有状态，连续调用即得平滑运动。 */
void pk_cal_wizard_render(uint16_t *fb);

/*
 * 触摸命中判定（按下的那一帧调一次）。返回 true 表示这一下被本页吃掉。
 * 约定同 pk_traffic_page_touch / pk_about_page_touch：归属由
 * pk_touch_arbiter 在按下那一刻定死，本页不做每帧重判。命中「稍后再说」时
 * 内部会调 pk_ui_cal_wizard_dismiss()。
 */
bool pk_cal_wizard_touch(int x, int y);

/* 松手：清掉按钮的按下态高亮。由 touch_gt911.c 的松手批处理统一调用。 */
void pk_cal_wizard_touch_up(void);
