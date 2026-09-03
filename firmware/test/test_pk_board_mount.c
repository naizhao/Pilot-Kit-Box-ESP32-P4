/*
 * test_pk_board_mount.c — host 端验证 pk_board 的 V3.9 / V4.3 传感器安装变换。
 *
 * 为什么需要它
 * ------------
 * 安装变换是一类**没有单元可查、错了也照样出数**的常量：装反 90°，PFD 照样
 * 有姿态、照样跟着动，只是全错了。而这次更糟——V3.9 与 V4.3 是两块**物理方向
 * 不同**的板：
 *
 *     器件            V3.9    V4.3
 *     U4 BNO085         0°     +90°
 *     U5 BMP388         0°     -90°
 *     U6 QMC5883P       0°     -90°
 *     U7 ATGM336H       0°       0°
 *
 * U4 与 U5/U6 转向**相反**，所以「整板旋转 90°」这种建模一定是错的；两颗姿态
 * 相关器件（BNO085 / QMC5883P）必须各自拥有独立的 board-to-body 变换。选错
 * profile 必须在这里变红，而不是等上了飞机才发现地平仪反了。
 *
 * 物理判据链（每一环都可复核，不引用被测代码）
 * --------------------------------------------
 * 1. 封装旋转 —— 从两版 kicad_pcb 的 footprint `(at x y rot)` 实测：
 *      hardware/expansion-board-v3/kicad/expansion-board-v3.kicad_pcb
 *        U4 (63.7,78.3) rot=0    U5 (71.3,81.2) rot=0
 *        U6 (71.3,77)   rot=0    U7 (70.4,66.4) rot=0     全部 F.Cu
 *      hardware/expansion-board-v4/kicad/expansion-board-v4.kicad_pcb
 *        U4 (59.69,84.455) rot=90    U5 (60.96,88.9) rot=-90
 *        U6 (59.69,96.52) rot=-90    U7 (73.66,89.09) rot=0  全部 F.Cu
 *    KiCad 正角 = 俯视图逆时针（与 hardware/tools/decoupling_check.py:57-72
 *    的 `a = radians(-rot)` 变换一致：rot=+90 把局部 (1,0) 映到板 (0,-1)）。
 *
 * 2. 扩展板在盒子里的朝向 —— 用 J1 排针焊盘绝对坐标与底板实测值对拍得到：
 *      base_x = 151.25 - exp_x（X 镜像）、base_y = exp_y - 51（Y 不镜像）
 *    代入后 J1 两排引脚轴 Y = 54.06 / 56.60、X 跨距 34.54..82.80，与
 *    hardware/expansion-board-v3/BASEBOARD_REF-zh_CN.md 的卡尺实测逐位吻合。
 *    X 镜像说明扩展板 F.Cu 朝向底板/屏幕一侧。
 *
 * 3. 装配姿态 —— 2026-09-03 用户机械裁定：盒子竖立、屏幕面向飞行员；
 *    J1 排针（F 面）朝机尾（朝飞行员），B 面（电池面）朝机头；
 *    J1 那条长边在盒子下边（靠仪表台）。于是（NED 机体系 +X 前 / +Y 右 / +Z 下）：
 *      +X_kicad（俯视图右）  → 机体右   (0,+1,0)
 *      +Y_kicad（俯视图下）  → 机体下   (0,0,+1)
 *      +Z_kicad（F.Cu 外法线）→ 机体后   (-1,0,0)
 *
 * 4. BNO085 芯片轴 —— hardware/datasheets/BNO085.pdf：
 *      第 40 页 Figure 4-1：pin1 标识点位于芯片 (-X, +Y) 角，+Z 出芯片顶面；
 *      第 52 页 Figure 7-1 TOP VIEW：PIN 1 CORNER 在左上角，水平向为 5.20
 *      长边、垂直向为 3.80 短边。
 *    两图联立 ⇒ 芯片 +X 沿 5.20 长边指向图右、+Y 沿 3.80 短边指向图上。
 *    项目自绘封装 expansion-board-v3:BNO085_LGA-28 的 pin1 在局部 (+1.55,-2.25)
 *    （封装 X 半宽 1.55=短边、Y 半高 2.325=长边）⇒ rot=0 时
 *      芯片 +X → +Y_kicad、芯片 +Y → +X_kicad、芯片 +Z → +Z_kicad。
 *
 * 5. QMC5883P —— hardware/datasheets/QMC5883P.pdf Rev A **没有任何轴向图**
 *    （第 3 页只有方框图、第 6 页只有 pin 表和封装图、第 8 页只有 land pattern）。
 *    芯片敏感轴与 pin1 的关系无法从手册推导，只能上板标定。因此本文件对
 *    QMC5883P 只断言**封装坐标系**的两版差异（这一条纯由封装旋转决定，可证），
 *    并要求实现把「封装轴 → 芯片敏感轴」这一段显式标记为未标定。
 *
 * 独立性
 * ------
 * 下面 bno_chip_axes_in_body() / mag_pkg_axes_in_body() 里的三列向量是按上述
 * 1+3+4 直接写死的**物理事实**，不调用、不引用 pk_board 的任何常量或矩阵。
 * pk_board 里的实现走的是 R_board→body · Rz(封装旋转) · R_pkg→board0 三段式；
 * 两条路径独立算到同一答案才算过。
 *
 * 编译运行（不需要 ESP-IDF）：
 *     cc -std=c11 -Wall -Wextra -O2 -I firmware/main -I sim/compat \
 *        -o /tmp/test_pk_board_mount firmware/test/test_pk_board_mount.c \
 *        firmware/main/pk_board.c -lm && /tmp/test_pk_board_mount
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "pk_board.h"
/* q_world_fix（ENU→NED）不是板级属性，它跟 quat_to_euler() 的欧拉约定绑定，
 * 所以留在 imu_task.h。这里 include 它只为拿这四个常量；imu_task.h 会拉
 * esp_err.h / driver/i2c_master.h，用 sim/compat 下的桩满足（-I sim/compat）。 */
