/*
 * pk_board.c — 板型 profile 与传感器安装变换的实现。
 *
 * 接口语义、坐标系约定和「为什么不能用 SY6970 猜板型」见 pk_board.h。
 * 本文件只放判据和数值。
 */

#include "pk_board.h"

#include <math.h>
#include <stddef.h>

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

/* ===================================================================== *
 * 判据 1：R_板→机体（两版共用）
 *
 * 几何链：
 *   a) 扩展板与底板的对位。用 J1(2x20 排针) 的**绝对焊盘坐标**与底板卡尺实测
 *      值对拍：只有取 base_x = 151.25 - exp_x（X 镜像）、base_y = exp_y - 51
 *      （Y 不镜像）时，两排引脚轴才落在 Y = 54.06 / 56.60、X 跨距 34.54..82.80，
 *      与 hardware/expansion-board-v3/BASEBOARD_REF-zh_CN.md §2 的实测逐位吻合。
 *      X 镜像意味着扩展板 F.Cu 朝向底板/屏幕那一侧。
 *      （四个 M2.5 安装孔 (54,56)(146,56)(54,106)(146,106) 是 92×50 对称阵列，
 *        本身分辨不出镜像，所以判据取的是 J1 那组非对称坐标。）
 *
 *   b) 盒子安装姿态。2026-09-03 用户机械裁定：
 *        · 盒子竖立，屏幕面向飞行员（机舱内、朝机尾）；
 *        · 电池面（B 面）远离飞行员，朝机头；J1 排针所在的 F 面朝机尾；
 *        · J1 那条长边在盒子下边（靠仪表台）。
 *
 * 代入 NED 机体系（+X 前 / +Y 右 / +Z 下）：
 *      Bx（KiCad 俯视图向右）→ 机体右  ( 0, +1,  0)
 *      By（KiCad 俯视图向上）→ 机体上  ( 0,  0, -1)
 *      Bz（F.Cu 外法线）      → 机体后  (-1,  0,  0)
 *
 * 按列排成矩阵（列 = Bx/By/Bz 的像，行 = 机体 X/Y/Z 分量）。det = +1，是正交
 * 旋转不是镜像——KiCad 的 (x, y_down, z_out) 是左手三元组，所以这里用 By =
 * -Y_kicad 把板系先扳成右手系，再谈旋转。
 * ===================================================================== */
static const float R_BOARD_TO_BODY[3][3] = {
    {  0.0f,  0.0f, -1.0f },
    {  1.0f,  0.0f,  0.0f },
    {  0.0f, -1.0f,  0.0f },
};

/* ===================================================================== *
 * 判据 2：封装在 PCB 上的摆放角。
 *
 * 直接来自两版最终 PCB 的 footprint `(at x y rot)`：
 *   hardware/expansion-board-v3/kicad/expansion-board-v3.kicad_pcb
 *     U4 (63.7, 78.3) rot=0     U5 (71.3, 81.2)  rot=0
 *     U6 (71.3, 77)   rot=0     U7 (70.4, 66.4)  rot=0        全部 F.Cu
 *   hardware/expansion-board-v4/kicad/expansion-board-v4.kicad_pcb
 *     U4 (59.69, 84.455) rot=90     U5 (60.96, 88.9)  rot=-90
 *     U6 (59.69, 96.52)  rot=-90    U7 (73.66, 89.09) rot=0   全部 F.Cu
 *
 * KiCad 正角 = 俯视图逆时针，与 hardware/tools/decoupling_check.py:57-72 的
 * `a = radians(-rot)` 一致（rot=+90 把封装局部 (1,0) 映到板 (0,-1)）。
 * 同一事实另有硬件侧合同 hardware/test_component_contract.py 的
 * test_bno085_orientation_is_explicit_per_board 盯着，改板会双向报警。
 * ===================================================================== */
static const float FOOTPRINT_ROT_DEG[PK_BOARD_PROFILE_COUNT][PK_BOARD_SENSOR_COUNT] = {
    /*                        IMU/U4   BARO/U5   MAG/U6   GNSS/U7 */
    [PK_BOARD_PROFILE_V3] = {   0.0f,     0.0f,    0.0f,    0.0f },
    [PK_BOARD_PROFILE_V4] = {  90.0f,   -90.0f,  -90.0f,    0.0f },
};

