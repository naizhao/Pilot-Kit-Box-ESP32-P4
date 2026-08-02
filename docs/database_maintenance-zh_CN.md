# 航空识别数据库维护

英文版：[`database_maintenance.md`](database_maintenance.md)

本文说明 ESP32-P4 固件用到的航空识别数据库维护流程，既包括编进固件镜像的那几张表，也包括随 microSD 卡分发的机型库。这些数据库用于提升 ADS-B / Mode-S 显示质量，但它们不是航班计划、航线、时刻表、放行或实时联网数据。

## 安全边界

Pilot Kit Box 是开源态势感知和开发设备。这些数据库不能被写成或用作经过认证的航电数据、导航源或防撞数据源。

固件有意不暴露上游飞机数据库中的逐机 `flags` 字段，包括 military、VIP、PIA、LADD 等标记。刷新或替换数据源时必须保持这个边界。

## 各数据库存放在哪

| 资源 | 位置 | 来源 | 使用位置 |
|---|---|---|---|
| 飞机 ICAO24 数据库 | microSD `/sdcard/aero/pk_actdb.bin`；仓库内产物 `datafiles/data/pk_actdb.bin`；读取端 `firmware/main/aircraft_db.c`、`firmware/main/aircraft_db_reader.c`、`firmware/main/aircraft_db.h` | `wiedehopf/tar1090-db` 飞机分片，以及 Doc 8643 机型表 `db/icao_aircraft_types.js` | ADS-B LIST 机型、型号、注册号和详情面板 |
| 航司代码表 | 编进固件：`firmware/main/airline_codes.c`、`firmware/main/airline_codes.h` | Wikipedia 分字母航司代码页面，以及生成器中维护者审查过的手工补充项 | 呼号显示和运营人名称查询 |
| ICAO24 国家地址段 | 编进固件：`firmware/main/icao_country.c`、`firmware/main/icao_country.h` | tar1090 `flags.js`，来源为 ICAO 24-bit 地址分配 | ADS-B LIST 国家列和详情面板 |

搬到卡上的只有飞机 ICAO24 数据库。航司代码表和 ICAO24 国家地址段仍然是生成式 C 源码，照旧链进固件镜像；磁偏角表 `firmware/main/mag_var_table.h` 同理。

机型库文件是 64 字节 `PKACT1` 容器头（magic、cycle 戳、载荷 SHA-256）包着一份 `PKADB1` v2 载荷。它**不加密**（`enc_algo = 0`），因为 tar1090-db 是公开数据；卡上另一个文件——航空数据库 `/sdcard/aero/pk_aero.bin`——仍然是 AES-128-CTR 加密的。当前这份是 cycle `20260802`、574053 条飞机记录、1886 个机型表项、8,604,583 字节（8.21 MB）。这些数字是当前快照，不是固定协议上限。

两个文件故意分开：tar1090-db 是周更，航空数据走 28 天 AIRAC 周期，合在一起就得让其中一个等另一个。

## 用户更新模型

数据库刷新属于维护者流程，但两类数据到达设备的方式不同。

- **飞机 ICAO24 数据库**——重新生成 `pk_actdb.bin`，拷到卡上的 `/aero/pk_actdb.bin` 即可，**不涉及刷固件**。固件开机后把它懒加载进 PSRAM；没有卡或没有这个文件时盒子照常工作，ADS-B 的机型/型号/注册号字段显示为 `---`。
- **航司代码表和 ICAO24 国家地址段**——重新生成 C 源码、重新编译，随固件镜像发布。终端用户通过网页刷机或文档中的固件发布流程安装新固件来获得新数据。

SD 卡目录结构以及其余数据文件（`pk_aero.bin`、`/maps/*.pmtiles`）见 `datafiles/README.md`。

## 更新飞机 ICAO24 数据库

