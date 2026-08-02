/*
 * apt_detail_page.h — 机场详情页（跑道 / 频率）。
 *
 * 形态：**模态层，不是 pk_ui_mode_t**
 * -----------------------------------
 * 与 search_page / keyboard_page 同一套（设计文档 §3.1 那两条硬约束照旧成立：
 * dock 第 8 个页签算出来 852 px > 800 px 会溢出；pk_ui_nav_on_back() 硬编码
 * 回 DIAG）。做成模态层只要改 render/touch 两处分派 + 新增本文件。
 *
 * 三层导航是怎么解决的
 * --------------------
 * 设计文档 §1.3 把「搜索 → 结果 → 详情是三层，而导航只有两层」列为拦路虎，
 * D3 因此把详情推到二期。真做起来发现**那个障碍不存在**：
 *
 *   模态层是"盖在上面"，不是"切过去"。底下那一页（地图）与中间那一层
 *   （搜索）从来没有被关掉，只是被盖住；关掉最上面一层，露出来的自然就是
 *   来时的地方。返回目标不需要一个栈来记——它就是**分派次序**本身：
 *
 *       键盘 > 详情 > 搜索 > 各 pk_ui_mode_t
 *
 *   从搜索点进详情：搜索仍 active → 关详情 → 分派落到搜索。回到了搜索。
 *   从地图点进详情：搜索没 active → 关详情 → 分派落到 mode=MAP。回到了地图。
 *
 * 于是 pk_ui_nav_on_back() 一行都不用改（它服务的是 backbar / FAB / 右滑
 * 这三条**二级页面**退路，而模态层期间 FAB 是藏起来的、dock 也收着，那三条
 * 路根本走不到）。真正需要显式记住来路的只有两件事，用 opener 一个枚举带过：
 *   - 关页时 FAB 该不该放出来（搜索还开着就得继续藏）；
 *   - 「在地图上显示」要把详情和搜索**一起**关掉，而不是退回搜索。
 *
 * 为什么详情排在搜索**之上**：它是从搜索里打开的，反过来不成立（同"键盘在
 * 搜索之上"的理由）。两处分派（pfd.c 的渲染、touch_gt911.c 的触摸）次序必须
 * 一致——"看得见的就是点得中的"全靠这一条。
 *
 * 数据取用
 * --------
 * 打开的那一刻把要显示的全部内容 snprintf 进本页自己的缓冲，之后每帧只读
 * 缓冲。两条理由，都是硬的：
 *   - 记录里的 name/city/callsign 是指向 PSRAM 池的裸指针，**拔卡即悬空**
 *     （pk_aero_db.h:23-25），跨帧留指针就是等着读 free 掉的内存；
 *   - 每帧重查 = 每帧持一次库锁，而这一页最多要读 1+24+64 = 89 条记录。
 * 89 次记录读全是 µs 级（by_icao / 聚簇区间 / *_get 都是 6–10 µs，
 * pk_aero_db.h:14-15），一次打开总共不到 1 ms，落在用户已经在等页面变化的
 * 那一帧上——与 search_page 把 NVS 写在触摸路径上是同一笔账。
 * **绝不在这里调 nearest 系列**（那才是毫秒级的那一类）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 跑道方向条数上限。真库里最多的是 ZGGG 一类的大场（10 条方向 = 5 条跑道），
 * 24 留了一倍余量；超出的静默丢弃——屏上滚到第 24 条时早已不是"看一眼频率"
 * 这个场景了。 */
#define PK_APT_DETAIL_RWY_MAX   24

/* 频率条数上限。**63 是真实数据**（ZGGG，KJFK 43），所以这个数不能按"够看
 * 就行"来定：截断会让飞行员以为塔台频率不存在。64 是把已知最大值兜住之后
 * 再取的最近的 2 的幂。 */
#define PK_APT_DETAIL_FREQ_MAX  64

/* 机场名/城市的显示预算（含 NUL）。名称行从 x=16 画到 784 共 768 px，
 * S 档 ASCII 11 px/字符 → 69 个字符；取 64 留一点余量，超长的截断加 "..."。 */
#define PK_APT_DETAIL_NAME_MAX  64

/* 呼号预算。真库里的呼号是 "GUANGZHOU TOWER" 这一类，最长约 30 字节。 */
#define PK_APT_DETAIL_CALL_MAX  32

