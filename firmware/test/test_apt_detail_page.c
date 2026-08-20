/* test_apt_detail_page.c — host proof for 机场详情页的纯函数区
 * （频率业务序 / 服务与道面缩写 / kHz→MHz / 跑道哨兵处置）。
 *   cc -std=c11 -Wall -Wextra -O2 -I firmware/main -DPK_APT_DETAIL_HOST_TEST \
 *      -o /tmp/test_apt firmware/test/test_apt_detail_page.c && /tmp/test_apt
 *
 * 同 test_pk_aero_layer.c / test_search_page.c 的翻译单元惯例：把被测 .c
 * 直接拉进来，用 PK_APT_DETAIL_HOST_TEST 切掉要 IDF 的渲染与触摸两段。
 *
 * 四段，每一段都对着一个"错了用户看不出来但数据是假的"的坑：
 *   1) 频率业务序——库里是上游行序，塔台可能排在第 40 条；
 *   2) 缩写表——服务类型/道面一律不译，未知值不许画成 0 或空白；
 *   3) kHz→MHz——不能走 double，118275 会落在 118.27499…；
 *   4) 跑道哨兵——0xFFFF 磁航向直接除 10 会画出 6553°，填 0 会读成正北。
 */
#include <stdio.h>
#include <string.h>

#include "../main/apt_detail_page.c"

static int g_fail;

static void chk_int(const char *what, int got, int want)
{
    if (got != want) { printf("FAIL %s: got %d want %d\n", what, got, want); g_fail++; }
}

static void chk_true(const char *what, bool got)
{
    if (!got) { printf("FAIL %s: 期望 true\n", what); g_fail++; }
}

static void chk_str(const char *what, const char *got, const char *want)
{
    if (got == NULL || strcmp(got, want) != 0) {
        printf("FAIL %s: got \"%s\" want \"%s\"\n", what, got ? got : "(null)", want);
        g_fail++;
    }
}

/* ── 1) 频率业务序 ─────────────────────────────────────────────── */

static void test_freq_rank(void)
{
    /* App 那张表：TWR > GND > ATIS > APP > DEP > CTL > AFIS。
     * 盒子的 SERVICE_ENUM 没有 CTL 这一档，其余逐档对齐。 */
    chk_int("TWR 最先",  pk_apt_freq_rank(SVC_TWR),  0);
    chk_int("GND 第二",  pk_apt_freq_rank(SVC_GND),  1);
    chk_int("ATIS 第三", pk_apt_freq_rank(SVC_ATIS), 2);
    chk_int("APP 第四",  pk_apt_freq_rank(SVC_APP),  3);
    chk_int("DEP 第五",  pk_apt_freq_rank(SVC_DEP),  4);
    chk_int("AFIS 第六", pk_apt_freq_rank(SVC_AFIS), 5);
    /* ATIS 排在 APP 之前不是笔误：起飞落地前先抄 ATIS，这是流程顺序。 */
    chk_true("ATIS 先于 APP",
             pk_apt_freq_rank(SVC_ATIS) < pk_apt_freq_rank(SVC_APP));
    /* 表外的一律并列 99，靠稳定排序保住存储序。 */
    chk_int("CTAF 未命中",   pk_apt_freq_rank(SVC_CTAF),   99);
    chk_int("UNICOM 未命中", pk_apt_freq_rank(SVC_UNICOM), 99);
    chk_int("枚举 0 未命中", pk_apt_freq_rank(0),          99);
    chk_int("越界值未命中",  pk_apt_freq_rank(200),        99);
}

static pk_apt_freq_item_t mk(uint8_t svc, uint32_t khz)
{
    pk_apt_freq_item_t f;
    memset(&f, 0, sizeof(f));
    f.service  = svc;
    f.freq_khz = khz;
    return f;
}

