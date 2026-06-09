/* mag_var.h — 磁偏角(磁偏角)查表。给定经纬度返回磁偏角，东偏为正。
 *
 * 数据来自 mag_var_table.h（5° WMM 网格，gen_mag_var.py 生成），运行时
 * 双线性插值。用于把 GPS 算出的真北方位降到磁系，与 IMU 磁航向对齐。 */
#pragma once

float pk_mag_var_lookup(double lat_deg, double lon_deg);  /* 东偏+，西偏- */
