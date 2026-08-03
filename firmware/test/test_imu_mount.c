/*
 * test_imu_mount.c — host 端验证 PK_IMU_MOUNT_QUAT_*（IMU 安装方位）。
 *
 * 为什么需要它
 * ------------
 * 安装四元数是一个**没有单元可查、错了也照样出数**的常量：装反 180°，
 * PFD 照样有姿态、照样跟着动，只是全反了。真机上靠肉眼「歪着摆一摆看
 * 数对不对」既慢又不可复现，改一次装配就得重来一遍。
 *
 * 这里的做法是：**从物理装配独立地**构造 BNO085 应当输出的四元数，
 * 再喂进固件真实的 sandwich 变换，断言 PFD 该显示的欧拉角。
 *
 *     q_aircraft = q_world_fix · q_bno · q_body_fix        （imu_task.c）
 *
 * 关键点是 q_bno 的构造**不使用被测的宏**，只用两件物理事实：
 *
 *   1. R_chip→aircraft：芯片三轴在机体系里分别指向哪 —— 由用户描述的
 *      实际装配写死在 chip_axes_in_aircraft() 里；
 *   2. R_aircraft→ENU：测试姿态（水平朝北 / 抬头 10° / 右滚 20° /
 *      朝东）下机体三轴在 ENU 世界系里指向哪。
 *
 * 两者相乘得到 R_chip→ENU，正是 BNO085 Rotation Vector 的定义
 * （Android 约定：world = Earth ENU，device = chip 体轴）。所以这个
 * 测试是独立的：宏写错了它就红。
 *
 * 与 test_imu_tare.c 一致，纯数学函数从 firmware/main/imu_task.c 逐字
 * 复制（那边是 static，无法链接）；但**安装宏是 include 真头文件拿的**，
 * 所以改 imu_task.h 会立刻反映到这里，不存在两份常量走偏。
 *
 * 编译运行（不需要 ESP-IDF）：
 *     cc -std=c11 -O2 -I firmware/main -I sim/compat \
 *        -o /tmp/test_imu_mount firmware/test/test_imu_mount.c -lm
 *     /tmp/test_imu_mount
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* 真实的安装常量来自固件头文件 —— 不复制，改宏这里立刻跟着变。
 * imu_task.h 会 include esp_err.h / driver/i2c_master.h，用 sim/compat
 * 下模拟器已有的桩满足（-I sim/compat）。 */
#include "imu_task.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- 以下三个函数是 firmware/main/imu_task.c 的逐字副本 ------------- */

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

/* imu_task.c 的 quat_rotate_vec_body_fix()：用 conj(q_body_fix) 把
 * chip 体轴向量转到机体轴（线加速度走这条路，与姿态的 sandwich 不同）。 */
static inline void quat_rotate_vec_body_fix(float vx, float vy, float vz,
                                            float *ox, float *oy, float *oz)
{
    const float pw =  PK_IMU_MOUNT_QUAT_W;
    const float px = -PK_IMU_MOUNT_QUAT_X;
    const float py = -PK_IMU_MOUNT_QUAT_Y;
    const float pz = -PK_IMU_MOUNT_QUAT_Z;

    float tw, tx, ty, tz;
    quat_mul(pw, px, py, pz, 0.0f, vx, vy, vz, &tw, &tx, &ty, &tz);

    float rw, rx, ry, rz;
    quat_mul(tw, tx, ty, tz, pw, -px, -py, -pz, &rw, &rx, &ry, &rz);
    (void)rw;
    *ox = rx;
    *oy = ry;
    *oz = rz;
}