static void test_freq_sort(void)
{
    /* 存储序刻意摆成最坏：塔台在最后一条，AWOS 抢在最前。 */
    pk_apt_freq_item_t a[] = {
        mk(SVC_AWOS, 128000),
        mk(SVC_DEP,  124500),
        mk(SVC_ATIS, 127350),
        mk(SVC_APP,  120500),
        mk(SVC_GND,  121600),
        mk(SVC_TWR,  118250),
    };
    pk_apt_freq_sort(a, 6);
    chk_int("排后 [0] = TWR",  a[0].service, SVC_TWR);
    chk_int("排后 [1] = GND",  a[1].service, SVC_GND);
    chk_int("排后 [2] = ATIS", a[2].service, SVC_ATIS);
    chk_int("排后 [3] = APP",  a[3].service, SVC_APP);
    chk_int("排后 [4] = DEP",  a[4].service, SVC_DEP);
    chk_int("排后 [5] = AWOS 垫底", a[5].service, SVC_AWOS);

    /* **稳定性**：同一服务的两条（主用/备用）必须保持存储先后。
     * 上游把它们排在一起且有先后含义，重排就是把备用摆到主用前面。 */
    pk_apt_freq_item_t b[] = {
        mk(SVC_TWR, 118250),   /* 主用 */
        mk(SVC_TWR, 118700),   /* 备用 */
        mk(SVC_GND, 121600),
    };
    pk_apt_freq_sort(b, 3);
    chk_int("同权重保序：主用在前", (int)b[0].freq_khz, 118250);
    chk_int("同权重保序：备用在后", (int)b[1].freq_khz, 118700);

    /* 一堆全是表外类型：整体等价于原样不动。 */
    pk_apt_freq_item_t c[] = {
        mk(SVC_UNICOM, 122800),
        mk(SVC_CTAF,   122900),
        mk(SVC_AWOS,   135075),
    };
    pk_apt_freq_sort(c, 3);
    chk_int("全表外保序 [0]", (int)c[0].freq_khz, 122800);
    chk_int("全表外保序 [1]", (int)c[1].freq_khz, 122900);
    chk_int("全表外保序 [2]", (int)c[2].freq_khz, 135075);

    /* 退化输入不许炸。 */
    pk_apt_freq_sort(NULL, 5);
    pk_apt_freq_sort(c, 0);
    pk_apt_freq_sort(c, 1);
    chk_int("n<2 原样", (int)c[0].freq_khz, 122800);
}

/* ── 2) 缩写表 ────────────────────────────────────────────────── */

static void test_tags(void)
{
    chk_str("TWR",  pk_apt_service_tag(SVC_TWR),  "TWR");
    chk_str("ATIS", pk_apt_service_tag(SVC_ATIS), "ATIS");
    chk_str("UNICOM", pk_apt_service_tag(SVC_UNICOM), "UNICOM");
    /* MULTICOM 缩到 5 字符：68 px 的徽章按 PK_AA_XS 10 px/字符只放得下 6 个，
     * 8 个字符会顶破圆角底。 */
    chk_str("MULTICOM 缩写", pk_apt_service_tag(SVC_MULTICOM), "MULTI");
    /* 未知服务给占位而不是空串：空串会让那一列看着像渲染坏了。 */
    chk_str("未知服务占位", pk_apt_service_tag(0),   "---");
    chk_str("越界服务占位", pk_apt_service_tag(250), "---");
    /* 所有徽章文字都必须放得进 68 px（含左右各 ~4 px 余量）。 */
    for (int s = 0; s <= 21; ++s) {
        const char *t = pk_apt_service_tag((uint8_t)s);
        if ((int)strlen(t) > 6) {
            printf("FAIL 服务缩写过长: %d -> \"%s\"\n", s, t);
            g_fail++;
        }
    }

    chk_str("ASPH", pk_apt_surface_tag(1), "ASPH");
    chk_str("CONC", pk_apt_surface_tag(2), "CONC");
    chk_str("ICE",  pk_apt_surface_tag(9), "ICE");
    /* 未知道面**返回 NULL**：调用方据此整项不画。印 "UNKNOWN" 等于用一行字
     * 说"我不知道"，不如把地方让给别的字段。 */
    chk_true("未知道面不画", pk_apt_surface_tag(0)   == NULL);
    chk_true("越界道面不画", pk_apt_surface_tag(200) == NULL);
}

