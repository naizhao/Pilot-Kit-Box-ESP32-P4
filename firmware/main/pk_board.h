/*
 * pk_board.h — 显式扩展板板型 profile 与传感器安装变换。
 *
 * 这个模块回答一件事：**这颗传感器的芯片轴，在机体 NED 坐标系里指向哪。**
 *
 * profile 的粒度是**板系**，不是修订号
 * ------------------------------------
 * `v3` / `v4`（将来还有 `v5`）指的是扩展板**板系**。`V3.9` / `V4.3` 只是这两个
 * 板系当前的修订号——修订会滚动，板系不会。所以 profile 用 v3/v4，别把修订号
 * 编进标识符里，否则每出一版 V4.4 就要改一遍固件、Kconfig、发布脚本和刷机页。
 *
 * 下面这张角度表实测自 V3.9 与 V4.3（即两个板系当前的修订）。把传感器摆位当作
 * **板系级属性**是一个假设，不是白拿的：`hardware/test_firmware_board_profile_
 * contract.py` 会把本文件的角度表与两个 kicad_pcb 现场逐项对拍，哪天某个新修订
 * 转了传感器，它先变红，而不是悄悄发出去。
 *
 * 为什么必须显式选板系
 * --------------------
 * 两个板系的传感器物理方向不同（2026-09-03 产品裁定，v3 保持 0° 是最终设计，
 * 不是回灌遗漏）：
 *
 *     器件            v3      v4      固件含义
 *     U4 BNO085         0°     +90°   两个板系必须用不同的 board-to-body 变换
 *     U5 BMP388         0°     -90°   气压是标量，安装角不参与数值变换
 *     U6 QMC5883P       0°     -90°   必须有独立于 BNO085 的变换
 *     U7 ATGM336H       0°       0°   无姿态轴变换
 *
 * U4 与 U5/U6 **转向相反**，所以任何「整板统一旋转 90°」的建模都是错的。
 *
 * 板型只能来自显式配置（Kconfig `PK_BOARD_PROFILE_*`）。**禁止**用
 * SY6970 是否 ACK 来推断板型：V3 与 unpowered V4 都可能探测不到 SY6970，
 * 那个探测结果只能表达 powered variant，是与板型正交的另一个维度。
 *
 * 怎么切板系
 * ----------
 * **默认是 v4。** 手上是 v3 板就必须改，两个板系差 90°，选错了 PFD 照样出
 * 姿态、只是横滚整体偏 90°，桌上看不出来。
 *
 *   交互：  cd firmware && ./build.sh menuconfig
 *           → Pilot Kit Box → Expansion board profile → 选 v3 → S 保存 Q 退出
 *           && ./build.sh build
 *
 *   脚本：  cd firmware
 *           sed -i '' 's/^# CONFIG_PK_BOARD_PROFILE_V3 is not set$/CONFIG_PK_BOARD_PROFILE_V3=y/' sdkconfig
 *           sed -i '' 's/^CONFIG_PK_BOARD_PROFILE_V4=y$/# CONFIG_PK_BOARD_PROFILE_V4 is not set/' sdkconfig
 *           ./build.sh build
 *           （Linux 用 `sed -i`，不带那对空引号。切回 v4 把两条反过来。）
 *
 * 确认编的是哪一版：
 *   烧录前  grep PK_BOARD_PROFILE firmware/build/config/sdkconfig.h   （只会有一行）
 *   烧录后  开机日志 `imu: board profile v3|v4: q_body_fix = …`
 *           v3 = 0.7071068 0 0.7071068 0；v4 = 0.5 0.5 0.5 -0.5，两组数不一样。
 *
 * 完整说明见 docs/configuration.md 第 1 节。
 *
 * 将来加 v5 要动的地方（就这几处，都有测试盯着）：
 *   1. 本文件的 pk_board_profile_t 加 PK_BOARD_PROFILE_V5
 *   2. pk_board.c 的 FOOTPRINT_ROT_DEG 加一行（值从新板 kicad_pcb 实测）
 *      + pk_board_profile_name() 加一个 case
 *   3. Kconfig.projbuild 的 choice 加一项
 *   4. firmware/sdkconfig.defaults.v5
 *   5. tools/firmware_release/prepare_esp32p4_release.py 的 BOARD_PROFILES
 *   6. .github/workflows/release-esp32p4-firmware.yml 的 BOARD_PROFILES 默认值
 *      与 board_profile 下拉项
 *   7. web/flasher/index.html 加一个按钮
 *   8. firmware/test/test_pk_board_mount.c 按物理轴独立推导新板的期望值
 *   如果新板系的 R_板→机体（盒子里的朝向）也变了，那是第 9 处，在 pk_board.c。
 *
 * 三段式分解
 * ----------
 *     R_芯片→机体 = R_板→机体 · Rz(封装旋转) · R_封装→板0
 *
 *   ─ R_板→机体：整块 PCB 在盒子/飞机里的朝向。两个板系共用（同板框、同安装
 *     孔、同堆叠方式）。判据见 R_BOARD_TO_BODY 处的注释。
 *   ─ Rz(封装旋转)：绕板法线的封装摆放角，来自各板系 kicad_pcb 实测。**这一段
 *     是本次冻结的硬件事实**，也是两个板系唯一的差别。
 *   ─ R_封装→板0：封装旋转为 0 时，芯片敏感轴相对封装/板坐标系的指向，来自
 *     原厂手册。每颗器件各一份。
 *
 * 坐标系约定
 * ----------
 *   板坐标系 B（右手系，跟着 PCB 走）：
 *     Bx = KiCad 俯视图向右      By = KiCad 俯视图向上（= -Y_kicad）
 *     Bz = F.Cu 外法线（指向元件面外）
 *   机体坐标系 = 航空 NED：+X 前（机头）/ +Y 右 / +Z 下。
 *
 * 回归测试：firmware/test/test_pk_board_mount.c —— 它从手册轴向、封装旋转和
 * 装配朝向**独立**推导期望值，不复用本文件的任何矩阵；选错 profile 会红。
 */
