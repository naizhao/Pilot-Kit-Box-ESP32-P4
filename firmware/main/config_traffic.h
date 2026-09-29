/*
 * config_traffic.h — Traffic 显示偏好(地图/雷达朝向 + 雷达量程) + NVS 持久化。
 *
 * NVS namespace: "pk_tfc"  keys: "orient"(u8) / "radar_orient"(u8) / "range"(u8)
 * 结构照 config_qnh.c，NVS 类型用 u8 照 i18n.c（枚举/索引比 blob 合适）。
 */
#pragma once

typedef enum { PK_MAP_HEADING_UP = 0, PK_MAP_NORTH_UP = 1 } pk_map_orient_t;

/*
 * 地图页朝向（设置页"地图朝向"行 + 地图页左下按钮）。设置页行与地图语义
 * 同名同源。NVS 键沿用最初的 "orient"——它在分拆前就是唯一那份，存量用户
 * 的选择自然归地图页。
 */
pk_map_orient_t pk_map_orient_get(void);
void            pk_map_orient_set(pk_map_orient_t m);   /* 立即 NVS 持久化 */

/*
 * 雷达页（交通雷达）朝向。2026-09-29 从 pk_map_orient 分拆：两页的"机头
 * 朝上"说的是不同的事（雷达=绕本机转的防撞视角，地图=旋转底图对航路），
 * 强制共享会让一页的切换踩掉另一页的选择。新 NVS 键 "radar_orient"，
 * 默认 HEADING_UP（雷达页出厂行为）。
 */
pk_map_orient_t pk_radar_orient_get(void);
void            pk_radar_orient_set(pk_map_orient_t m); /* 立即 NVS 持久化 */

/* 量程档：索引 0..3 → 2/5/10/20 NM */
int  pk_traffic_range_idx_get(void);     /* 0..3，默认 1(=5NM) */
void pk_traffic_range_idx_set(int idx);  /* 钳位 0..3，立即 NVS */
int  pk_traffic_range_nm(int idx);       /* 索引→NM，{2,5,10,20}[idx] */

void pk_config_traffic_load(void);       /* 开机加载各值，默认 HEADING_UP + 20NM */