/* ── 3) kHz → MHz ─────────────────────────────────────────────── */

static void test_format_freq(void)
{
    char b[16];
    pk_apt_format_freq(b, sizeof(b), 118250);
    chk_str("118.250", b, "118.250");
    /* 8.33 kHz 间隔的频率：走 double 的话 118275 会落在 118.27499…，
     * %.2f 就会打成 118.27。整数分解没有这个问题。 */
    pk_apt_format_freq(b, sizeof(b), 118275);
    chk_str("8.33 间隔", b, "118.275");
    /* 前导零必须补满三位，否则 121.005 会打成 121.5。 */
    pk_apt_format_freq(b, sizeof(b), 121005);
    chk_str("小数补零", b, "121.005");
    pk_apt_format_freq(b, sizeof(b), 121000);
    chk_str("整数兆赫", b, "121.000");
    /* NDB 那一档是三位数 kHz（库里的导航台频率），不能崩。 */
    pk_apt_format_freq(b, sizeof(b), 375);
    chk_str("NDB 375 kHz", b, "0.375");
    pk_apt_format_freq(b, sizeof(b), 0);
    chk_str("零频", b, "0.000");
    /* 退化入参不许写越界。 */
    pk_apt_format_freq(NULL, 16, 118250);
    pk_apt_format_freq(b, 0, 118250);
}

/* ── 4) 跑道的哨兵处置 ────────────────────────────────────────── */

static void test_rwy_sentinels(void)
{
    pk_apt_rwy_item_t r;
    memset(&r, 0, sizeof(r));
    int deg = -1;

    /* 缺磁航向（原始 0xFFFF）：**不显示**。真库里这一档不少见，直接除 10
     * 会画出 "MAG 6553"，填 0 会被读成正北——两种都是在编数据。 */
    r.has_bearing = false;
    r.mag_bearing_dd = 0xFFFF;
    chk_true("无磁航向不给值", !pk_apt_rwy_bearing_deg(&r, &deg));
    chk_int("失败时不写出参", deg, -1);

    r.has_bearing = true;
    r.mag_bearing_dd = 643;          /* 64.3° */
    chk_true("有磁航向给值", pk_apt_rwy_bearing_deg(&r, &deg));
    chk_int("64.3 → 64", deg, 64);

    r.mag_bearing_dd = 645;          /* 64.5° 进位 */
    chk_true("四舍五入", pk_apt_rwy_bearing_deg(&r, &deg));
    chk_int("64.5 → 65", deg, 65);

    /* 360.0° 要回到 0，不能画成 "MAG 360"（航图上正北是 360 或 0，但这里
     * 与 %03d 一起用，写 360 会占四位顶破列宽）。 */
    r.mag_bearing_dd = 3600;
    chk_true("360 度有值", pk_apt_rwy_bearing_deg(&r, &deg));
    chk_int("360.0 → 0", deg, 0);

    /* 359.6 进位到 360 之后同样归零，不许出现 360。 */
    r.mag_bearing_dd = 3596;
    chk_true("359.6 有值", pk_apt_rwy_bearing_deg(&r, &deg));
    chk_int("359.6 → 0", deg, 0);

    r.mag_bearing_dd = 0;
    chk_true("正北 0 度是合法值", pk_apt_rwy_bearing_deg(&r, &deg));
    chk_int("0.0 → 0", deg, 0);

    chk_true("空指针不崩", !pk_apt_rwy_bearing_deg(NULL, &deg));
    r.has_bearing = true;
    r.mag_bearing_dd = 100;
    chk_true("允许不要出参", pk_apt_rwy_bearing_deg(&r, NULL));
}