#include "imu_task.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- imu_task.c 的纯数学副本（那边是 static，链不进来）--------------- */

static inline void quat_mul(float aw, float ax, float ay, float az,
                            float bw, float bx, float by, float bz,
                            float *ow, float *ox, float *oy, float *oz)
{
    *ow = aw * bw - ax * bx - ay * by - az * bz;
    *ox = aw * bx + ax * bw + ay * bz - az * by;
    *oy = aw * by - ax * bz + ay * bw + az * bx;
    *oz = aw * bz + ax * by - ay * bx + az * bw;
}

static void quat_to_euler(float qi, float qj, float qk, float qw,
                          float *roll_deg, float *pitch_deg, float *yaw_deg)
{
    float sinr_cosp = 2.0f * (qw * qi + qj * qk);
    float cosr_cosp = 1.0f - 2.0f * (qi * qi + qj * qj);
    *roll_deg = atan2f(sinr_cosp, cosr_cosp) * (180.0f / (float)M_PI);

    float sinp = 2.0f * (qw * qj - qk * qi);
    if (sinp >  1.0f) sinp =  1.0f;
    if (sinp < -1.0f) sinp = -1.0f;
    *pitch_deg = asinf(sinp) * (180.0f / (float)M_PI);

    float siny_cosp = 2.0f * (qw * qk + qi * qj);
    float cosy_cosp = 1.0f - 2.0f * (qj * qj + qk * qk);
    float y = atan2f(siny_cosp, cosy_cosp) * (180.0f / (float)M_PI);
    if (y < 0) y += 360.0f;
    *yaw_deg = y;
}

/* parse_rotation_vector() 的 sandwich：q_aircraft = q_world_fix · q_bno · q_body_fix。
 * q_world_fix 是 ENU→NED 的固定常量，与安装无关；q_body_fix 才是 profile 给的。 */
static void sandwich_to_euler(const float q_bno[4], const float q_body_fix[4],
                              float *roll, float *pitch, float *yaw)
{
    float tw, ti, tj, tk;
    quat_mul(PK_IMU_WORLD_FIX_W, PK_IMU_WORLD_FIX_X,
             PK_IMU_WORLD_FIX_Y, PK_IMU_WORLD_FIX_Z,
             q_bno[0], q_bno[1], q_bno[2], q_bno[3],
             &tw, &ti, &tj, &tk);
    float aw, ai, aj, ak;
    quat_mul(tw, ti, tj, tk,
             q_body_fix[0], q_body_fix[1], q_body_fix[2], q_body_fix[3],
             &aw, &ai, &aj, &ak);
    quat_to_euler(ai, aj, ak, aw, roll, pitch, yaw);
}

/* ---- 物理事实（本测试的输入基准，不使用被测实现）-------------------- */