/* imu_task.c parse_rotation_vector() 里那段 sandwich，抽出来单独跑。 */
static void sandwich_to_euler(const float q_bno[4],
                              float *roll, float *pitch, float *yaw)
{
    float tw, ti, tj, tk;
    quat_mul(PK_IMU_WORLD_FIX_W, PK_IMU_WORLD_FIX_X,
             PK_IMU_WORLD_FIX_Y, PK_IMU_WORLD_FIX_Z,
             q_bno[0], q_bno[1], q_bno[2], q_bno[3],
             &tw, &ti, &tj, &tk);
    float aw, ai, aj, ak;
    quat_mul(tw, ti, tj, tk,
             PK_IMU_MOUNT_QUAT_W, PK_IMU_MOUNT_QUAT_X,
             PK_IMU_MOUNT_QUAT_Y, PK_IMU_MOUNT_QUAT_Z,
             &aw, &ai, &aj, &ak);
    quat_to_euler(ai, aj, ak, aw, roll, pitch, yaw);
}

/* ---- 物理装配 → BNO085 应输出的四元数（不使用被测的宏）------------- */

/*
 * 当前装配（用户 2026-08-03 重新焊装，相对上一版绕板法线转了 180°）：
 *
 *   旧板：VCC 在左上角，PS0 在左下角，芯片面朝屏幕
 *   新板：VCC 在右下角，PS0 在右上角，芯片面朝屏幕
 *
 * 芯片面朝向不变（仍朝屏幕 = 机体后方），只是绕板法线转了 180°，
 * 于是芯片的 X/Y 两轴同时取反、Z 轴不变：
 *
 *   旧： Xc→机体上(-Z)   Yc→机体左(-Y)   Zc→机体后(-X)
 *   新： Xc→机体下(+Z)   Yc→机体右(+Y)   Zc→机体后(-X)
 *
 * 下面按 NED 机体系（+X 前 / +Y 右 / +Z 下）写出芯片三轴，作为列向量
 * 组成 R_chip→aircraft。这是**物理事实**，是本测试的输入基准。
 */