/* ── 5) 容量与真实数据量的对账 ────────────────────────────────── */

static void test_capacity(void)
{
    /* ZGGG 实测 10 条跑道方向 / 63 条频率，KJFK 8/43。上限必须兜得住，
     * 否则塔台频率会被静默截掉——飞行员看不出"没有"和"被截了"的区别。 */
    chk_true("跑道上限兜得住 ZGGG 的 10 条", PK_APT_DETAIL_RWY_MAX >= 10);
    chk_true("频率上限兜得住 ZGGG 的 63 条", PK_APT_DETAIL_FREQ_MAX >= 63);
}

/* ── 6) 模态层次序 = 返回目标状态机 ───────────────────────────────
 *
 * 这一段钉的是三层导航的**全部**实现（apt_detail_page.h 的文件头）：模态层
 * 不切页、只盖页，关掉最上面一层露出来的就是来时的地方。所以"返回去哪"这个
 * 问题的答案就是 pk_ui_modal_top 在少一层之后返回什么——把两条真实路径按
 * 用户的动作顺序走一遍即可。
 *
 * 同时它也是"两处分派次序一致"的凭据：pfd.c 与 touch_gt911.c 都调这一个
 * 函数，次序错不开。 */
static void test_modal_order(void)
{
    /* 什么都没开：按 pk_ui_mode_t 走。 */
    chk_int("无模态层", pk_ui_modal_top(false, false, false, false), PK_UI_MODAL_NONE);

    /* 路径 A：地图 → 详情 → 返回。 */
    chk_int("A1 地图上点机场符号 → 详情",
            pk_ui_modal_top(false, false, true, false), PK_UI_MODAL_DETAIL);
    chk_int("A2 关掉详情 → 回地图",
            pk_ui_modal_top(false, false, false, false), PK_UI_MODAL_NONE);

    /* 路径 B：地图 → 搜索 → 详情 → 返回 → 关搜索。
     * 第 3 步是关键：搜索**仍然 active**，但屏上必须是详情。 */
    chk_int("B1 打开搜索", pk_ui_modal_top(false, false, false, true), PK_UI_MODAL_SEARCH);
    chk_int("B2 点结果进详情，搜索仍开着 → 屏上是详情",
            pk_ui_modal_top(false, false, true, true), PK_UI_MODAL_DETAIL);
    chk_int("B3 关掉详情 → 回到搜索",
            pk_ui_modal_top(false, false, false, true), PK_UI_MODAL_SEARCH);
    chk_int("B4 关掉搜索 → 回地图",
            pk_ui_modal_top(false, false, false, false), PK_UI_MODAL_NONE);

    /* 路径 C：搜索里点查询行拉起键盘——键盘压在所有人之上。 */
    chk_int("C1 键盘盖住搜索",
            pk_ui_modal_top(false, true, false, true), PK_UI_MODAL_KEYBOARD);
    chk_int("C2 键盘盖住详情+搜索",
            pk_ui_modal_top(false, true, true, true), PK_UI_MODAL_KEYBOARD);
    chk_int("C3 关掉键盘 → 回到搜索",
            pk_ui_modal_top(false, false, false, true), PK_UI_MODAL_SEARCH);

    /* 「在地图上显示」把详情与搜索**一起**关掉，一步落到地图。 */
    chk_int("D 一路关到地图", pk_ui_modal_top(false, false, false, false),
            PK_UI_MODAL_NONE);

    /* 导航网格：由 FAB 打开，而 FAB 在任何模态层活着时都是隐藏的，
     * 因此它与其余三层互斥。压在最高优先级不会与"上层只能从下层打开"
     * 那条推论冲突——它根本不在那条链上。 */
    chk_int("E1 网格压住一切",
            pk_ui_modal_top(true, true, true, true), PK_UI_MODAL_NAVGRID);
    chk_int("E2 只有网格",
            pk_ui_modal_top(true, false, false, false), PK_UI_MODAL_NAVGRID);
    chk_int("E3 关掉网格 → 回底层页",
            pk_ui_modal_top(false, false, false, false), PK_UI_MODAL_NONE);
}