/*
 * R_chip→aircraft：BNO085 芯片三轴在 NED 机体系里分别指向哪，按列排。
 * 由文件头判据 1（封装旋转）+ 3（装配朝向）+ 4（手册轴向）推得：
 *
 *   V3.9（rot=0）  芯片+X → +Y_kicad → 机体下(0,0,1)
 *                  芯片+Y → +X_kicad → 机体右(0,1,0)
 *                  芯片+Z → +Z_kicad → 机体后(-1,0,0)
 *
 *   V4.3（rot=+90，俯视逆时针 90°：+Y_kicad→+X_kicad、+X_kicad→-Y_kicad）
 *                  芯片+X → +X_kicad → 机体右(0,1,0)
 *                  芯片+Y → -Y_kicad → 机体上(0,0,-1)
 *                  芯片+Z → +Z_kicad → 机体后(-1,0,0)
 */
static void bno_chip_axes_in_body(pk_board_profile_t profile, float m[3][3])
{
    float Xc[3], Yc[3], Zc[3];
    if (profile == PK_BOARD_PROFILE_V3) {
        float x[3] = {  0.0f, 0.0f,  1.0f };   /* 芯片 +X → 机体下 */
        float y[3] = {  0.0f, 1.0f,  0.0f };   /* 芯片 +Y → 机体右 */
        float z[3] = { -1.0f, 0.0f,  0.0f };   /* 芯片 +Z → 机体后 */
        memcpy(Xc, x, sizeof x); memcpy(Yc, y, sizeof y); memcpy(Zc, z, sizeof z);
    } else {
        float x[3] = {  0.0f, 1.0f,  0.0f };   /* 芯片 +X → 机体右 */
        float y[3] = {  0.0f, 0.0f, -1.0f };   /* 芯片 +Y → 机体上 */
        float z[3] = { -1.0f, 0.0f,  0.0f };   /* 芯片 +Z → 机体后 */
        memcpy(Xc, x, sizeof x); memcpy(Yc, y, sizeof y); memcpy(Zc, z, sizeof z);
    }
    for (int r = 0; r < 3; ++r) {
        m[r][0] = Xc[r];
        m[r][1] = Yc[r];
        m[r][2] = Zc[r];
    }
}

/*
 * QMC5883P 的**封装坐标系**（不是敏感轴——手册没给）在机体系里的指向。
 * 封装系定义：Px = 俯视图右、Py = 俯视图上、Pz = F.Cu 外法线，随封装一起转。
 *
 *   V3.9（rot=0）   Px → +X_kicad → 机体右(0,1,0)
 *                   Py → -Y_kicad → 机体上(0,0,-1)
 *                   Pz → +Z_kicad → 机体后(-1,0,0)
 *
 *   V4.3（rot=-90，俯视顺时针 90°：+X_kicad→+Y_kicad、-Y_kicad→+X_kicad）
 *                   Px → +Y_kicad → 机体下(0,0,1)
 *                   Py → +X_kicad → 机体右(0,1,0)
 *                   Pz → +Z_kicad → 机体后(-1,0,0)
 */
static void mag_pkg_axes_in_body(pk_board_profile_t profile, float m[3][3])
{
    float Xp[3], Yp[3], Zp[3];
    if (profile == PK_BOARD_PROFILE_V3) {
        float x[3] = {  0.0f, 1.0f,  0.0f };
        float y[3] = {  0.0f, 0.0f, -1.0f };
        float z[3] = { -1.0f, 0.0f,  0.0f };
        memcpy(Xp, x, sizeof x); memcpy(Yp, y, sizeof y); memcpy(Zp, z, sizeof z);
    } else {
        float x[3] = {  0.0f, 0.0f,  1.0f };
        float y[3] = {  0.0f, 1.0f,  0.0f };
        float z[3] = { -1.0f, 0.0f,  0.0f };
        memcpy(Xp, x, sizeof x); memcpy(Yp, y, sizeof y); memcpy(Zp, z, sizeof z);
    }
    for (int r = 0; r < 3; ++r) {
        m[r][0] = Xp[r];
        m[r][1] = Yp[r];
        m[r][2] = Zp[r];
    }
}

/* R_aircraft→ENU：机体三轴（前/右/下）在 ENU 世界系中的方向，按列排。 */
static void aircraft_axes_in_enu(const float fwd[3], const float right[3],
                                 const float down[3], float m[3][3])
{
    for (int r = 0; r < 3; ++r) {
        m[r][0] = fwd[r];
        m[r][1] = right[r];
        m[r][2] = down[r];
    }
}

