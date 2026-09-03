#!/usr/bin/env python3
"""固件板型 profile 的源级合同。

为什么放在 hardware/ 而不是 firmware/test/
------------------------------------------
这里断言的不是数值，而是**硬件事实与固件常量之间的一致性**，以及一条只能靠
读源码才能守住的禁令。它的判据一半来自 kicad_pcb，一半来自 pk_board.c，所以
和其它跨 KiCad/固件的合同（test_component_contract.py）放在一起。

三件事：

1. pk_board.c 里的封装旋转表必须与两版最终 PCB 的实测值逐项一致。
   —— 这张表是 IMU/磁力计变换的唯一输入，抄错一个符号，PFD 就整体歪 90°，
   而屏上看起来「有姿态、会动」，人眼发现不了。

2. 板型**禁止**由 SY6970 是否 ACK 推断。V3 与 unpowered V4 都探测不到
   SY6970，用它猜板型会把 unpowered V4.3 当成 V3.9，姿态和 VBUS 换算一起错。
   powered/unpowered 与 V3/V4 是两个正交维度，不能压成一个。

3. BMP388 是标量传感器，不得出现姿态旋转接口。

数值层面的正确性由 firmware/test/test_pk_board_mount.c 负责（它从手册轴向和
装配朝向独立推导期望值）；本文件只守「源和板对不对得上」这一层。
"""

from __future__ import annotations

import math
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

PK_BOARD_C = ROOT / "firmware" / "main" / "pk_board.c"
PK_BOARD_H = ROOT / "firmware" / "main" / "pk_board.h"

# 板型 → (kicad_pcb 路径, pk_board.c 里的 profile 枚举名)
BOARDS = {
    "expansion-board-v3": "PK_BOARD_PROFILE_V3",
    "expansion-board-v4": "PK_BOARD_PROFILE_V4",
}

# 位号 → pk_board.c 表里的列序（与 pk_board.h 的 pk_board_sensor_t 同序）
SENSOR_COLUMN = {"U4": 0, "U5": 1, "U6": 2, "U7": 3}


def _footprint_blocks(text: str):
    """切出 kicad_pcb 里每个 (footprint ...) 的完整括号块。"""
    for m in re.finditer(r"\(footprint ", text):
        i = m.start()
        depth = 0
        j = i
        while True:
            c = text[j]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        yield text[i : j + 1]


def pcb_rotation(board: str, ref: str) -> float:
    """从最终 PCB 读某个位号的封装摆放角（度，KiCad 约定）。"""
    pcb = ROOT / "hardware" / board / "kicad" / f"{board}.kicad_pcb"
    text = pcb.read_text(encoding="utf-8")
    for blk in _footprint_blocks(text):
        prop = re.search(r'\(property "Reference" "([^"]+)"', blk)
        if not prop or prop.group(1) != ref:
            continue
        at = re.search(r"\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)", blk)
        return float(at.group(3) or 0.0)
    raise AssertionError(f"{board} 里找不到位号 {ref}")


def pcb_layer(board: str, ref: str) -> str:
    pcb = ROOT / "hardware" / board / "kicad" / f"{board}.kicad_pcb"
    text = pcb.read_text(encoding="utf-8")
    for blk in _footprint_blocks(text):
        prop = re.search(r'\(property "Reference" "([^"]+)"', blk)
        if not prop or prop.group(1) != ref:
            continue
        return re.search(r'\(layer "([^"]+)"\)', blk).group(1)
    raise AssertionError(f"{board} 里找不到位号 {ref}")


def firmware_rotation_table() -> dict[str, list[float]]:
    """解析 pk_board.c 的 FOOTPRINT_ROT_DEG 指定初始化表。"""
    src = PK_BOARD_C.read_text(encoding="utf-8")
    table: dict[str, list[float]] = {}
    for m in re.finditer(
        r"\[(PK_BOARD_PROFILE_\w+)\]\s*=\s*\{([^}]*)\}", src
    ):
        vals = [float(v) for v in re.findall(r"(-?\d+(?:\.\d+)?)f", m.group(2))]
        table[m.group(1)] = vals
    return table