/* ===================================================================== *
 * 判据 3：R_封装→板0 —— 封装旋转为 0 时，芯片轴相对板系 B 的指向。
 *
 * BNO085（hardware/datasheets/BNO085.pdf）：
 *   · 第 40 页 Figure 4-1：pin1 标识点在芯片 (-X, +Y) 角，+Z 出芯片顶面。
 *   · 第 52 页 Figure 7-1 TOP VIEW："PIN 1 CORNER" 指左上角；水平向标注
 *     5.20（长边）、垂直向标注 3.80（短边）。
 *   两图联立：芯片 +X 沿 5.20 长边指向图右，+Y 沿 3.80 短边指向图上。
 *
 *   项目自绘封装 expansion-board-v3:BNO085_LGA-28 的 pin1 局部坐标是
 *   (+1.55, -2.25)；封装 X 半宽 1.55（= 3.8mm 短边）、Y 半高 2.325（= 5.2mm
 *   长边）。也就是说封装把手册 TOP VIEW 顺时针转了 90° 画：手册的左短边
 *   （pin 1-6 那条）画成了封装的上边，pin1 落在右端。核对：手册里 pin1 在
 *   (-X_芯片, +Y_芯片)，封装里 pin1 在 (+x_kicad, -y_kicad) = (+Bx, +By)，
 *   两者一致要求 -X_芯片 → +Bx、+Y_芯片 → +By，即：
 *
 *       芯片 +X → -By        （= KiCad 俯视图向下）
 *       芯片 +Y → +Bx        （= KiCad 俯视图向右）
 *       芯片 +Z → +Bz        （器件在 F.Cu，未翻面）
 *
 *   det = +1（右手系到右手系）。
 *
 * QMC5883P（hardware/datasheets/QMC5883P.pdf Rev A）：
 *   封装 Package_LGA:LGA-16_3x3mm_P0.5mm 的 pin 排布（pin1 左上、1-4 沿左边
 *   向下、5-8 沿下边向右、9-12 沿右边向上、13-16 沿上边向左）与手册第 6 页
 *   TOP VIEW 完全一致，所以封装系 = 手册 TOP VIEW，不需要额外旋转。
 *
 *   但手册**没有给敏感轴方向**：第 3 页只有方框图、第 6 页只有 pin 表和封装
 *   尺寸图、第 8 页只有 land pattern，全篇找不到 X/Y/Z 相对 pin1 的箭头。
 *   所以这里只能填单位阵占位，并由 pk_board_mag_axes_calibrated() 返回 false。
 *   替换它需要实板标定（已知外磁场方向下读三轴），不是查资料能补的。
 * ===================================================================== */
static const float R_PKG_TO_BOARD0_IMU[3][3] = {
    /* 列 = 芯片 X/Y/Z 的像，按板系 B 分量 */
    {  0.0f,  1.0f,  0.0f },
    { -1.0f,  0.0f,  0.0f },
    {  0.0f,  0.0f,  1.0f },
};

static const float R_PKG_TO_BOARD0_MAG[3][3] = {
    /* 占位：封装系 == 板系。敏感轴未标定，见上文。 */
    {  1.0f,  0.0f,  0.0f },
    {  0.0f,  1.0f,  0.0f },
    {  0.0f,  0.0f,  1.0f },
};

/* QMC5883P 敏感轴标定状态。上板标定完成前恒为 false —— 不要因为「代码里有
 * 变换了」就把它翻成 true，占位单位阵不是标定结果。 */
#define PK_BOARD_MAG_AXES_CALIBRATED   false

/* ===================================================================== *
 * 板型选择：显式 Kconfig，禁止运行时探测推断
 * ===================================================================== */
#if defined(CONFIG_PK_BOARD_PROFILE_V3)
#  define PK_BOARD_SELECTED_PROFILE  PK_BOARD_PROFILE_V3
#elif defined(CONFIG_PK_BOARD_PROFILE_V4)
#  define PK_BOARD_SELECTED_PROFILE  PK_BOARD_PROFILE_V4
#elif defined(ESP_PLATFORM)
#  error "没有选择扩展板板型：请在 menuconfig 的 Pilot Kit Box 里选 PK_BOARD_PROFILE_V3 或 V4。板型不能在运行时靠探测猜。"
#else
/* host 单元测试没有 sdkconfig。测试从不依赖这个默认值——它对两个 profile
 * 都逐一断言，pk_board_profile() 只被检查「返回两个已冻结板型之一」。 */
#  define PK_BOARD_SELECTED_PROFILE  PK_BOARD_PROFILE_V4
#endif

/* ===================================================================== *
 * 小工具
 * ===================================================================== */

static void mat_mul(const float a[3][3], const float b[3][3], float o[3][3])
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 3; ++k) s += a[r][k] * b[k][c];
            o[r][c] = s;
        }
}

/* 绕板法线 Bz 的旋转，右手系正角 = 俯视逆时针 = KiCad 正角。 */
static void rot_about_board_normal(float deg, float o[3][3])
{
    const float rad = deg * (float)(M_PI / 180.0);
    const float c = cosf(rad), s = sinf(rad);
    o[0][0] =  c;   o[0][1] = -s;   o[0][2] = 0.0f;
    o[1][0] =  s;   o[1][1] =  c;   o[1][2] = 0.0f;
    o[2][0] = 0.0f; o[2][1] = 0.0f; o[2][2] = 1.0f;
}