static void mat_mul(const float a[3][3], const float b[3][3], float o[3][3])
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 3; ++k) s += a[r][k] * b[k][c];
            o[r][c] = s;
        }
}

/* 旋转矩阵 → 四元数（Shepperd 分支法）。输出 (w,x,y,z)。 */
static void mat_to_quat(const float m[3][3], float q[4])
{
    float tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q[0] = 0.25f * s;
        q[1] = (m[2][1] - m[1][2]) / s;
        q[2] = (m[0][2] - m[2][0]) / s;
        q[3] = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float s = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q[0] = (m[2][1] - m[1][2]) / s;
        q[1] = 0.25f * s;
        q[2] = (m[0][1] + m[1][0]) / s;
        q[3] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        float s = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q[0] = (m[0][2] - m[2][0]) / s;
        q[1] = (m[0][1] + m[1][0]) / s;
        q[2] = 0.25f * s;
        q[3] = (m[1][2] + m[2][1]) / s;
    } else {
        float s = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q[0] = (m[1][0] - m[0][1]) / s;
        q[1] = (m[0][2] + m[2][0]) / s;
        q[2] = (m[1][2] + m[2][1]) / s;
        q[3] = 0.25f * s;
    }
}

/*
 * 给定机体在 ENU 中的姿态，算出 BNO085 此刻应当输出的 Rotation Vector。
 * Rotation Vector 的定义就是 R_chip→ENU（Android 约定：world=ENU，device=芯片体轴）：
 *     R_chip→ENU = R_aircraft→ENU · R_chip→aircraft
 */
static void bno_quat_for_pose(pk_board_profile_t profile,
                              const float fwd[3], const float right[3],
                              const float down[3], float q_bno[4])
{
    float r_ac[3][3], r_ae[3][3], r_ce[3][3];
    bno_chip_axes_in_body(profile, r_ac);
    aircraft_axes_in_enu(fwd, right, down, r_ae);
    mat_mul(r_ae, r_ac, r_ce);
    mat_to_quat(r_ce, q_bno);
}

/* ---- 断言 ----------------------------------------------------------- */

static int g_fail;
static int g_checks;

