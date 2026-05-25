# 内置数据库维护

英文版：[`database_maintenance.md`](database_maintenance.md)

本文说明 ESP32-P4 固件内置的航空识别数据库维护流程。这些数据库用于提升 ADS-B / Mode-S 显示质量，但它们不是航班计划、航线、时刻表、放行或实时联网数据。

## 安全边界

Pilot Kit Box 是开源态势感知和开发设备。内置数据库不能被写成或用作经过认证的航电数据、导航源或防撞数据源。

固件有意不暴露上游飞机数据库中的逐机 `flags` 字段，包括 military、VIP、PIA、LADD 等标记。刷新或替换数据源时必须保持这个边界。

## 内置资源

| 资源 | 固件路径 | 来源 | 使用位置 |
|---|---|---|---|
| 飞机 ICAO24 数据库 | `firmware/main/aircraft_db.bin`、`firmware/main/aircraft_db.c`、`firmware/main/aircraft_db.h` | `wiedehopf/tar1090-db` 飞机分片和 `icao_aircraft_types.json` | ADS-B LIST 机型、型号、注册号和详情面板 |
| 航司代码表 | `firmware/main/airline_codes.c`、`firmware/main/airline_codes.h` | Wikipedia 分字母航司代码页面，以及生成器中维护者审查过的手工补充项 | 呼号显示和运营人名称查询 |
| ICAO24 国家地址段 | `firmware/main/icao_country.c`、`firmware/main/icao_country.h` | tar1090 `flags.js`，来源为 ICAO 24-bit 地址分配 | ADS-B LIST 国家列和详情面板 |

当前仓库里的 `aircraft_db.bin` 快照使用 `PKADB1` v2 格式，包含 570141 条飞机记录、1889 个机型表项，嵌入 blob 约 8.16 MiB。这些数字是当前快照，不是固定协议上限。

## 用户更新模型

数据库刷新属于维护者流程。固件在构建时嵌入生成出来的 C 源码和二进制 blob，并作为正常固件镜像的一部分发布。

终端用户不在设备上单独更新这些数据库。如果需要更新数据，应通过网页刷机或文档中的固件发布流程安装新的完整固件。

## 更新飞机 ICAO24 数据库

先获取上游 tar1090-db 分片和 ICAO aircraft type 表：

```bash
mkdir -p /tmp/tar1090-db
cd /tmp/tar1090-db

curl -sL https://api.github.com/repos/wiedehopf/tar1090-db/contents/db \
  | python3 -c "import sys,json;[print(i['name']) for i in json.load(sys.stdin) if i['name'].endswith('.js')]" \
  | xargs -I {} -P 16 curl -sLO https://raw.githubusercontent.com/wiedehopf/tar1090-db/master/db/{}

curl -sL -o /tmp/types.json \
  https://raw.githubusercontent.com/wiedehopf/tar1090-db/master/icao_aircraft_types.json
```

重新生成固件 blob：

```bash
cd /path/to/Pilot-Kit-Box-ESP32-P4/firmware
scripts/gen_aircraft_db.py \
  --db-dir /tmp/tar1090-db \
  --types /tmp/types.json \
  --out main/aircraft_db.bin
```

生成器会按设计丢弃上游 `flags` 字段。不要把这些字段重新加入固件输出。

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

还需要复查：

- 如果嵌入文件或源码名称变化，更新 `firmware/main/CMakeLists.txt`。
- 如果数据库能力、路径或用户更新方式变化，更新 `README.md` 和 `docs/README.md`。
- 如果发布流程变化，更新 `docs/firmware_update.md` / `docs/firmware_update-zh_CN.md`。
