# datafiles/ — 盒子离线数据工作区

英文版：[`README.md`](README.md)


盒子跑起来要读的离线数据，统一放这里，不再散落在 `tmp/`、`firmware/main/` 各处。

**这些文件不进 git**（根 `.gitignore` 里忽略 `datafiles/**/*.bin` 与
`datafiles/**/*.pmtiles`），只有目录结构和这份说明进。理由是体积：四个底图包
合计 3.4 GB，单包最大 1.4 GB；数据 bin 每个 8~17 MB。全部是脚本可再生的产物，
没有一个是手写的源码。

```
datafiles/
├── README.md      ← 本文件
├── data/          航空/机型数据 bin，对应 SD 卡的 /aero/
└── maps/          离线底图 pmtiles 包，对应 SD 卡的 /maps/
```

---

## SD 卡上的目录结构

盒子把卡挂在 `/sdcard`，只认下面这几个固定路径（都是固件里写死的常量）：

| SD 卡路径 | 本仓库来源 | 固件里的常量 |
|---|---|---|
| `/aero/pk_aero.bin`  | `datafiles/data/pk_aero.bin`  | `AERO_BIN_PATH`（`firmware/main/pk_aero_db.c:56`） |
| `/aero/pk_actdb.bin` | `datafiles/data/pk_actdb.bin` | `ACTDB_PATH`（`firmware/main/aircraft_db.c:67`） |
| `/maps/*.pmtiles`    | `datafiles/maps/*.pmtiles`    | `MAP_DIR`（`firmware/main/pk_tile_loader.c:30`） |

刷卡就是把 `data/` 里的两个现役 bin 拷进 `/aero/`、`maps/` 里的包拷进 `/maps/`，
文件名保持不变（`/maps` 是扫目录的，文件名不参与路由，但另外两个是写死的路径）。

microSD 必须插在 Slot 0——ESP-Hosted 占掉了唯一的 SDMMC 控制器，插错槽任何卡都
识别不出来。

---

## data/ — 航空 / 机型数据

| 文件 | 大小 | 格式 | 状态 |
|---|---|---|---|
| `pk_aero.bin` | 11,450,416 B (10.92 MB) | `PKAER1` v3，cycle `2026-02`，9 段 | **现役**，与盒子里那张卡上的一致 |
| `pk_actdb.bin` | 8,604,583 B (8.21 MB) | `PKACT1` 容器 + `PKADB1` v2，cycle `20260802` | **现役** |
| `pk_aero_v4.bin` | 17,438,040 B (16.63 MB) | `PKAER1` v4，cycle `2026-08`，14 段 | **固件尚未适配，先别拷进 SD 卡** |
| `aircraft_db.bin` | 8,560,452 B (8.16 MB) | 裸 `PKADB1` v2 载荷（无容器），570,141 条 | **历史遗留，已不参与构建** |

### pk_aero.bin — 航空数据（机场/跑道/导航台/航路点/空域）

生成端不在本仓库：是 Pilot-Kit 仓的
`scripts/aero_data_pipeline/export_box_bin.py`。格式的权威定义以那个脚本为准，
本仓库这侧的解析器是 `firmware/main/pk_aero_reader.c/.h`，两边靠文件头注释对齐。
按 AIRAC 28 天周期更新。

**v4 那份先别用**：`firmware/main/pk_aero_reader.h:26` 写着 init 只接受
`version ∈ {2, 3}`，v4 的 header 里 version=4、段数从 9 涨到 14，直接拷进卡里
固件会拒绝加载。留在这里是为了固件适配时手边有一份真实样本。

### pk_actdb.bin — ICAO24 机型库

本仓库自己出：