/* ── 7) FAB 显隐判据 ─────────────────────────────────────────────
 *
 * 规则一句话：还有任何一层模态活着，FAB 就得藏着。之所以值得单独钉住，是因为
 * 2026-08-04 之前这个布尔值由四层各自 open/close 时**手算**，普查抓到两处算
 * 错的，两处都是"关掉自己这一层时无条件放出 FAB"：
 *
 *   - 导航网格点「搜索」：先 open 搜索（藏）再 close 网格（放）→ FAB 浮在
 *     搜索页上；
 *   - 搜索页里敲完键盘按「确定」：keyboard 的 close_page 无条件放，而底下的
 *     搜索页还开着 → FAB 浮在结果列表上。
 *
 * 这两条正是下面的 F2 / F4，用户能看见的现象都是"一枚点不动的悬浮球盖住了
 * 底下那一行"。判据本身是 top != NONE 一句话，测试的价值不在算式，在把
 * **哪些状态组合是真实会出现的**写下来。 */
static void test_fab_hidden(void)
{
    /* 一层都没有 = 底层页在屏上，FAB 是唯一的导航入口，必须露着。 */
    chk_true("F1 无模态层 → FAB 露着",
             !pk_ui_fab_hidden_for(pk_ui_modal_top(false, false, false, false)));

    /* F2 = 导航网格点「搜索」的中间态：两层同时活着。旧实现在这一刻按
     * "网格关了"算，放出了 FAB。 */
    chk_true("F2 网格+搜索同时活着 → 仍然藏",
             pk_ui_fab_hidden_for(pk_ui_modal_top(true, false, false, true)));
    chk_true("F3 网格关掉、搜索留下 → 仍然藏",
             pk_ui_fab_hidden_for(pk_ui_modal_top(false, false, false, true)));

    /* F4 = 搜索页里关掉键盘的那一刻。旧实现无条件放出 FAB。 */
    chk_true("F4 键盘+搜索 → 藏",
             pk_ui_fab_hidden_for(pk_ui_modal_top(false, true, false, true)));
    chk_true("F5 关掉键盘、搜索留下 → 仍然藏",
             pk_ui_fab_hidden_for(pk_ui_modal_top(false, false, false, true)));

    /* F6/F7 = 「在地图上显示」：详情与搜索一起关掉，一步落到地图，FAB 回来。 */
    chk_true("F6 详情+搜索 → 藏",
             pk_ui_fab_hidden_for(pk_ui_modal_top(false, false, true, true)));
    chk_true("F7 详情与搜索一起关掉 → FAB 回来",
             !pk_ui_fab_hidden_for(pk_ui_modal_top(false, false, false, false)));

    /* 单层各自也要成立——四个入参谁单独为真都得藏。 */
    chk_true("F8 只有网格",   pk_ui_fab_hidden_for(pk_ui_modal_top(true, false, false, false)));
    chk_true("F9 只有键盘",   pk_ui_fab_hidden_for(pk_ui_modal_top(false, true, false, false)));
    chk_true("F10 只有详情",  pk_ui_fab_hidden_for(pk_ui_modal_top(false, false, true, false)));
    chk_true("F11 只有搜索",  pk_ui_fab_hidden_for(pk_ui_modal_top(false, false, false, true)));
}

