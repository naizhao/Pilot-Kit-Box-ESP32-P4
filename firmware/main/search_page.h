/*
 * search_page.h — 航空数据搜索页（机场 / 导航台 / FIX）。
 *
 * 形态：**模态层，不是一个 pk_ui_mode_t**
 * ---------------------------------------
 * 照 keyboard_page 的先例。两条硬约束把"做成第 8 个页签"这条路堵死了
 * （设计文档 §1.3 / §3.1）：
 *   - dock 已经有 7 个页签，第 8 个算出来 852 px > 800 px，直接溢出
 *     （pk_ui_nav.c:255）；
 *   - 导航只有两层，pk_ui_nav_on_back() 硬编码回 DIAG（pk_ui_nav_host.c:88），
 *     而"搜索 → 结果 → 详情"是三层。
 * 做成模态层则只要改 render/touch 两处分派 + 新增本文件，代价小一个数量级。
 * 出口是页首的 CLOSE。
 *
 * 入口有两个，**底下那一页可以是任何一页**（2026-08-04 更新）
 * ----------------------------------------------------------
 *   1. 地图页右侧那一列按钮上的放大镜（pk_map_page_on_search）——初版唯一入口；
 *   2. 全屏导航网格里的「搜索」格（nav_grid_page.c 的 activate_item），
 *      而网格由 FAB 打开，**任何一页**都能叫出来（dock 删除后的新形态，
 *      commit f560c8a）。
 *
 * 第 2 个入口把初版隐含的前提"本页盖着的一定是地图页"整个推翻了。凡是"关掉
 * 本页之后要让用户看到地图"的动作，都必须自己 pk_ui_set_mode(PK_UI_MODE_MAP)，
 * 不能靠"露出来的就是地图"。踩过的坑与修法写在 .c 的 goto_item() 上方。
 *
 * 点结果做什么（2026-08-02 起分两条路）
 * -------------------------------------
 *   机场         → 机场详情页（跑道/频率），页内有「在地图上显示」；
 *   导航台 / FIX → 落 PIN + pk_map_page_goto() + 切到 PK_UI_MODE_MAP + 关本页。
 *
 * 中文：**框架文字有，数据没有**（D1 的边界修正）。
 * 页面文案（标题、分组标题、三种空态）已全部进 i18n catalog，词条前缀
 * SEARCH_，跟随系统语言切换——那批是给人读的散文，没有理由让中文用户
 * 顶着一屏英文。
 * 但**数据侧仍然只能是 ASCII**，这一条没变：屏上的 CJK 字形是 catalog
 * 驱动的子集（gen_pfd_aa_font.py 只把 catalog 里出现过的字做进字模），
 * 机场中文名是任意汉字，渲染出来会是一片空白；数据侧 bin 里也早把非
 * ASCII 换成了 '?'。所以查询串与结果名一律 ASCII，键盘也不做中文输入。
 *
 * 并发：查询**绝不在触摸回调里同步跑**。前 4 桶是 µs 级二分；第 5 桶
 * （名称/城市子串）要顺扫约 3 MB PSRAM 字符串池，真机实测**数秒**级
 * （远超设计文档估的 65 ms——那是 Mac 上的外推），且本任务只有 prio 2，
 * 还要给 12 FPS 的渲染让路。所以：所有查询都丢给本模块自己的后台任务，
 * 渲染只读一份已发布的快照（同 pk_aero_layer 的双缓冲手法）；第 5 桶
 * 只在前 4 桶**一条都没有**时才跑，屏上全程有 loading 态。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 查询串上限。键盘的字符集是 A-Z 0-9 - _，代码类最长的是 FIX/导航台 ident
 * （6 字符）；留到 12 是为了能敲名字片段跑第 5 桶（"GUANGZ"）。
 * 必须 ≤ PK_KBD_TEXT_MAX，见 search_page.c 里的编译期断言。 */
#define PK_SEARCH_QUERY_MAX    12

/* 一次最多攒多少条结果。屏上一次看得见 5 行，多出来的靠滚动；
 * 取 12 是"够翻两屏"与"单字母前缀会枚举出几千条、必须截断"之间的折中。 */
#define PK_SEARCH_MAX_RESULTS  12

/* 结果行显示的名称字节数上限（含 NUL）。
 *
 * 真库里机场名平均 29 字节、最长 85。屏上名称那一行从 x=92 画到 x=784，
 * S 档 11 px/字符 → 62 个字符，所以 63 是"屏幕真正摆得下"的数，缓冲按它定；
 * 再长的在拷贝时截断并以 "..." 收尾（硬切会在屏上留下半截词）。
 * 无论如何都必须当场拷走：查询返回的 name 是指向 PSRAM 池的裸指针，
 * 拔卡即悬空（pk_aero_db.h:23-25）。 */
#define PK_SEARCH_NAME_MAX     64

/* 最近搜索保留多少条。设计文档 §3.2：无键盘设备上"重敲一遍"的成本很高，
 * 历史因此是一等公民而不是辅助胶囊，条数取到 12–16 这一档。 */
#define PK_SEARCH_HIST_MAX     14

/* ── 生命周期（签名风格照 keyboard_page.h）────────────────────────── */

/* 建后台查询任务 + 从 NVS 读回历史。须晚于 pk_aero_db_init()。幂等。 */
void pk_search_page_init(void);

/*
 * 打开。**COLLAPSED（收起）时等价于恢复**：查询串与结果快照原样留着，
 * 不重查也不清空。见下面 collapse/restore 那一段与 apt_detail_page.h 的
 * pk_sheet_state_t。
 */