#pragma once

#include <stdbool.h>

/* 当前支持的板系。v1/v2 不再兼容，已从固件中退出。
 * 同一板系的不同修订共用一个 profile：例如 V4.0 是 V4.3 的返修前身，固件层面
 * 都按 v4 处理（GNSS Gate 飞线返修不改变传感器安装方向）。 */
typedef enum {
    PK_BOARD_PROFILE_V3 = 0,
    PK_BOARD_PROFILE_V4 = 1,
    PK_BOARD_PROFILE_COUNT
} pk_board_profile_t;

/* 板上四颗与安装方向有关的器件。 */
typedef enum {
    PK_BOARD_SENSOR_IMU  = 0,   /* U4 BNO085   */
    PK_BOARD_SENSOR_BARO = 1,   /* U5 BMP388   */
    PK_BOARD_SENSOR_MAG  = 2,   /* U6 QMC5883P */
    PK_BOARD_SENSOR_GNSS = 3,   /* U7 ATGM336H */
    PK_BOARD_SENSOR_COUNT
} pk_board_sensor_t;

/*
 * 当前构建目标板型。来源是显式的 Kconfig 选择，**不是**运行时探测。
 * powered / unpowered 是另一个维度，由 PMIC 探测表达，不在这里。
 */
pk_board_profile_t pk_board_profile(void);

/* "v3" / "v4"，用于日志和诊断页。返回的是**板系**，不是修订号——固件区分不了
 * 同一板系的修订（板上没有 revision strap 或 EEPROM）。 */
const char *pk_board_profile_name(pk_board_profile_t profile);

/*
 * 封装在 PCB 上的摆放角（度，KiCad 约定：正角 = 俯视逆时针）。
 * 这是机械元数据，供诊断页和装配核对用。
 * 注意 BMP388 也有摆放角，但它是标量传感器——这个角**不参与任何数值变换**。
 */
float pk_board_footprint_rotation_deg(pk_board_profile_t profile,
                                      pk_board_sensor_t sensor);

/* ---- BNO085（U4）--------------------------------------------------- */

/*
 * q_body_fix = R_机体→芯片，供 imu_task.c 的 sandwich 右乘：
 *     q_aircraft = q_world_fix · q_bno · q_body_fix
 * 输出顺序 (w, x, y, z)。
 */
void pk_board_imu_body_fix_quat(pk_board_profile_t profile, float q[4]);

/*
 * 把 BNO085 芯片体轴向量（线加速度等）转到机体 NED。
 * 与姿态走的是不同代码路径：向量只做体轴重映射，不做 ENU→NED 的世界系修正。
 * in 与 out 可以是同一个数组。
 */
void pk_board_imu_chip_vec_to_body(pk_board_profile_t profile,
                                   const float in[3], float out[3]);

/* ---- QMC5883P（U6）------------------------------------------------- */

/*
 * 把 QMC5883P **封装坐标系**向量转到机体 NED。
 * 封装系定义：Px = 俯视图右、Py = 俯视图上、Pz = F.Cu 外法线，随封装一起转。
 *
 * ⚠️ 这里到「芯片敏感轴」还差一段：hardware/datasheets/QMC5883P.pdf Rev A
 * 通篇没有轴向图（第 3 页方框图、第 6 页 pin 表 + 封装图、第 8 页 land
 * pattern，都没标 X/Y/Z 相对 pin1 的方向）。所以 R_敏感轴→封装 目前是单位阵
 * **占位**，必须上板标定后才能替换。在那之前 pk_board_mag_axes_calibrated()
 * 返回 false，磁力计不得作为 PFD 航向源。
 */
void pk_board_mag_pkg_vec_to_body(pk_board_profile_t profile,
                                  const float in[3], float out[3]);

/*
 * 磁力计敏感轴是否已标定。当前恒为 false——手册没给轴向图，占位是单位阵。
 * 消费方必须检查这个标志：false 时只能把 QMC5883P 当诊断/冗余数据，
 * 不能进入航向融合，也不能替换 BNO085 的 heading。
 */
bool pk_board_mag_axes_calibrated(pk_board_profile_t profile);

/* ---- BMP388（U5）/ ATGM336H（U7）----------------------------------- *
 *
 * 故意不提供向量变换接口：
 *   ─ BMP388 输出的是气压标量，安装角对数值没有任何影响；给它加"姿态旋转"
 *     是无意义的复杂度，也会诱导后来人以为气压需要按板型换算。
 *   ─ ATGM336H 两版均为 0°，且它输出的是大地坐标（经纬度/航迹角），
 *     不存在机体轴变换。
 * 两者的摆放角仍可通过 pk_board_footprint_rotation_deg() 查到，作为装配元数据。
 */