/* ── 生命周期（签名风格照 search_page.h）────────────────────────── */

/* 打开时的来路。见文件头「三层导航是怎么解决的」。 */
typedef enum {
    PK_APT_DETAIL_FROM_MAP = 0,   /* 地图页点机场符号 */
    PK_APT_DETAIL_FROM_SEARCH,    /* 搜索结果点一条机场 */
} pk_apt_detail_from_t;

/* apt_idx = 机场段内记录下标（搜索结果 / 叠加层命中都带着它）。
 * 库未就绪或下标越界时仍然打开页面，但只显示一条"数据不可用"——静默不开
 * 会让用户以为自己没点中。 */
void pk_apt_detail_page_open(uint32_t apt_idx, pk_apt_detail_from_t from);

bool pk_apt_detail_page_active(void);
void pk_apt_detail_page_close(void);

void pk_apt_detail_page_render(uint16_t *fb);

/* 触摸：约定同 search/diag/settings。本页是模态的，active 期间**吃掉整屏**。 */
bool pk_apt_detail_page_touch(int x, int y);
bool pk_apt_detail_page_drag(int x, int y);
void pk_apt_detail_page_touch_up(void);
/* 当前唯一调用方是模拟器 sim/main.c 的 PK_SIM_APT_SCROLL 截图铺垫
 * （详见 .c 里的注释）。 */
void pk_apt_detail_page_touch_cancel(void);

/*
 * 真机自检开关（默认 0）。
 *
 * 开着的话，开机等库 READY 之后自动查一次 ZGGG，把"取到几条跑道方向、几条
 * 频率、业务序排完第一条是什么"打进串口日志。用来在没人守在盒子边上时验证
 * 数据链路——触摸手感仍然只能人工验。
 *
 * 它**只跑数据链路**，不碰 pk_apt_detail_page_open()：那个函数会调
 * pk_ui_nav_set_fab_hidden()，而那是 LVGL 对象操作，本项目的 LVGL 没开
 * LV_OS 锁、整棵控件树由 pfd 任务独占，从后台任务调进去实测会卡死
 * （search_page.c 的 smoke_run 记着这次教训）。
 *
 * 默认 0 的理由同 PK_SEARCH_PAGE_SMOKE：它要多起一个任务、多占 4 KB 栈，
 * 而正式固件里没人看那几行日志。要验收时临时改 1、烧一次、看日志，再改回 0。
 */
#define PK_APT_DETAIL_SMOKE  0

/* 自检任务的创建入口。PK_APT_DETAIL_SMOKE=0 时是个空实现（inline 掉），
 * 所以 main.c 那边不必包 #if——少一处两边要对齐的条件编译。 */
void pk_apt_detail_smoke_init(void);

/* ── 模态层的分派次序（唯一真源）────────────────────────────────
 *
 * 渲染（pfd.c）与触摸（touch_gt911.c）两处必须用**同一个**次序，否则就会
 * 出现"屏上画的是详情、点下去命中的是底下那一页"。此前这条次序是两串手写的
 * if/else 各写一遍，靠注释互相提醒——搜索页刚上线时那两串还只有两层，第三层
 * 一加就是两处各改一次的机会。收成一个纯函数之后，改次序只有一个地方，
 * 而且 host 单测钉得住（test_apt_detail_page.c）。
 *
 * 次序 键盘 > 详情 > 搜索 的依据是**打开方向**：上层只能从下层打开，反过来
 * 不成立。键盘从搜索的查询行打开；详情从搜索结果或地图符号打开。于是"关掉
 * 最上面一层就回到来时的地方"自动成立——这就是三层导航的全部实现，
 * 见本文件开头那段。
 *
 * 导航网格（NAVGRID）压在这三层之上，但它不属于那条"打开方向"链——它由
 * FAB 打开，而 FAB 在键盘/详情/搜索任一层活着时都是隐藏的（不然按下去会
 * 穿透到被盖住的那一页）。也就是说网格与其余三层互斥，四层之间不存在谁比
 * 谁"更外层"的嵌套关系，排最前只是把这份互斥关系直接翻译成次序，不会跟
 * "关掉最上层回到来时的地方"这条推论打架。
 *
 * 放在本头文件而不是另开一个：详情页是把模态层从两层变成三层的那一个，
 * 这条规则的成本也正是它带来的。真出现第五层时再谈独立文件。
 */
