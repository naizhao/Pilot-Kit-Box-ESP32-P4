/* qnh_math.c — 见 qnh_math.h。纯数学，无任何平台依赖。 */
#include "qnh_math.h"

#include <math.h>

/* 与 baro_task.c 保持同一组标准大气常数。0.190295 = 1/5.255（ISA 温度梯度）。 */
#define QNH_ISA_H0_M      44330.0f
#define QNH_ISA_EXP       0.190295f

#define QNH_DEFAULT_HPA   1013.25f
#define QNH_MIN_HPA       950.0f
#define QNH_MAX_HPA       1050.0f

float pk_qnh_from_pressure_alt(float pressure_pa, float alt_m)
{
    /* 底数必须 > 0：alt >= 44330 或 P <= 0 都无意义，退回标准海压。 */
    float base = 1.0f - alt_m / QNH_ISA_H0_M;
    if (!(pressure_pa > 0.0f) || !(base > 0.0f)) return QNH_DEFAULT_HPA;

    /* QNH = P / base^(1/0.190295) */
    float qnh_pa = pressure_pa / powf(base, 1.0f / QNH_ISA_EXP);
    float qnh_hpa = qnh_pa / 100.0f;

    if (!(qnh_hpa > QNH_MIN_HPA)) return QNH_MIN_HPA;
    if (qnh_hpa > QNH_MAX_HPA)    return QNH_MAX_HPA;
    return qnh_hpa;
}