class FirmwareBoardProfileContractTest(unittest.TestCase):
    def test_footprint_rotation_table_matches_both_pcbs(self):
        table = firmware_rotation_table()
        self.assertEqual(
            set(table), set(BOARDS.values()),
            "pk_board.c 的封装旋转表必须且只能覆盖两个已冻结板型",
        )
        for board, profile in BOARDS.items():
            row = table[profile]
            self.assertEqual(len(row), len(SENSOR_COLUMN), f"{profile} 行长不对")
            for ref, col in SENSOR_COLUMN.items():
                with self.subTest(board=board, ref=ref):
                    self.assertAlmostEqual(
                        row[col], pcb_rotation(board, ref), places=3,
                        msg=f"{board} {ref}: 固件表与 PCB 实测摆放角不一致",
                    )

    def test_all_orientation_sensors_are_on_front_copper(self):
        # pk_board.c 的 R_封装→板0 里「+Z_芯片 → +Bz」这一步的前提是器件没翻面。
        # 任何一颗跑到 B.Cu，Z 轴就要取反，那张表必须重算。
        for board in BOARDS:
            for ref in SENSOR_COLUMN:
                with self.subTest(board=board, ref=ref):
                    self.assertEqual(pcb_layer(board, ref), "F.Cu")

    def test_u4_and_u6_rotate_in_opposite_directions_on_v4(self):
        # 这是「不能抽象成整板统一旋转」的硬判据。它红了说明要么板改了，
        # 要么有人把两颗合成了一张表。
        u4 = pcb_rotation("expansion-board-v4", "U4")
        u6 = pcb_rotation("expansion-board-v4", "U6")
        self.assertAlmostEqual(u4, 90.0, places=3)
        self.assertAlmostEqual(u6, -90.0, places=3)
        self.assertLess(u4 * u6, 0.0, "V4.3 的 U4 与 U6 必须转向相反")

    def test_board_profile_is_not_inferred_from_pmic(self):
        src = PK_BOARD_C.read_text(encoding="utf-8")
        hdr = PK_BOARD_H.read_text(encoding="utf-8")
        # 只查代码，不查注释——注释里必须能写「禁止用 SY6970 猜板型」。
        code = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
        code = re.sub(r"//[^\n]*", "", code)
        code += re.sub(r"/\*.*?\*/", "", hdr, flags=re.S)
        for banned in ("SY6970", "sy6970", "0x6A", "0x6a", "pmic", "PMIC"):
            self.assertNotIn(
                banned, code,
                f"pk_board 的代码里出现了 {banned}：板型必须来自显式配置，"
                "SY6970 只能表达 powered variant（V3 与 unpowered V4 都不 ACK 它）",
            )
        # 选择只能来自 Kconfig，且没选时必须编译期报错，不能静默默认。
        self.assertIn("CONFIG_PK_BOARD_PROFILE_V3", src)
        self.assertIn("CONFIG_PK_BOARD_PROFILE_V4", src)
        self.assertRegex(src, r"#\s*error")

    def test_kconfig_offers_exactly_the_two_frozen_profiles(self):
        kconfig = (ROOT / "firmware" / "main" / "Kconfig.projbuild").read_text(
            encoding="utf-8"
        )
        self.assertIn("choice PK_BOARD_PROFILE", kconfig)
        offered = set(re.findall(r"config (PK_BOARD_PROFILE_\w+)", kconfig))
        self.assertEqual(offered, {"PK_BOARD_PROFILE_V3", "PK_BOARD_PROFILE_V4"})

    def test_kconfig_default_is_pinned(self):
        # 产品裁定：默认 V4.3。默认值本身不是问题，**默认被悄悄改掉**才是。
        # 把它钉在合同里，改默认就必须同时改这里和文档，改不动就改不成。
        kconfig = (ROOT / "firmware" / "main" / "Kconfig.projbuild").read_text(
            encoding="utf-8"
        )
        defaults = re.findall(r"(?m)^\s*default (PK_BOARD_PROFILE_\w+)\s*$", kconfig)
        self.assertEqual(
            defaults, ["PK_BOARD_PROFILE_V4"],
            "Kconfig 的板型默认值必须恰好是 V4.3（2026-09-03 产品裁定）",
        )

    def test_both_boards_have_a_first_class_build_config(self):
        # 只有一个 default 意味着 V3.9 是"要自己记得切"的二等公民，忘了切就静默
        # 出 V4.3 固件。两版各给一份 sdkconfig.defaults 片段，让 V3.9 也有一条
        # 可复现、可进 CI 的构建路径。
        fw = ROOT / "firmware"
        for stem, on, off in (
            ("sdkconfig.defaults.v3", "V3", "V4"),
            ("sdkconfig.defaults.v4", "V4", "V3"),
        ):
            path = fw / stem
            with self.subTest(file=stem):
                self.assertTrue(path.is_file(), f"缺少 {stem}")
                text = path.read_text(encoding="utf-8")
                self.assertIn(f"CONFIG_PK_BOARD_PROFILE_{on}=y", text)
                self.assertNotIn(f"CONFIG_PK_BOARD_PROFILE_{off}=y", text)
        # 基础 defaults 不许写死板型，否则两个片段谁也覆盖不掉它。
        base = (fw / "sdkconfig.defaults").read_text(encoding="utf-8")
        self.assertNotIn("CONFIG_PK_BOARD_PROFILE", base)

    def test_build_announces_the_selected_profile(self):
        # 构建时必须把选中的板型打出来。人在场的两个时刻——configure 和开机——
        # 都要能看见，光靠"记得自己选过"不算。
        cmake = (ROOT / "firmware" / "main" / "CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        self.assertIn("CONFIG_PK_BOARD_PROFILE_V3", cmake)
        self.assertIn("CONFIG_PK_BOARD_PROFILE_V4", cmake)
        self.assertRegex(cmake, r"message\(STATUS[^)]*[Bb]oard profile")
        imu_c = (ROOT / "firmware" / "main" / "imu_task.c").read_text(encoding="utf-8")
        self.assertIn("board profile %s", imu_c)

    def test_no_second_global_mounting_correction_path(self):
        # imu_task 里那四个手工修正旋钮是旧 breakout 时代的遗留：它们在 profile
        # 变换**之后**再改欧拉角，构成第二套全局安装修正入口。而且欧拉角后处理
        # 不等价于三维安装旋转（invert pitch 不是任何一个真实姿态变换），留着
        # 只会让"安装方向到底由谁决定"变成两个答案。必须删干净。
        banned = (
            "PK_IMU_MOUNT_INVERT_ROLL",
            "PK_IMU_MOUNT_INVERT_PITCH",
            "PK_IMU_MOUNT_INVERT_YAW",
            "PK_IMU_MOUNT_YAW_OFFSET_DEG",
        )
        for name in ("imu_task.h", "imu_task.c"):
            text = (ROOT / "firmware" / "main" / name).read_text(encoding="utf-8")
            for sym in banned:
                with self.subTest(file=name, symbol=sym):
                    self.assertNotIn(sym, text)

    def test_baro_has_no_attitude_transform(self):
        hdr = PK_BOARD_H.read_text(encoding="utf-8")
        src = PK_BOARD_C.read_text(encoding="utf-8")
        for text in (hdr, src):
            self.assertNotRegex(
                text, r"pk_board_baro_\w*(vec|quat|rot\w*_to_body)",
                "BMP388 是标量传感器，不得提供姿态旋转接口",
            )

    def test_magnetometer_transform_is_independent_and_flagged_uncalibrated(self):
        src = PK_BOARD_C.read_text(encoding="utf-8")
        # 两颗必须各有一张 R_封装→板0，不能共用一个符号。
        self.assertIn("R_PKG_TO_BOARD0_IMU", src)
        self.assertIn("R_PKG_TO_BOARD0_MAG", src)
        # 手册 Rev A 没有轴向图，敏感轴未标定这件事必须留在代码里，
        # 而不是靠谁记得。
        self.assertRegex(src, r"#define\s+PK_BOARD_MAG_AXES_CALIBRATED\s+false")

    def test_imu_task_no_longer_carries_a_hardcoded_mount_quaternion(self):
        # 旧 breakout 的 PK_IMU_MOUNT_QUAT_* 必须已从代码中消失，
        # 否则会出现「两份安装常量、其中一份没人维护」。
        for name in ("imu_task.h", "imu_task.c"):
            text = (ROOT / "firmware" / "main" / name).read_text(encoding="utf-8")
            code = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
            with self.subTest(file=name):
                self.assertNotIn("PK_IMU_MOUNT_QUAT_", code)
        imu_c = (ROOT / "firmware" / "main" / "imu_task.c").read_text(encoding="utf-8")
        self.assertIn("pk_board_imu_body_fix_quat", imu_c)

    def test_board_to_body_matrix_is_a_proper_rotation(self):
        # det 必须为 +1。det = -1 说明有人把 KiCad 那个左手 (x, y_down, z_out)
        # 三元组直接当右手系用了，结果是一个镜像而不是旋转——姿态会左右反。
        src = PK_BOARD_C.read_text(encoding="utf-8")
        m = re.search(
            r"R_BOARD_TO_BODY\[3\]\[3\]\s*=\s*\{(.*?)\n\};", src, flags=re.S
        )
        self.assertIsNotNone(m, "找不到 R_BOARD_TO_BODY")
        vals = [float(v) for v in re.findall(r"(-?\d+(?:\.\d+)?)f", m.group(1))]
        self.assertEqual(len(vals), 9)
        a = [vals[0:3], vals[3:6], vals[6:9]]
        det = (
            a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
            - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
            + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0])
        )
        self.assertTrue(
            math.isclose(det, 1.0, abs_tol=1e-6),
            f"R_BOARD_TO_BODY 的行列式是 {det}，不是 +1（镜像不是旋转）",
        )


if __name__ == "__main__":
    unittest.main()