typedef enum {
    PK_UI_MODAL_NONE = 0,   /* 没有模态层：按 pk_ui_mode_t 走 */
    PK_UI_MODAL_NAVGRID,    /* 全屏导航网格：FAB 打开，压在一切之上 */
    PK_UI_MODAL_KEYBOARD,
    PK_UI_MODAL_DETAIL,
    PK_UI_MODAL_SEARCH,
} pk_ui_modal_t;

pk_ui_modal_t pk_ui_modal_top(bool navgrid_active, bool keyboard_active,
                              bool detail_active, bool search_active);

/* ── 以下是纯函数区：无 OS / 无全局状态，host 单测直接编
 * （firmware/test/test_apt_detail_page.c）。同 search_page.h 的分区惯例。 ── */

typedef struct {
    char     desig[6];        /* "06L"；数据侧 char[4]，留 NUL 与余量 */
    uint16_t length_ft;       /* 0 = 无数据，那一项**不显示** */
    uint16_t width_ft;        /* 同上 */
    uint8_t  surface;         /* SURFACE_ENUM；0 = 未知，不显示 */
    bool     has_bearing;     /* false = 原始值是 0xFFFF 哨兵 */
    uint16_t mag_bearing_dd;  /* 0.1°，has_bearing=false 时无效 */
    bool     has_coord;       /* 入口坐标是否存在（真库里约 81% 没有）*/
} pk_apt_rwy_item_t;

typedef struct {
    uint32_t freq_khz;
    uint8_t  service;                         /* SERVICE_ENUM */
    char     callsign[PK_APT_DETAIL_CALL_MAX];
} pk_apt_freq_item_t;

/*
 * 频率的**业务序**权重（越小越靠前，未命中一律 99）。
 *
 * 照抄 App 机场详情页那张表：TWR > GND > ATIS > APP > DEP > CTL > AFIS。
 * 盒子这边的 SERVICE_ENUM 里没有 CTL 这一档（它在管线里被拆成了 APP/DEP/
 * RADAR），所以实际落地是 TWR > GND > ATIS > APP > DEP > AFIS > 其余。
 *
 * 为什么不能按存储序摆：库里的顺序来自上游数据源的行序，ZGGG 那 63 条里
 * 塔台可能排在第 40 条。飞行员打开这一页九成是为了找塔台或地面，让他滚三屏
 * 才看到 TWR 是把最常用的信息藏起来。
 */
uint8_t pk_apt_freq_rank(uint8_t service);

/* 按业务序**稳定**排序。同权重的保持原有存储序——同一个塔台的两条频率
 * （主用/备用）在上游是挨着的且有先后含义，重排会把备用摆到主用前面。
 * 插入排序：n ≤ 64，比引入 qsort 少一层函数指针间接，也与 search_page 一致。 */
void pk_apt_freq_sort(pk_apt_freq_item_t *arr, int n);

/* 服务类型缩写。**不翻译**——TWR/GND/ATIS 是 ICAO 通用缩写，中文飞行员
 * 也这么读、无线电里也这么念（同 HDG/QNH/ICAO 的处置）。
 * 未知枚举返回 "---"：留空会让那一列看着像渲染坏了。 */
const char *pk_apt_service_tag(uint8_t service);

/* 道面类型。同样不翻译（航图上就印 ASPH/CONC）。
 * **未知返回 NULL**，调用方据此整项不画——印一个 "UNKNOWN" 是在用一行字
 * 告诉飞行员"我不知道"，不如把那块地方让给别的字段。 */
const char *pk_apt_surface_tag(uint8_t surface);

/* kHz → "118.250"。库里是整 kHz，除 1000 取三位小数正好还原航空频率的
 * 8.33/25 kHz 间隔；不做 "%.3f" 直接算是为了避开 printf 的浮点舍入
 * （118.275 在 double 上是 118.27499999…，%.3f 侥幸对，%.2f 就错了）。 */
void pk_apt_format_freq(char *out, size_t cap, uint32_t freq_khz);

/* 磁航向（整度，0..359）。has_bearing=false 时返回 false，调用方**不显示**
 * 这一项——哨兵 0xFFFF 直接除 10 会画出 "6553°"，而填 0 会被读成"正北"，
 * 两种都是在编数据。 */
bool pk_apt_rwy_bearing_deg(const pk_apt_rwy_item_t *r, int *out_deg);

#ifdef __cplusplus
}
#endif
