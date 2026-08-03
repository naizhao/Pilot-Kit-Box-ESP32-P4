/*
 * demo_gpx.h —— SD 卡上的 GPX → 演示模式轨迹表（盒子上现算）。
 *
 * 为什么要在盒子上再实现一遍
 * --------------------------
 * 演示轨迹此前只有一条：firmware/scripts/gen_demo_track.py 在 PC 上把一份内部
 * 飞行记录抽稀成 demo_track_data.c 编进固件。展台上想换一条轨迹（针对客户所在
 * 空域演示）、开发时想构造某个区域的场景，都得重烧固件。
 *
 * 产品的要求是「丢一个 GPX 到 SD 卡的 demo 目录里就自动播放」。于是这里把
 * gen_demo_track.py 的**全部数据清洗逻辑**移植成 C：GPX 解析、去毛刺、去台阶、
 * 停机坪航迹保持、航迹/地速在原始采样率上求、坡度反算、五量联合抽稀、时间戳
 * 去撞车。少移植任何一条，屏上就会出现一个具体的错读数——每一条都是被
 * test_gen_demo_track.py 逼出来的真缺陷修复，不是防御性代码：
 *
 *   ① 高度台阶（记录跨段换基准，2 s 内 +439 m）不做限速跟随 → VS 打到
 *      −16000 fpm；
 *   ② 停机坪 gs≈0 时方位角是纯噪声，不做 3 kt 门槛保持 → 开场十几秒 HSI 疯转；
 *   ③ 非整秒采样四舍五入后时间戳撞车 → 回放二分查找的插值分母为 0；
 *   ④ 地速/航迹必须在**原始 1 Hz** 上算完再抽稀（抽稀后按弦长算，转弯段与真值
 *      差很远，而这两个数直接画在 PFD 上）；
 *   ⑤ 坡度按协调转弯 tanφ = Vω/g 反算并钳 ±30°。
 *
 * 两份实现怎么防漂移
 * ------------------
 * firmware/scripts/check_demo_track_parity.py 拿同一个 GPX 分别跑 Python 与本
 * 模块，逐点逐字段比对量化后的整数。目标是**零差异**，不是「差不多」。为此
 * C 侧刻意做了两件反直觉的事：
 *   - 中间量全部用 double（不是 float）。float 会让抽稀的容差判断在边界上与
 *     Python 分歧，保留点数就对不上了，那是这类双实现最典型的雷。
 *   - 取整用 round-half-to-even（见 round_half_even）。Python 的内建 round()
 *     是 banker's rounding，C 的 round() 是 half-away-from-zero，x.5 上会差 1。
 *
 * 硬约束
 * ------
 *   - **不依赖 ESP-IDF**：只用 C 标准库 + dirent/stdio，好让 host 单测直接
 *     一行 cc 编出来跑（firmware/test/test_demo_gpx.c）。PSRAM 分配由调用方
 *     用 alloc/free 钩子注入（ESP 侧传 heap_caps_*，host 侧传 NULL 用 stdlib）。
 *   - **文件不整个进内存**：源 GPX 实测 984 KB，按 4 KB 块流式扫描。进内存的
 *     只有解析出来的采样点数组（6084 点 × 56 B ≈ 341 KB，落 PSRAM），抽稀后
 *     拷成 12 KB 的最终表并立刻释放原始数组。
 *
 *     没有做「连清洗也全流式」：①的限速跟随是因果的没问题，但②的首个有效航迹
 *     回填是全局的（飞机可能在廊桥上停几分钟才动）、坡度平滑要 ±6 s 前瞻、抽稀
 *     的贪心要回看到锚点（最长 30 s），全流式就得引入延迟线加事后补丁，代码量
 *     翻倍且再也没法和 Python 版逐字对拍。341 KB 峰值在 32 MB PSRAM 上不值得
 *     用这个复杂度去换。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "demo_track.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 原始采样点上限。超出即截断（保留前 max_raw 个点）并置 truncated。
 * 20000 点 ≈ 1.1 MB 峰值、1 Hz 下 5.5 小时航程，比任何演示轨迹都长得多；
 * 存在的意义只是给「用户丢进来一个 100 MB 的怪文件」一个有界的结局。 */
#define PK_DEMO_GPX_MAX_RAW_DEFAULT   20000u

/* 目录扫描一次最多列出的 .gpx 个数，以及单个文件名的缓冲长度。 */
#define PK_DEMO_GPX_LIST_MAX          8
#define PK_DEMO_GPX_NAME_MAX          64

/* PSRAM 分配钩子。两个指针要么都给要么都不给（都不给 = 用 stdlib）。 */
typedef struct {
    void *(*alloc)(size_t n);
    void  (*release)(void *p);
} pk_demo_gpx_alloc_t;

typedef struct {
    pk_demo_track_pt_t *pts;   /* 抽稀后的表，由调用方用同一个钩子释放 */
    uint32_t n;                /* 表长 */
    uint32_t dur_s;            /* 末点 t_s，即真实时长（秒） */
    uint32_t raw_n;            /* 解析出的原始点数（单调过滤之后） */
    bool     truncated;        /* 触到 max_raw 上限 */
} pk_demo_gpx_result_t;

/*
 * 解析并处理一个 GPX 文件。成功返回 true 并填满 *out（out->pts 需调用方释放）。
 * 失败返回 false，*out 不动，*err（若非 NULL）指向一句静态的中文原因。
 *
 * max_raw 传 0 表示用 PK_DEMO_GPX_MAX_RAW_DEFAULT。
 * ac 传 NULL 表示用 stdlib 的 malloc/free。
 *
 * 判为失败的情形：文件打不开、可用轨迹点 < 2、内存不够。
 * **缺 <time> 的点被静默跳过**而不是判失败——没有时间戳就没法定义回放速度，
 * 猜一个 dt 出来只会让 PFD 上的地速是编的。这与 Python 版行为一致。
 */
bool pk_demo_gpx_load_file(const char *path,
                           uint32_t max_raw,
                           const pk_demo_gpx_alloc_t *ac,
                           pk_demo_gpx_result_t *out,
                           const char **err);

/*
 * 列出目录下的 .gpx（不递归），按文件名做 ASCII 升序排序，返回个数。
 *
 * **排序而不是"随便挑一个"**：readdir 的返回序在 FAT 上取决于目录项的物理
 * 顺序，用户删一个再拷一个就可能换人。排序之后「哪个文件会被播放」是用户
 * 可预期、可控制的——想指定就把文件名改成 00-xxx.gpx。
 *
 * 只取第一个来播放是本轮的定案（设置页选轨迹要动 UI，不在本轮范围）。列表
 * 本身完整返回，将来设置页直接消费它即可，不用再改这里。
 *
 * names 为 [PK_DEMO_GPX_LIST_MAX][PK_DEMO_GPX_NAME_MAX] 的二维数组，写入的是
 * **文件名**（不含目录）。目录不存在/打不开返回 0。
 */
int pk_demo_gpx_list_dir(const char *dir,
                         char names[][PK_DEMO_GPX_NAME_MAX],
                         int max_names);

#ifdef __cplusplus
}
#endif
