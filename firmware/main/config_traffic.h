/*
 * config_traffic.h — Traffic 显示偏好(地图朝向 + 雷达量程) + NVS 持久化。
 *
 * NVS namespace: "pk_tfc"  keys: "orient"(u8) / "range"(u8 索引)
 * 结构照 config_qnh.c，NVS 类型用 u8 照 i18n.c（枚举/索引比 blob 合适）。
 */
#pragma once

typedef enum { PK_MAP_HEADING_UP = 0, PK_MAP_NORTH_UP = 1 } pk_map_orient_t;

pk_map_orient_t pk_map_orient_get(void);
void            pk_map_orient_set(pk_map_orient_t m);   /* 立即 NVS 持久化 */

/* 量程档：索引 0..3 → 2/5/10/20 NM */
int  pk_traffic_range_idx_get(void);     /* 0..3，默认 1(=5NM) */
void pk_traffic_range_idx_set(int idx);  /* 钳位 0..3，立即 NVS */
int  pk_traffic_range_nm(int idx);       /* 索引→NM，{2,5,10,20}[idx] */

void pk_config_traffic_load(void);       /* 开机加载两值，默认 HEADING_UP + 5NM */