static void check_ang(const char *what, float got, float want, float tol)
{
    float d = got - want;
    while (d >  180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    bool ok = fabsf(d) <= tol;
    g_checks++;
    printf("  %-40s got %+8.2f  want %+8.2f   %s\n",
           what, (double)got, (double)want, ok ? "OK" : "*** FAIL ***");
    if (!ok) g_fail++;
}

static void check_num(const char *what, float got, float want, float tol)
{
    bool ok = fabsf(got - want) <= tol;
    g_checks++;
    printf("  %-40s got %+8.4f  want %+8.4f   %s\n",
           what, (double)got, (double)want, ok ? "OK" : "*** FAIL ***");
    if (!ok) g_fail++;
}

static void check_true(const char *what, bool got)
{
    g_checks++;
    printf("  %-40s %s\n", what, got ? "OK" : "*** FAIL ***");
    if (!got) g_fail++;
}

static void attitude_case(pk_board_profile_t pose_profile,
                          pk_board_profile_t used_profile,
                          const char *name,
                          const float fwd[3], const float right[3],
                          const float down[3],
                          float want_roll, float want_pitch, float want_yaw)
{
    float q_bno[4];
    bno_quat_for_pose(pose_profile, fwd, right, down, q_bno);

    float q_body_fix[4];
    pk_board_imu_body_fix_quat(used_profile, q_body_fix);

    float roll, pitch, yaw;
    sandwich_to_euler(q_bno, q_body_fix, &roll, &pitch, &yaw);
    printf("%s\n  q_bno(w,i,j,k) = %+.4f %+.4f %+.4f %+.4f\n",
           name, (double)q_bno[0], (double)q_bno[1],
           (double)q_bno[2], (double)q_bno[3]);
    check_ang("roll",  roll,  want_roll,  0.05f);
    check_ang("pitch", pitch, want_pitch, 0.05f);
    check_ang("yaw",   yaw,   want_yaw,   0.05f);
    printf("\n");
}

static float wrap180(float d)
{
    while (d >  180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

/* 用错 profile 时，把同一物理姿态喂进去，逐轴算出偏差。 */
static void attitude_error(pk_board_profile_t pose_profile,
                           pk_board_profile_t used_profile,
                           const float fwd[3], const float right[3],
                           const float down[3],
                           float want_roll, float want_pitch, float want_yaw,
                           float err[3])
{
    float q_bno[4], q_body_fix[4], roll, pitch, yaw;
    bno_quat_for_pose(pose_profile, fwd, right, down, q_bno);
    pk_board_imu_body_fix_quat(used_profile, q_body_fix);
    sandwich_to_euler(q_bno, q_body_fix, &roll, &pitch, &yaw);

    err[0] = wrap180(roll  - want_roll);
    err[1] = wrap180(pitch - want_pitch);
    err[2] = wrap180(yaw   - want_yaw);

    printf("    用 %-5s 解 %-5s 的姿态 → roll %+7.2f pitch %+7.2f yaw %+7.2f"
           "   偏差 (%+.2f, %+.2f, %+.2f)\n",
           pk_board_profile_name(used_profile),
           pk_board_profile_name(pose_profile),
           (double)roll, (double)pitch, (double)yaw,
           (double)err[0], (double)err[1], (double)err[2]);
}

/* q_delta = conj(q_body_fix[A]) · q_body_fix[B]：把 A 板的姿态用 B 板的 profile
 * 解算，等于在**机体系**里多右乘一次这个旋转。返回旋转角（度），并给出转轴。 */
static float profile_delta_rotation(pk_board_profile_t a, pk_board_profile_t b,
                                    float axis[3])
{
    float qa[4], qb[4];
    pk_board_imu_body_fix_quat(a, qa);
    pk_board_imu_body_fix_quat(b, qb);

    float dw, dx, dy, dz;
    quat_mul(qa[0], -qa[1], -qa[2], -qa[3], qb[0], qb[1], qb[2], qb[3],
             &dw, &dx, &dy, &dz);

    float w = dw;
    if (w >  1.0f) w =  1.0f;
    if (w < -1.0f) w = -1.0f;
    float ang = 2.0f * acosf(fabsf(w)) * (180.0f / (float)M_PI);

    float n = sqrtf(dx * dx + dy * dy + dz * dz);
    if (n < 1e-6f) n = 1.0f;
    axis[0] = dx / n; axis[1] = dy / n; axis[2] = dz / n;
    return ang;
}

static const float POSE_LEVEL_N_FWD[3]   = { 0, 1,  0 };
static const float POSE_LEVEL_N_RIGHT[3] = { 1, 0,  0 };
static const float POSE_LEVEL_N_DOWN[3]  = { 0, 0, -1 };

int main(void)
{
    const float d2r = (float)M_PI / 180.0f;
    const pk_board_profile_t profiles[2] = {
        PK_BOARD_PROFILE_V3, PK_BOARD_PROFILE_V4
    };

    /* === 1. 两版各自的 BNO085 姿态必须正确 ========================== */
    for (int i = 0; i < 2; ++i) {
        pk_board_profile_t p = profiles[i];
        printf("========== BNO085 姿态 · %s ==========\n",
               pk_board_profile_name(p));

        attitude_case(p, p, "[1] 水平朝北 → 0 / 0 / 0",
                      POSE_LEVEL_N_FWD, POSE_LEVEL_N_RIGHT, POSE_LEVEL_N_DOWN,
                      0, 0, 0);
        {
            float a = 10.0f * d2r;
            const float fwd[3]   = { 0,  cosf(a),  sinf(a) };
            const float right[3] = { 1,  0,        0       };
            const float down[3]  = { 0,  sinf(a), -cosf(a) };
            attitude_case(p, p, "[2] 抬头 10° → pitch +10",
                          fwd, right, down, 0, 10, 0);
        }
        {
            float a = 20.0f * d2r;
            const float fwd[3]   = { 0,        1,  0        };
            const float right[3] = { cosf(a),  0, -sinf(a)  };
            const float down[3]  = { -sinf(a), 0, -cosf(a)  };
            attitude_case(p, p, "[3] 右滚 20° → roll +20",
                          fwd, right, down, 20, 0, 0);
        }
        {
            const float fwd[3]   = { 1,  0,  0 };   /* 东 */
            const float right[3] = { 0, -1,  0 };   /* 南 */
            const float down[3]  = { 0,  0, -1 };
            attitude_case(p, p, "[4] 水平朝东 → yaw 90",
                          fwd, right, down, 0, 0, 90);
        }
    }

    /* === 2. 用错 profile 必须明确失败 ================================ *
     *
     * 这是本文件存在的核心理由：两版差 90°，选错板型不能「看起来还行」。
     *
     * 先说清楚这个误差**长什么样**，因为它决定了断言只能怎么写：
     *
     *   q_delta = conj(q_body_fix[V3.9]) · q_body_fix[V4.3]
     *           = (√2/2, √2/2, 0, 0) = 绕机体 +X 轴 90°
     *
     * q_body_fix 是右乘项，所以用错 profile 等于在**机体系**里多转一次
     * q_delta。而 ZYX Tait-Bryan 分解里绕机体 X 的旋转是最内层那一次，
     * 于是它**只加到 roll，pitch 和 yaw 一点不动**。
     *
     * 这意味着「让俯仰和航向也各自发现错误 profile」对这一对 profile 是
     * 做不到的——不是测试写弱了，是几何上如此。硬断言 pitch/yaw 跑偏等于
     * 断言一个假命题。所以这里改成**精确刻画**而不是阈值：
     *
     *   · roll  偏差恰好 ±90°（用 V4.3 解 V3.9 是 +90，反过来 −90）
     *   · pitch 偏差恰好 0
     *   · yaw   偏差恰好 0
     *
     * 这比「最大偏差 ≥45°」强：阈值断言在误差从 90° 变成 91° 或者跑到
     * 另一个轴上时都还是绿的，精确断言不会。
     *
     * 另外补一条与 profile 数量无关的通用护栏（见下面第 2b 段），这样将来
     * 真加了第三个板型、而它与现有两版的 q_delta 不是纯 roll 时，也有东西
     * 兜着。姿态集合里加了「水平朝东」，把 heading≠0 的情况也覆盖上。 */
    printf("========== 错误 profile 必须跑偏 ==========\n");
    {
        float a = 10.0f * d2r;
        const float up_fwd[3]   = { 0,  cosf(a),  sinf(a) };
        const float up_right[3] = { 1,  0,        0       };
        const float up_down[3]  = { 0,  sinf(a), -cosf(a) };

        float b = 20.0f * d2r;
        const float rl_fwd[3]   = { 0,        1,  0        };
        const float rl_right[3] = { cosf(b),  0, -sinf(b)  };
        const float rl_down[3]  = { -sinf(b), 0, -cosf(b)  };

        const float e_fwd[3]   = { 1,  0,  0 };   /* 东 */
        const float e_right[3] = { 0, -1,  0 };   /* 南 */
        const float e_down[3]  = { 0,  0, -1 };

        /* 朝东 + 抬头 8°：航向和俯仰同时非零，防止「只在 yaw=0 时成立」。 */
        float c = 8.0f * d2r;
        const float eu_fwd[3]   = {  cosf(c), 0,  sinf(c) };
        const float eu_right[3] = {  0,      -1,  0       };
        const float eu_down[3]  = {  sinf(c), 0, -cosf(c) };

        struct {
            const char *name;
            const float *fwd, *right, *down;
            float roll, pitch, yaw;
        } poses[] = {
            { "水平朝北",        POSE_LEVEL_N_FWD, POSE_LEVEL_N_RIGHT, POSE_LEVEL_N_DOWN, 0,  0,   0 },
            { "抬头 10°",        up_fwd, up_right, up_down,                               0, 10,   0 },
            { "右滚 20°",        rl_fwd, rl_right, rl_down,                              20,  0,   0 },
            { "水平朝东 yaw 90", e_fwd,  e_right,  e_down,                                0,  0,  90 },
            { "朝东抬头 8°",     eu_fwd, eu_right, eu_down,                               0,  8,  90 },
        };

        for (size_t k = 0; k < sizeof poses / sizeof poses[0]; ++k) {
            printf("  姿态：%s\n", poses[k].name);
            float e1[3], e2[3];
            attitude_error(PK_BOARD_PROFILE_V4, PK_BOARD_PROFILE_V3,
                           poses[k].fwd, poses[k].right, poses[k].down,
                           poses[k].roll, poses[k].pitch, poses[k].yaw, e1);
            attitude_error(PK_BOARD_PROFILE_V3, PK_BOARD_PROFILE_V4,
                           poses[k].fwd, poses[k].right, poses[k].down,
                           poses[k].roll, poses[k].pitch, poses[k].yaw, e2);

            check_num("    V4.3 姿态用 V3.9 解：roll 偏差",  e1[0], -90.0f, 0.05f);
            check_num("    V4.3 姿态用 V3.9 解：pitch 偏差", e1[1],   0.0f, 0.05f);
            check_num("    V4.3 姿态用 V3.9 解：yaw 偏差",   e1[2],   0.0f, 0.05f);
            check_num("    V3.9 姿态用 V4.3 解：roll 偏差",  e2[0], +90.0f, 0.05f);
            check_num("    V3.9 姿态用 V4.3 解：pitch 偏差", e2[1],   0.0f, 0.05f);
            check_num("    V3.9 姿态用 V4.3 解：yaw 偏差",   e2[2],   0.0f, 0.05f);
        }
        printf("\n");
    }

    /* === 2b. 与 profile 数量无关的通用护栏 =========================== *
     * 上面那组断言把「误差是纯 90° roll」这件事钉死了，代价是它只对当前
     * 这一对 profile 成立。这里再补一条对任意两个不同 profile 都成立的：
     * 任意两版之间的 q_delta 必须是一个**显著的非单位旋转**（≥45°），
     * 否则两个 profile 在姿态上就是同一个东西，"分板型"这件事本身就假了。 */
    printf("========== 任意两个 profile 之间必须显著不同 ==========\n");
    for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
            if (i == j) continue;
            float axis[3];
            float ang = profile_delta_rotation(profiles[i], profiles[j], axis);
            printf("  %s → %s: q_delta = 绕 (%+.3f, %+.3f, %+.3f) 转 %.2f°\n",
                   pk_board_profile_name(profiles[i]),
                   pk_board_profile_name(profiles[j]),
                   (double)axis[0], (double)axis[1], (double)axis[2], (double)ang);
            check_true("    两版 q_delta 必须 ≥45°（不能等价）", ang >= 45.0f);
        }
    }
    /* 顺带把「误差为什么只出现在 roll」这条几何事实本身也钉住：转轴必须是
     * 机体 X。哪天它不再是纯 X（比如加了第三版，或装配基准变了），上面那组
     * 精确断言就不再适用，这条会先红，提醒回来重写而不是默默放过。 */
    {
        float axis[3];
        float ang = profile_delta_rotation(PK_BOARD_PROFILE_V3,
                                           PK_BOARD_PROFILE_V4, axis);
        check_num("V3.9↔V4.3 的 q_delta 转角", ang, 90.0f, 0.05f);
        check_num("  转轴 X 分量（必须是纯机体 X）", fabsf(axis[0]), 1.0f, 0.001f);
        check_num("  转轴 Y 分量", axis[1], 0.0f, 0.001f);
        check_num("  转轴 Z 分量", axis[2], 0.0f, 0.001f);
    }
    printf("\n");

    /* === 3. 线加速度向量的 chip→机体旋转（与姿态不同的代码路径）===== */
    printf("========== BNO085 线加速度向量 chip→机体 ==========\n");
    for (int i = 0; i < 2; ++i) {
        pk_board_profile_t p = profiles[i];
        float want[3][3];
        bno_chip_axes_in_body(p, want);
        printf("  %s\n", pk_board_profile_name(p));
        for (int axis = 0; axis < 3; ++axis) {
            float v[3] = { 0, 0, 0 };
            v[axis] = 1.0f;
            float o[3];
            pk_board_imu_chip_vec_to_body(p, v, o);
            char label[64];
            for (int c = 0; c < 3; ++c) {
                snprintf(label, sizeof label, "    芯片+%c → 机体[%d]",
                         "XYZ"[axis], c);
                check_num(label, o[c], want[c][axis], 0.001f);
            }
        }
        printf("\n");
    }

    /* === 4. QMC5883P 独立变换 ======================================= *
     * 手册没有轴向图，所以只断言**封装坐标系**这一段（纯由封装旋转决定），
     * 并要求实现把「封装轴→敏感轴」显式标记为未标定。 */
    printf("========== QMC5883P 封装轴 → 机体 ==========\n");
    for (int i = 0; i < 2; ++i) {
        pk_board_profile_t p = profiles[i];
        float want[3][3];
        mag_pkg_axes_in_body(p, want);
        printf("  %s\n", pk_board_profile_name(p));
        for (int axis = 0; axis < 3; ++axis) {
            float v[3] = { 0, 0, 0 };
            v[axis] = 1.0f;
            float o[3];
            pk_board_mag_pkg_vec_to_body(p, v, o);
            char label[64];
            for (int c = 0; c < 3; ++c) {
                snprintf(label, sizeof label, "    封装+%c → 机体[%d]",
                         "XYZ"[axis], c);
                check_num(label, o[c], want[c][axis], 0.001f);
            }
        }
    }
    /* 两颗器件必须是各自独立的变换，不是同一张表。V4.3 上 U4 转 +90°、
     * U6 转 -90°，所以同一个「封装/芯片 +X」在两颗上映射到不同机体轴。 */
    {
        const float ex[3] = { 1, 0, 0 };
        float imu_o[3], mag_o[3];
        pk_board_imu_chip_vec_to_body(PK_BOARD_PROFILE_V4, ex, imu_o);
        pk_board_mag_pkg_vec_to_body(PK_BOARD_PROFILE_V4, ex, mag_o);
        float diff = fabsf(imu_o[0] - mag_o[0]) + fabsf(imu_o[1] - mag_o[1])
                   + fabsf(imu_o[2] - mag_o[2]);
        printf("  V4.3 上 U4(+90°) 与 U6(-90°) 不能共用一张变换表\n");
        check_true("    IMU 与 MAG 的 +X 映射必须不同", diff > 0.5f);
    }
    check_true("QMC5883P 敏感轴未标定（手册 Rev A 无轴向图）",
               pk_board_mag_axes_calibrated(PK_BOARD_PROFILE_V3) == false &&
               pk_board_mag_axes_calibrated(PK_BOARD_PROFILE_V4) == false);
    printf("\n");

    /* === 5. 板型元数据与 BMP388 ===================================== */
    printf("========== 板型元数据 ==========\n");
    check_num("V3.9 U4 封装旋转",
              pk_board_footprint_rotation_deg(PK_BOARD_PROFILE_V3, PK_BOARD_SENSOR_IMU),
              0.0f, 0.01f);
    check_num("V4.3 U4 封装旋转",
              pk_board_footprint_rotation_deg(PK_BOARD_PROFILE_V4, PK_BOARD_SENSOR_IMU),
              90.0f, 0.01f);
    check_num("V3.9 U6 封装旋转",
              pk_board_footprint_rotation_deg(PK_BOARD_PROFILE_V3, PK_BOARD_SENSOR_MAG),
              0.0f, 0.01f);
    check_num("V4.3 U6 封装旋转",
              pk_board_footprint_rotation_deg(PK_BOARD_PROFILE_V4, PK_BOARD_SENSOR_MAG),
              -90.0f, 0.01f);
    check_num("V3.9 U5 封装旋转",
              pk_board_footprint_rotation_deg(PK_BOARD_PROFILE_V3, PK_BOARD_SENSOR_BARO),
              0.0f, 0.01f);
    check_num("V4.3 U5 封装旋转",
              pk_board_footprint_rotation_deg(PK_BOARD_PROFILE_V4, PK_BOARD_SENSOR_BARO),
              -90.0f, 0.01f);
    check_num("V3.9 U7 封装旋转",
              pk_board_footprint_rotation_deg(PK_BOARD_PROFILE_V3, PK_BOARD_SENSOR_GNSS),
              0.0f, 0.01f);
    check_num("V4.3 U7 封装旋转",
              pk_board_footprint_rotation_deg(PK_BOARD_PROFILE_V4, PK_BOARD_SENSOR_GNSS),
              0.0f, 0.01f);

    /* BMP388 是标量传感器：安装角只作机械元数据，绝不能有姿态旋转接口。
     * 这里用编译期检查表达——pk_board.h 必须不提供 baro 的向量变换。 */
#ifdef PK_BOARD_HAS_BARO_VECTOR_TRANSFORM
    check_true("BMP388 不得提供姿态旋转接口", false);
#else
    check_true("BMP388 不提供姿态旋转接口（标量传感器）", true);
#endif

    /* 板型必须是显式选择的编译期/配置输入，与 powered variant 是两个维度。
     * pk_board_profile() 只能返回两个已冻结板型之一。 */
    {
        pk_board_profile_t sel = pk_board_profile();
        check_true("pk_board_profile() 返回已冻结板型之一",
                   sel == PK_BOARD_PROFILE_V3 || sel == PK_BOARD_PROFILE_V4);
    }
    printf("\n");

    printf("检查项 %d，失败 %d\n", g_checks, g_fail);
    if (g_fail) {
        printf("FAILED: %d 项不符\n", g_fail);
        return 1;
    }
    printf("所有断言通过\n");
    return 0;
}