/* R_芯片→机体 = R_板→机体 · Rz(封装旋转) · R_封装→板0 */
static void sensor_to_body(pk_board_profile_t profile,
                           pk_board_sensor_t sensor,
                           const float pkg_to_board0[3][3],
                           float out[3][3])
{
    float rz[3][3], tmp[3][3];
    rot_about_board_normal(pk_board_footprint_rotation_deg(profile, sensor), rz);
    mat_mul(rz, pkg_to_board0, tmp);
    mat_mul(R_BOARD_TO_BODY, tmp, out);
}

static void apply(const float m[3][3], const float in[3], float out[3])
{
    const float x = in[0], y = in[1], z = in[2];   /* in 与 out 可同一数组 */
    for (int r = 0; r < 3; ++r)
        out[r] = m[r][0] * x + m[r][1] * y + m[r][2] * z;
}

/* 旋转矩阵 → 四元数 (w,x,y,z)，Shepperd 分支法（数值稳定）。 */
static void mat_to_quat(const float m[3][3], float q[4])
{
    const float tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0.0f) {
        const float s = sqrtf(tr + 1.0f) * 2.0f;
        q[0] = 0.25f * s;
        q[1] = (m[2][1] - m[1][2]) / s;
        q[2] = (m[0][2] - m[2][0]) / s;
        q[3] = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
        q[0] = (m[2][1] - m[1][2]) / s;
        q[1] = 0.25f * s;
        q[2] = (m[0][1] + m[1][0]) / s;
        q[3] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        const float s = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
        q[0] = (m[0][2] - m[2][0]) / s;
        q[1] = (m[0][1] + m[1][0]) / s;
        q[2] = 0.25f * s;
        q[3] = (m[1][2] + m[2][1]) / s;
    } else {
        const float s = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
        q[0] = (m[1][0] - m[0][1]) / s;
        q[1] = (m[0][2] + m[2][0]) / s;
        q[2] = (m[1][2] + m[2][1]) / s;
        q[3] = 0.25f * s;
    }
}

/* ===================================================================== *
 * 公开接口
 * ===================================================================== */

pk_board_profile_t pk_board_profile(void)
{
    return PK_BOARD_SELECTED_PROFILE;
}

const char *pk_board_profile_name(pk_board_profile_t profile)
{
    switch (profile) {
    case PK_BOARD_PROFILE_V3: return "v3";
    case PK_BOARD_PROFILE_V4: return "v4";
    default:                    return "?";
    }
}

float pk_board_footprint_rotation_deg(pk_board_profile_t profile,
                                      pk_board_sensor_t sensor)
{
    if (profile < 0 || profile >= PK_BOARD_PROFILE_COUNT) return 0.0f;
    if (sensor  < 0 || sensor  >= PK_BOARD_SENSOR_COUNT)  return 0.0f;
    return FOOTPRINT_ROT_DEG[profile][sensor];
}

void pk_board_imu_body_fix_quat(pk_board_profile_t profile, float q[4])
{
    float chip_to_body[3][3];
    sensor_to_body(profile, PK_BOARD_SENSOR_IMU, R_PKG_TO_BOARD0_IMU,
                   chip_to_body);

    /* sandwich 右乘要的是 q_body_fix = R_机体→芯片，即上式的逆；
     * 单位四元数的逆就是共轭。 */
    float q_chip_to_body[4];
    mat_to_quat(chip_to_body, q_chip_to_body);
    q[0] =  q_chip_to_body[0];
    q[1] = -q_chip_to_body[1];
    q[2] = -q_chip_to_body[2];
    q[3] = -q_chip_to_body[3];
}

void pk_board_imu_chip_vec_to_body(pk_board_profile_t profile,
                                   const float in[3], float out[3])
{
    float chip_to_body[3][3];
    sensor_to_body(profile, PK_BOARD_SENSOR_IMU, R_PKG_TO_BOARD0_IMU,
                   chip_to_body);
    apply(chip_to_body, in, out);
}

void pk_board_mag_pkg_vec_to_body(pk_board_profile_t profile,
                                  const float in[3], float out[3])
{
    float pkg_to_body[3][3];
    sensor_to_body(profile, PK_BOARD_SENSOR_MAG, R_PKG_TO_BOARD0_MAG,
                   pkg_to_body);
    apply(pkg_to_body, in, out);
}

bool pk_board_mag_axes_calibrated(pk_board_profile_t profile)
{
    (void)profile;   /* 两版都未标定；标定是器件级事实，不随板型变 */
    return PK_BOARD_MAG_AXES_CALIBRATED;
}