```bash
# 1) 抓上游 tar1090-db（脚本在 --db-dir 不存在时会把这段配方打出来）
mkdir -p /tmp/tar1090-db && cd /tmp/tar1090-db
curl -sL https://api.github.com/repos/wiedehopf/tar1090-db/contents/db \
  | python3 -c "import sys,json;[print(i['name']) for i in json.load(sys.stdin) if i['name'].endswith('.js')]" \
  | xargs -I {} -P 16 curl -sLO https://raw.githubusercontent.com/wiedehopf/tar1090-db/master/db/{}
curl -sL https://raw.githubusercontent.com/wiedehopf/tar1090-db/master/db/icao_aircraft_types.js \
  | gunzip > /tmp/types.json

# 2) 出包（--out 默认就是 datafiles/data/pk_actdb.bin，不用写）
firmware/scripts/gen_aircraft_db.py --db-dir /tmp/tar1090-db --types /tmp/types.json
```

上游那张 Doc 8643 类型表挪过位置：老地址（仓库根的 `icao_aircraft_types.json`）
现在是 404，新地址在 `db/icao_aircraft_types.js` 且是 gzip，所以上面要 `gunzip`
成纯 JSON 再喂给 `--types`。tar1090-db 周更，与 pk_aero.bin 的 28 天 AIRAC 周期
不同步——两个文件故意分开，就是不想让其中一个等另一个。

### aircraft_db.bin — 已退役

原先走 `EMBED_FILES` 嵌进固件 `.rodata`，占掉 factory 分区一大块。机型库改到 SD
卡懒加载后（见 `firmware/main/aircraft_db.c` 文件头），`EMBED_FILES` 那行已删，
全仓已无 `_binary_aircraft_db_bin` 引用，分区占用率从 91% 降到 23%。

它是**裸载荷**、没有 64 字节容器头，与 `pk_actdb.bin` 不是同一种文件，不能拷进
SD 卡。留着只有一个用途：新出的库跟这份老快照做 diff 对账（`gen_aircraft_db.py
--no-container` 出来的才是可比对的同构文件）。确认不再需要对账即可删。

---

## maps/ — 离线底图

PMTiles 格式的栅格底图包，深色主题。固件启动扫 `/sdcard/maps`，按 (z,x,y) 在多
个包之间路由：重叠区选 maxzoom 最深的那个，够不到就用父瓦片放大（overzoom）并
打提示徽标。

| 文件 | 大小 | 覆盖 |
|---|---|---|
| `pk_map_global.pmtiles` | 584,152,227 B (557 MB) | 全球 z0-9 |
| `pk_map_cn.pmtiles` | 1,459,408,344 B (1.36 GB) | 中国 z10-12 |
| `pk_map_us_conus.pmtiles` | 1,365,682,229 B (1.27 GB) | 美国本土 z10-12 |
| `pk_map_prd_pilot.pmtiles` | 14,016,795 B (13.4 MB) | 珠三角试点 z0-12，bounds `112.5,21.5,114.6,23.5` |

出包链路：tileserver-gl 渲染 → MBTiles → `pmtiles convert`。也可以直接从已刷好
的 SD 卡 `/maps/` 拷回来。

---

## 谁会读这个目录

移动这些文件之前先看一眼，下面几处的默认路径都指着这里：

| 位置 | 默认值 | 覆盖方式 |
|---|---|---|
| `firmware/test/test_pk_pmtiles.c` | `datafiles/maps` | 环境变量 `PK_MAP_TEST_DATA_DIR` |
| `firmware/test/test_pk_map_store.c` | `datafiles/maps` | 环境变量 `PK_MAP_TEST_DATA_DIR` |
| `sim/compat/pk_tile_loader_sim.c` | `datafiles/maps` | 环境变量 `PK_SIM_MAPS_DIR` |
| `sim/capture.py` | `<repo>/datafiles/maps` | 改脚本里的 `PK_SIM_MAPS_DIR` |
| `firmware/scripts/gen_aircraft_db.py` | `<repo>/datafiles/data/pk_actdb.bin` | `--out` |

两个 host 测试的默认值是**相对仓库根**的路径，所以那几行一行式 `cc` 命令要在仓库
根目录下跑。样本包缺失时它们整段 SKIP、不算失败——所以跑完记得看一眼输出里是不是
真的跑了用例，别把 SKIP 当通过。