/* ── 8) 收起态（sheet 语义）的状态机 ──────────────────────────────
 *
 * 2026-08-04 评审推翻了"跳转是终点、不提供返回"：
 *
 *   「搜索到地图后，点击返回就回到搜索结果页……手持设备没有电脑或者手机
 *     那么方便，所以不要为用户增加'再搜索一次'的交互难度。」
 *
 * 于是「点结果 → 跳地图」从 close() 变成 collapse()。这一段钉的是那条
 * 状态机以及它与 pk_ui_modal_top / pk_ui_fab_hidden_for 的关系——三者错开
 * 一格，用户看到的就是"FAB 一直藏着、地图点不动"或者"屏上是地图、点下去
 * 命中的是搜索页"。 */
static void test_sheet_state(void)
{
    /* 基本迁移。 */
    chk_int("S1 没开过 → 打开",
            pk_sheet_next(PK_SHEET_CLOSED, PK_SHEET_EV_OPEN), PK_SHEET_OPEN);
    chk_int("S2 开着 → 收起",
            pk_sheet_next(PK_SHEET_OPEN, PK_SHEET_EV_COLLAPSE), PK_SHEET_COLLAPSED);
    chk_int("S3 收起 → 恢复",
            pk_sheet_next(PK_SHEET_COLLAPSED, PK_SHEET_EV_RESTORE), PK_SHEET_OPEN);
    chk_int("S4 开着 → 真关闭",
            pk_sheet_next(PK_SHEET_OPEN, PK_SHEET_EV_CLOSE), PK_SHEET_CLOSED);
    chk_int("S5 收起 → 真关闭（页首 CLOSE 恢复后按下）",
            pk_sheet_next(PK_SHEET_COLLAPSED, PK_SHEET_EV_CLOSE), PK_SHEET_CLOSED);

    /*
     * S6 是这条状态机存在的理由之一：**没开过的收不起来**。
     * 「在地图上显示」要把详情与它底下的搜索一起收起，而从地图点机场符号
     * 进来时搜索根本没开过——不挡住的话，地图上会冒出一枚返回钮，点下去
     * "恢复"出一个用户从没打开过的空搜索页。
     */
    chk_int("S6 没开过的收不起来",
            pk_sheet_next(PK_SHEET_CLOSED, PK_SHEET_EV_COLLAPSE), PK_SHEET_CLOSED);
    /* 反向同理：不在收起态时"恢复"必须是空操作。地图上那枚钮只在
     * has_collapsed 时才画，但 restore 是整叠一起调的（搜索 + 详情），
     * 其中一层常常本来就是 CLOSED 或 OPEN。 */
    chk_int("S7 没收起的恢复不了（CLOSED 保持）",
            pk_sheet_next(PK_SHEET_CLOSED, PK_SHEET_EV_RESTORE), PK_SHEET_CLOSED);
    chk_int("S8 已经开着的恢复是空操作",
            pk_sheet_next(PK_SHEET_OPEN, PK_SHEET_EV_RESTORE), PK_SHEET_OPEN);

    /* 幂等：连点两下返回钮、或者 collapse 被 goto_map 与 goto_item 各调一次。 */
    chk_int("S9 收起两次",
            pk_sheet_next(PK_SHEET_COLLAPSED, PK_SHEET_EV_COLLAPSE), PK_SHEET_COLLAPSED);
    chk_int("S10 打开两次",
            pk_sheet_next(PK_SHEET_OPEN, PK_SHEET_EV_OPEN), PK_SHEET_OPEN);
    chk_int("S11 关两次",
            pk_sheet_next(PK_SHEET_CLOSED, PK_SHEET_EV_CLOSE), PK_SHEET_CLOSED);

    /* S12：收起态下再点入口（放大镜 / 导航网格的「搜索」格）= 恢复，
     * 不是重开。调用方据此**不重置查询串**（search_page.c 的 open()）。 */
    chk_int("S12 收起态下再打开 → OPEN（调用方据此不清空查询串）",
            pk_sheet_next(PK_SHEET_COLLAPSED, PK_SHEET_EV_OPEN), PK_SHEET_OPEN);

    /* 重查闸门：**只有 OPEN 能重查**。收起期间后台任务那条"库刚加载完就
     * 自己重跑一次"的兜底必须闭嘴——演示模式下本机一直在动，「附近机场」
     * 重跑一次就换一批，用户把列表拉回来会发现内容变了。 */
    chk_true("S13 开着可以重查",   pk_sheet_may_requery(PK_SHEET_OPEN));
    chk_true("S14 收起不许重查",  !pk_sheet_may_requery(PK_SHEET_COLLAPSED));
    chk_true("S15 关掉不许重查",  !pk_sheet_may_requery(PK_SHEET_CLOSED));
}

