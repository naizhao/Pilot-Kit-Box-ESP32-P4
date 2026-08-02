/*
 * aircraft_db.h — ICAO 24-bit address → aircraft type / model / registration.
 *
 * The 24-bit ICAO transponder address uniquely identifies a registered
 * aircraft worldwide. It is NOT broadcast inside the ADS-B payload, but
 * is the cleartext header of every Mode-S frame. This module maps that
 * address to:
 *   - ICAO type designator (e.g. "B738", "A320", "EC35")
 *   - canonical human-readable model name (e.g. "BOEING 737-800")
 *   - ICAO Doc 8643 technical descriptor (e.g. "L2J" = Land 2-Jet)
 *   - aircraft registration tail (e.g. "B-5797", "N12345", "9V-MBH")
 *
 * Data source: tar1090-db (github.com/wiedehopf/tar1090-db) — mictronics'
 * aggregated dump of ~600 k observed aircraft. 存放在 SD 卡上
 * （`/sdcard/aero/pk_actdb.bin`，8.16 MB，懒加载进 PSRAM），由
 * `firmware/scripts/gen_aircraft_db.py` 离线生成。文件格式见该脚本的
 * 文档字符串与 aircraft_db_reader.h。
 *
 * 为什么在卡上而不在固件里：tar1090-db 周更，而 SD 上的航空数据是 28 天
 * 的 AIRAC 周期，两者节奏不同必须分开；放卡上才能单独换文件而不走 OTA。
 * 产品形态上 SD 是必插的（地图、航空数据都在卡上），所以"无卡"不是
 * 需要额外照顾的场景——降级行为就是查不到，显示 ICAO24。
 *
 * What we deliberately DON'T expose: the per-aircraft `flags` field
 * (military / VIP / PIA / LADD bits). Identifying military aircraft from
 * a civilian receiver is legally fraught in some jurisdictions (specifically
 * PRC — military aircraft surveillance is treated as espionage under
 * domestic law). The firmware never embeds or surfaces those bits.
 *
 * Lookup is O(log n) binary search over an ICAO24-sorted record array.
 * ~20 µs worst case at ~570 k entries.
 *
 * 单条 last-lookup 缓存（见 aircraft_db.c）：连续查同一架飞机的
 * code/model/desc/registration 只做 1 次二分，其余 3 次走缓存。
 *
 * 线程模型：查询只允许从 UI 渲染任务调用（现实调用点全在 pfd.c 的渲染
 * 路径上：adsb_list.c 详情面板、traffic_page.c 详情条）。库缓冲本身有锁
 * 保护，但返回的指针指向模块内的单条缓存，跨任务并发调用会互相踩。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * 一次查询返回的全部字段。定容拷贝而非指针——数据源是 SD 卡载入 PSRAM
 * 的缓冲，拔卡即释放，再往外发指针就是 use-after-free。
 * 定容按当前上游（tar1090-db 2026-05）的实测最长值取：reg 17、code 5、
 * model 47、desc 3 字符。超长截断。
 */
typedef struct {
    char reg[20];      /* "B-5797" / "N12345"；空串 = 无 */
    char code[8];      /* ICAO 机型代号 "B738"；空串 = 无 */
    char model[48];    /* "BOEING 737-800"；空串 = 无 */
    char desc[8];      /* Doc 8643 描述符 "L2J"；空串 = 无 */
} pk_aircraft_info_t;

/*
 * 启动 SD 卡机型库的后台懒加载任务（/sdcard/aero/pk_actdb.bin）。
 * 开机路径零阻塞：本函数只建任务就返回，加载在后台核 1 上做。
 * 未就绪（无卡 / 无文件 / 加载中 / 文件坏）时所有查询返回"查不到"。
 */
void pk_aircraft_db_init(void);

/*
 * 一次二分取全部字段。命中返回 true 并填满 *out（缺的字段是空串），
 * 未命中（含库未就绪）返回 false 并把 *out 清空。
 *
 * 这是首选入口：下面四个单字段版本每个都要一次「查缓存/二分」，而本函数
 * 一次拿全。四个旧入口保留只是为了不动既有调用点。
 */
bool pk_aircraft_lookup(uint32_t icao24, pk_aircraft_info_t *out);

/*
 * 以下四个单字段入口全部走 pk_aircraft_lookup 的同一份缓存。
 *
 * ── 指针生命周期（相对 flash 时代收紧了，务必看）──────────────────
 * 返回的指针指向模块内的单条 last-lookup 缓存，**只保证到下一次查询
 * 另一架飞机之前有效**（原先是"程序生命周期"，因为那时指进 flash）。
 * 用完立刻 snprintf/strcpy 走，别跨帧持有。现有两个调用点
 * （adsb_list.c:902-906、traffic_page.c:516-518）拿到就 snprintf，符合。
 *
 * Resolve ICAO24 → ICAO type designator (e.g. "B738"). NULL if the
 * aircraft isn't in the database OR is in the database with only a
 * registration (no type code) OR the database isn't loaded.
 */
const char *pk_aircraft_type_code(uint32_t icao24);

/*
 * Resolve ICAO24 → canonical model name for the aircraft's type (e.g.
 * "BOEING 737-800"). Same NULL / lifetime semantics as the code variant.
 * One model name per type — chosen as the most common model string
 * observed for that type across all aircraft in the dataset.
 */
const char *pk_aircraft_type_model(uint32_t icao24);

/*
 * Resolve ICAO24 → ICAO Doc 8643 technical descriptor (e.g. "L2J" =
 * Land 2-Jet, "H2T" = Helicopter 2-Turboprop). Three-char string.
 * NULL if the aircraft has no type code OR its type isn't in the
 * descriptor table (rare — 96% of types have a desc).
 */
const char *pk_aircraft_type_desc(uint32_t icao24);

/*
 * Resolve ICAO24 → aircraft registration tail (e.g. "B-5797", "N12345").
 * NULL if the aircraft is in the database without a registration field
 * OR isn't in the database at all. Note that an aircraft can have a
 * registration but no type code, in which case pk_aircraft_type_code()
 * returns NULL while this returns the tail.
 */
const char *pk_aircraft_registration(uint32_t icao24);