static void chip_axes_in_aircraft(float m[3][3])
{
    const float Xc[3] = {  0.0f, 0.0f, 1.0f };   /* 芯片 +X → 机体下 */
    const float Yc[3] = {  0.0f, 1.0f, 0.0f };   /* 芯片 +Y → 机体右 */
    const float Zc[3] = { -1.0f, 0.0f, 0.0f };   /* 芯片 +Z → 机体后 */
    for (int r = 0; r < 3; ++r) {
        m[r][0] = Xc[r];
        m[r][1] = Yc[r];
        m[r][2] = Zc[r];
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

/* 旋转矩阵 → 四元数（Shepperd 的分支法，数值稳定）。输出 (w,x,y,z)。 */
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
 * Rotation Vector 的定义就是 R_chip→ENU（Android 约定），于是
 *
 *     R_chip→ENU = R_aircraft→ENU · R_chip→aircraft
 */
static void bno_quat_for_pose(const float fwd[3], const float right[3],
                              const float down[3], float q_bno[4])
{
    float r_ac[3][3], r_ae[3][3], r_ce[3][3];
    chip_axes_in_aircraft(r_ac);
    aircraft_axes_in_enu(fwd, right, down, r_ae);
    mat_mul(r_ae, r_ac, r_ce);
    mat_to_quat(r_ce, q_bno);
}

/* ---- 断言 ----------------------------------------------------------- */

static int g_fail;

static void check(const char *what, float got, float want, float tol)
{
    /* 角度环绕：359.9 与 0.0 只差 0.1。 */
    float d = got - want;
    while (d >  180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    bool ok = fabsf(d) <= tol;
    printf("  %-34s got %+8.2f  want %+8.2f   %s\n",
           what, (double)got, (double)want, ok ? "OK" : "*** FAIL ***");
    if (!ok) g_fail++;
}

static void case_attitude(const char *name,
                          const float fwd[3], const float right[3],
                          const float down[3],
                          float want_roll, float want_pitch, float want_yaw)
{
    float q[4];
    bno_quat_for_pose(fwd, right, down, q);
    float roll, pitch, yaw;
    sandwich_to_euler(q, &roll, &pitch, &yaw);
    printf("%s\n  q_bno(w,i,j,k) = %+.4f %+.4f %+.4f %+.4f\n",
           name, (double)q[0], (double)q[1], (double)q[2], (double)q[3]);
    check("roll",  roll,  want_roll,  0.05f);
    check("pitch", pitch, want_pitch, 0.05f);
    check("yaw",   yaw,   want_yaw,   0.05f);
    printf("\n");
}

int main(void)
{
    printf("PK_IMU_MOUNT_QUAT = (%.7f, %.7f, %.7f, %.7f)\n\n",
           (double)PK_IMU_MOUNT_QUAT_W, (double)PK_IMU_MOUNT_QUAT_X,
           (double)PK_IMU_MOUNT_QUAT_Y, (double)PK_IMU_MOUNT_QUAT_Z);

    const float d2r = (float)M_PI / 180.0f;

    /* 1. 水平、机头朝北 —— PFD 应该三个角全 0。 */
    {
        const float fwd[3]   = { 0, 1,  0 };   /* 北 */
        const float right[3] = { 1, 0,  0 };   /* 东 */
        const float down[3]  = { 0, 0, -1 };
        case_attitude("[1] 水平朝北 → 0 / 0 / 0", fwd, right, down, 0, 0, 0);
    }

    /* 2. 机头上抬 10°（绕机体右轴），仍朝北 —— pitch 应为 +10。 */
    {
        float a = 10.0f * d2r;
        const float fwd[3]   = { 0,  cosf(a),  sinf(a) };
        const float right[3] = { 1,  0,        0       };
        const float down[3]  = { 0,  sinf(a), -cosf(a) };
        case_attitude("[2] 抬头 10° → pitch +10", fwd, right, down, 0, 10, 0);
    }

    /* 3. 右翼下沉 20°（绕机体前轴），水平朝北 —— roll 应为 +20。 */
    {
        float a = 20.0f * d2r;
        const float fwd[3]   = { 0,       1,  0        };
        const float right[3] = { cosf(a), 0, -sinf(a)  };
        const float down[3]  = { -sinf(a), 0, -cosf(a) };
        case_attitude("[3] 右滚 20° → roll +20", fwd, right, down, 20, 0, 0);
    }

    /* 4. 水平朝东 —— yaw 应为 90。 */
    {
        const float fwd[3]   = { 1,  0,  0 };   /* 东 */
        const float right[3] = { 0, -1,  0 };   /* 南 */
        const float down[3]  = { 0,  0, -1 };
        case_attitude("[4] 水平朝东 → yaw 90", fwd, right, down, 0, 0, 90);
    }

    /* 5. 线加速度向量的 chip→机体旋转，与姿态走的是不同代码路径
     *    （quat_rotate_vec_body_fix 用 conj(q_body_fix)），单独验。
     *    期望值就是 chip_axes_in_aircraft() 里那三列。 */
    {
        printf("[5] 线加速度向量 chip→机体\n");
        struct { const char *name; float v[3]; float want[3]; } cases[] = {
            { "chip +X → 机体下 (0,0,+1)", { 1, 0, 0 }, {  0, 0, 1 } },
            { "chip +Y → 机体右 (0,+1,0)", { 0, 1, 0 }, {  0, 1, 0 } },
            { "chip +Z → 机体后 (-1,0,0)", { 0, 0, 1 }, { -1, 0, 0 } },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            float ox, oy, oz;
            quat_rotate_vec_body_fix(cases[i].v[0], cases[i].v[1],
                                     cases[i].v[2], &ox, &oy, &oz);
            printf("  %s\n", cases[i].name);
            check("    x", ox, cases[i].want[0], 0.001f);
            check("    y", oy, cases[i].want[1], 0.001f);
            check("    z", oz, cases[i].want[2], 0.001f);
        }
        printf("\n");
    }

    if (g_fail) {
        printf("FAILED: %d 项不符\n", g_fail);
        return 1;
    }
    printf("所有断言通过\n");
    return 0;
}