先获取上游 tar1090-db 分片和 ICAO Doc 8643 机型表。上游把机型表挪了位置：老地址（仓库根的 `icao_aircraft_types.json`）现在是 404，新地址 `db/icao_aircraft_types.js` 是 gzip 压缩的，所以要 gunzip 成纯 JSON 再喂给 `--types`。

```bash
mkdir -p /tmp/tar1090-db
cd /tmp/tar1090-db

curl -sL https://api.github.com/repos/wiedehopf/tar1090-db/contents/db \
  | python3 -c "import sys,json;[print(i['name']) for i in json.load(sys.stdin) if i['name'].endswith('.js')]" \
  | xargs -I {} -P 16 curl -sLO https://raw.githubusercontent.com/wiedehopf/tar1090-db/master/db/{}

curl -sL https://raw.githubusercontent.com/wiedehopf/tar1090-db/master/db/icao_aircraft_types.js \
  | gunzip > /tmp/types.json
```

重新生成卡上的文件。`--out` 默认就是 `<repo>/datafiles/data/pk_actdb.bin`，可以不写：

```bash
cd /path/to/Pilot-Kit-Box-ESP32-P4
firmware/scripts/gen_aircraft_db.py \
  --db-dir /tmp/tar1090-db \
  --types /tmp/types.json
```

然后把产物拷到卡上的 `/aero/pk_actdb.bin`。文件名必须一模一样——这个路径是编译期常量（`firmware/main/aircraft_db.c` 的 `ACTDB_PATH`）。刷新机型库不需要重新编译或重刷固件。

生成器会按设计丢弃上游 `flags` 字段。不要把这些字段重新加入输出。

## 更新航司代码表

航司生成器通过 MediaWiki API 获取 Wikipedia wikitext，把页面缓存到 `/tmp/pkb_airlines_cache`，再合并维护者审查过的 `MANUAL_ADDITIONS`，并可以直接替换仓库中的 C 表。

```bash
cd firmware
scripts/gen_airline_codes.py \
  --letters all \
  --update-source main/airline_codes.c
```

CI 或维护者自检：

```bash
cd firmware
scripts/gen_airline_codes.py \
  --letters all \
  --update-source main/airline_codes.c \
  --check
```

脚本按每秒一次请求限速。大 diff 必须人工审查，重点看航司改名、历史/停运运营人，以及手工补充项是否仍然合理。

## 更新 ICAO24 国家地址段

国家地址段生成器默认抓取 tar1090 `flags.js`，并输出 `main/icao_country.c`：

```bash
cd firmware
scripts/gen_icao_country.py --out main/icao_country.c
```

如果需要审计或离线更新，可以先下载源文件：

```bash
cd firmware
scripts/gen_icao_country.py \
  --source-file /tmp/flags.js \
  --out main/icao_country.c
```

CI 或维护者自检：

```bash
cd firmware
scripts/gen_icao_country.py --check --out main/icao_country.c
```

生成后的查询逻辑会返回最具体的匹配地址段，因此大国家段内的小属地分配会覆盖父级地址段。如果上游 flag code 不是有效的 ISO 3166-1 alpha-2 国家代码，生成器也会保留明确的 ISO 修正项。

## 构建与发布检查

任何数据库刷新后都要执行：

```bash
cd firmware
python3 -m py_compile scripts/gen_aircraft_db.py scripts/gen_airline_codes.py scripts/gen_icao_country.py
./build.sh build
```

只有航司表和国家表需要重新编译；机型库是卡上的文件，不影响固件镜像。

还需要复查：

- 如果源码名称变化，更新 `firmware/main/CMakeLists.txt`。
- 如果 SD 卡目录结构、文件名或生成器默认值变化，更新 `datafiles/README.md`。
- 如果数据库能力、路径或用户更新方式变化，更新 `README.md` 和 `docs/README.md`。
- 如果发布流程变化，更新 `docs/firmware_update.md` / `docs/firmware_update-zh_CN.md`。