/* ── 9) 收起态与模态次序 / FAB 判据的联动 ────────────────────────
 *
 * 收起态最容易错的地方不在状态机本身，而在"它算不算活跃层"。答案是**不算**：
 * active() 只在 OPEN 时为真。错成"收起也算活跃"的现象非常具体——地图铺在
 * 屏上，但 FAB 一直藏着、点哪儿都没反应，因为触摸分派仍然落在那个不渲染的
 * 搜索页上。下面把三条真实路径按用户的动作顺序走一遍。
 *
 * 约定：把 pk_sheet_state_t 折成 active 布尔（== PK_SHEET_OPEN）再喂给
 * pk_ui_modal_top，与 pk_search_page_active() / pk_apt_detail_page_active()
 * 在固件里做的是同一件事。 */
static bool sheet_active(pk_sheet_state_t st) { return st == PK_SHEET_OPEN; }

static void test_sheet_vs_modal(void)
{
    /* 路径 G：搜索 → 点导航台 → 地图 → 返回钮 → 搜索。 */
    pk_sheet_state_t s = PK_SHEET_CLOSED, d = PK_SHEET_CLOSED;

    s = pk_sheet_next(s, PK_SHEET_EV_OPEN);
    chk_int("G1 搜索开着 → 屏上是搜索",
            pk_ui_modal_top(false, false, sheet_active(d), sheet_active(s)),
            PK_UI_MODAL_SEARCH);
    chk_true("G1 FAB 藏着",
             pk_ui_fab_hidden_for(pk_ui_modal_top(false, false, sheet_active(d),
                                                  sheet_active(s))));

    s = pk_sheet_next(s, PK_SHEET_EV_COLLAPSE);
    chk_int("G2 点导航台跳地图 → 搜索收起，屏上回到底层页",
            pk_ui_modal_top(false, false, sheet_active(d), sheet_active(s)),
            PK_UI_MODAL_NONE);
    chk_true("G2 FAB 必须放出来（否则地图上一枚点不动的球都没有，且地图点不动）",
             !pk_ui_fab_hidden_for(pk_ui_modal_top(false, false, sheet_active(d),
                                                   sheet_active(s))));
    chk_int("G2 但状态留着 → 地图上要画返回钮", s, PK_SHEET_COLLAPSED);

    s = pk_sheet_next(s, PK_SHEET_EV_RESTORE);
    chk_int("G3 点返回钮 → 又回到搜索",
            pk_ui_modal_top(false, false, sheet_active(d), sheet_active(s)),
            PK_UI_MODAL_SEARCH);

    s = pk_sheet_next(s, PK_SHEET_EV_CLOSE);
    chk_int("G4 页首 CLOSE → 真关掉，返回钮也该消失", s, PK_SHEET_CLOSED);

    /* 路径 H：搜索 → 机场 → 详情 → 在地图上显示 → 返回钮 → **详情** → 搜索。
     * 这是"逐层返回"那条决定的凭据：整叠一起收、一起恢复，恢复后屏上是详情
     * （它在搜索之上），再按 BACK 才回搜索，而搜索的结果列表原样还在。 */
    s = pk_sheet_next(PK_SHEET_CLOSED, PK_SHEET_EV_OPEN);
    d = pk_sheet_next(PK_SHEET_CLOSED, PK_SHEET_EV_OPEN);
    chk_int("H1 详情盖住搜索",
            pk_ui_modal_top(false, false, sheet_active(d), sheet_active(s)),
            PK_UI_MODAL_DETAIL);

    /* goto_map()：两层一起收。 */
    d = pk_sheet_next(d, PK_SHEET_EV_COLLAPSE);
    s = pk_sheet_next(s, PK_SHEET_EV_COLLAPSE);
    chk_int("H2 在地图上显示 → 整叠收起，屏上是地图",
            pk_ui_modal_top(false, false, sheet_active(d), sheet_active(s)),
            PK_UI_MODAL_NONE);
    chk_true("H2 FAB 放出来",
             !pk_ui_fab_hidden_for(pk_ui_modal_top(false, false, sheet_active(d),
                                                   sheet_active(s))));

    /* pk_ui_sheet_restore()：也是两层一起。 */
    d = pk_sheet_next(d, PK_SHEET_EV_RESTORE);
    s = pk_sheet_next(s, PK_SHEET_EV_RESTORE);
    chk_int("H3 返回钮 → 回到**详情**（逐层返回）",
            pk_ui_modal_top(false, false, sheet_active(d), sheet_active(s)),
            PK_UI_MODAL_DETAIL);

    d = pk_sheet_next(d, PK_SHEET_EV_CLOSE);
    chk_int("H4 详情 BACK → 结果列表还在（这条错了整个改动就白做）",
            pk_ui_modal_top(false, false, sheet_active(d), sheet_active(s)),
            PK_UI_MODAL_SEARCH);

    /* 路径 I：地图点机场符号 → 详情 → 在地图上显示 → 返回钮。
     * 搜索**从没打开过**，收/恢复都不许把它变出来。 */
    s = PK_SHEET_CLOSED;
    d = pk_sheet_next(PK_SHEET_CLOSED, PK_SHEET_EV_OPEN);
    d = pk_sheet_next(d, PK_SHEET_EV_COLLAPSE);
    s = pk_sheet_next(s, PK_SHEET_EV_COLLAPSE);   /* 空操作 */
    chk_int("I1 搜索没开过 → 收不起来", s, PK_SHEET_CLOSED);
    d = pk_sheet_next(d, PK_SHEET_EV_RESTORE);
    s = pk_sheet_next(s, PK_SHEET_EV_RESTORE);    /* 空操作 */
    chk_int("I2 恢复后屏上是详情，搜索仍然不存在",
            pk_ui_modal_top(false, false, sheet_active(d), sheet_active(s)),
            PK_UI_MODAL_DETAIL);
    d = pk_sheet_next(d, PK_SHEET_EV_CLOSE);
    chk_int("I3 详情 BACK → 直接回地图，不会露出空搜索页",
            pk_ui_modal_top(false, false, sheet_active(d), sheet_active(s)),
            PK_UI_MODAL_NONE);

    /* 路径 J：收起期间键盘/导航网格照样能压上来。收起的层不是活跃层，
     * 所以它对次序没有任何影响——这一条钉住"收起 ≠ 半开"。 */
    s = pk_sheet_next(PK_SHEET_CLOSED, PK_SHEET_EV_OPEN);
    s = pk_sheet_next(s, PK_SHEET_EV_COLLAPSE);
    chk_int("J1 搜索收起时打开导航网格 → 网格",
            pk_ui_modal_top(true, false, sheet_active(d), sheet_active(s)),
            PK_UI_MODAL_NAVGRID);
    chk_int("J2 关掉网格 → 回地图，而不是回那个收起的搜索页",
            pk_ui_modal_top(false, false, sheet_active(d), sheet_active(s)),
            PK_UI_MODAL_NONE);
}

int main(void)
{
    test_freq_rank();
    test_freq_sort();
    test_tags();
    test_format_freq();
    test_rwy_sentinels();
    test_capacity();
    test_modal_order();
    test_fab_hidden();
    test_sheet_state();
    test_sheet_vs_modal();
    printf("%s (%d fail)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