void pk_search_page_open(void);

/* **只有 OPEN 才算 active**：收起态既不渲染也不吃触摸，pk_ui_modal_top /
 * pk_ui_fab_sync 都得把它当成"不在"，否则 FAB 一直藏着、地图也点不动。 */
bool pk_search_page_active(void);

/* 真关闭：状态作废，回到当初打开它的那一页。 */
void pk_search_page_close(void);

/*
 * 收起 / 恢复（sheet 语义，2026-08-04）。
 *
 * 收起 ≠ 关闭：不渲染、不吃触摸、不算活跃层，但 s_query 与 PSRAM 里那份
 * 结果快照原样留着，**收起期间也不重查**（pk_sheet_may_requery）。恢复回来
 * 时看到的逐像素还是离开前那一屏，包括滚动位置。
 *
 * 为什么不能"回来时重查一遍"：
 *   - 慢。第 5 桶要顺扫 3 MB PSRAM 字符串池，真机数秒（见 .c run_buckets）；
 *   - 会变。默认视图是「附近机场」，本机一直在动，重查回来就是另一批，
 *     用户会以为自己点错了。
 *
 * 唯一会重查的情形是**库换代**（拔卡/重挂载）：那时快照里的段内记录下标
 * 已经指向别的记录，留着才是真的危险。判据是 pk_aero_db_generation()。
 *
 * 三个都幂等，状态不对时是空操作。
 */
void pk_search_page_collapse(void);
void pk_search_page_restore(void);
bool pk_search_page_collapsed(void);

void pk_search_page_render(uint16_t *fb);

/* 触摸：约定同 diag/settings。本页是模态的，active 期间**吃掉整屏**。 */
bool pk_search_page_touch(int x, int y);
bool pk_search_page_drag(int x, int y);
void pk_search_page_touch_up(void);

#ifdef PK_SIM_BUILD
/*
 * 截图用：点第 row 条结果（0 起），走**真机同一条** touch()+touch_up() 路径。
 *
 * 落点从上一帧留下的命中表里取，而不是在 sim/main.c 那边照抄一遍行高与分组
 * 标题的算术——那份几何依赖滚动偏移与当前是不是默认视图，抄一份迟早会飘，
 * 而飘掉之后截出来的图看着还挺正常（点在了相邻那一行上）。
 * **必须先渲染过一帧**：命中表是 render 填的。
 */
void pk_search_sim_tap_row(int row);
#endif

/* ── 以下是纯函数区：无 OS / 无全局状态，host 单测直接编
 * （firmware/test/test_search_page.c）。同 pk_aero_layer.h 的分区惯例。 ── */

typedef enum {
    PK_SEARCH_KIND_AIRPORT = 0,
    PK_SEARCH_KIND_NAVAID,
    PK_SEARCH_KIND_FIX,
} pk_search_kind_t;

typedef struct {
    uint8_t  kind;        /* pk_search_kind_t */
    uint8_t  bucket;      /* 1..5，排序主键：桶间保持提交顺序，桶内才排 */
    uint32_t idx;         /* 段内记录下标，与 kind 一起做去重键 */
    char     code[8];     /* ICAO / ident，全大写 */
    char     name[PK_SEARCH_NAME_MAX];
    double   lat, lon;
    bool     have_dist;   /* 无本机位置时为 false —— 那一列**不显示**，
                           * 不是显示 0.0（0 NM 会被读成"就在脚下"）*/
    double   dist_nm, brg_deg;
} pk_search_item_t;

/*
 * 查询串归一化：转大写 + 丢掉字符集之外的字节 + 截到 cap-1。
 *
 * 必须 toupper：池里的代码类字段全大写，而前缀枚举与 ident 查找走的是
 * memcmp，大小写敏感（设计文档 §6 坑 6）。虽然本机键盘只吐得出大写，
 * 归一化仍放在这里——历史是从 NVS 读回来的，可能来自别的固件版本或手工写入。
 * 返回写进 out 的字符数（0 = 没有可用查询）。
 */
int pk_search_norm_query(const char *in, char *out, size_t cap);

/* 按 (kind, idx) 去重后追加。已满或重复返回 false。
 * 去重是必需的：同一个机场可能既被"ICAO 精确"命中、又被"key 前缀"命中，
 * 子串那一桶里 name 与 city 也会各命中一次。 */
bool pk_search_result_add(pk_search_item_t *arr, int *n, int cap,
                          const pk_search_item_t *it);

/*
 * 桶内排序 [from, to)。
 *
 * 有本机位置就按距离升序（飞行员要的是"离我最近的那个 ZG…"），没有就按
 * 代码字典序——按 0 排等于按数据库里的物理顺序排，屏上看着像随机。
 * **只在桶内排**：桶序本身编码了匹配质量（精确 > 前缀 > 子串），跨桶重排
 * 会把 ZGGG 的精确命中挤到某个名字里含 "ZGGG" 的 FIX 后面。
 * 插入排序：n ≤ 12，比调 qsort 少一个函数指针间接层，也不引入 stdlib。
 */
void pk_search_sort_range(pk_search_item_t *arr, int from, int to);

/* 最近搜索的 LRU。命中已有条目 = 提到队首（不新增），否则插队首、挤掉队尾。 */
typedef struct {
    char items[PK_SEARCH_HIST_MAX][PK_SEARCH_QUERY_MAX + 1];
    int  n;
} pk_search_hist_t;

void pk_search_hist_push(pk_search_hist_t *h, const char *q);

#ifdef __cplusplus
}
#endif
