#!/usr/bin/env python3
"""test_cc13_ccfg.py — CC13x2 镜像的防砖判据（tools/cc13_ccfg.py）单测。

这条判据保护的是**升级通道本身**：烧进一版把 bootloader/backdoor 关掉的镜像，
后果不是"这次没烧好"，而是**以后再也烧不了**——本板没有调试座、也没有那三根
网络的测试点，只能拆机焊 QFN 引脚再上 cJTAG 仿真器。

所以判据必须自己有测试：它一旦漏判，没有任何运行期症状，等发现时已经晚了。
"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
import cc13_ccfg  # noqa: E402

FAIL = 0


def check(cond, msg):
    global FAIL
    if not cond:
        print(f"  [FAIL] {msg}")
        FAIL += 1


def image(word: int, size: int = 360448) -> bytes:
    """造一张 352KB 镜像，CCFG 的 BL_CONFIG 置成 word（小端）。"""
    buf = bytearray(b"\xff" * size)
    if size >= cc13_ccfg.BL_CONFIG_END:
        buf[cc13_ccfg.BL_CONFIG_OFFSET:cc13_ccfg.BL_CONFIG_END] = \
            word.to_bytes(4, "little")
    return bytes(buf)


# 本仓库 firmware/cc1312r/ccfg.c 编出来的值（已从真实产物字节核过）
GOOD = 0xC5FF0DC5


def test_good_image_passes():
    check(cc13_ccfg.problems(image(GOOD)) == [],
          f"合格镜像被拒：{cc13_ccfg.problems(image(GOOD))}")
    f = cc13_ccfg.parse(image(GOOD))
    check(f["BOOTLOADER_ENABLE"] == 0xC5, "BOOTLOADER_ENABLE 解析错")
    check(f["BL_ENABLE"] == 0xC5, "BL_ENABLE 解析错")
    check(f["BL_PIN_NUMBER"] == 13, f"BL_PIN_NUMBER 解析成 {f['BL_PIN_NUMBER']}")
    check(f["BL_LEVEL"] == 1, "BL_LEVEL 解析错")


def test_sdk_default_rejected():
    """SDK 自带 ccfg.c 的默认值：BOOTLOADER_ENABLE=0x00。这是最可能踩的一脚
    ——只要有人把本地 ccfg.c 漏掉，编出来就是这个。"""
    probs = cc13_ccfg.problems(image(0x00FFFFFF))
    check(len(probs) >= 1, "SDK 默认镜像竟然通过了")
    check(any("BOOTLOADER_ENABLE" in p for p in probs),
          f"没点名 BOOTLOADER_ENABLE：{probs}")


def test_blank_ccfg_rejected():
    """擦除态全 0xFF —— 空片就是这样，烧这种镜像等于把通道关掉。"""
    probs = cc13_ccfg.problems(image(0xFFFFFFFF))
    check(any("BOOTLOADER_ENABLE" in p for p in probs), f"全 FF 未被拒：{probs}")
    check(any("BL_ENABLE" in p for p in probs), f"全 FF 未点名 backdoor：{probs}")


def test_each_field_guarded_individually():
    """逐个字段单独打坏，每个都必须被单独点名。
    只测"整体拒绝"会漏掉"某个字段根本没在判据里"这种情况。"""
    cases = {
        "BOOTLOADER_ENABLE": GOOD & 0x00FFFFFF,          # → 0x00
        "BL_ENABLE": (GOOD & 0xFFFFFF00) | 0xFF,          # → 0xFF
        "BL_PIN_NUMBER": (GOOD & 0xFFFF00FF) | (0x0C << 8),  # DIO12，错一个脚
        "BL_LEVEL": GOOD & ~(1 << 16),                    # → 低有效
    }
    for field, word in cases.items():
        probs = cc13_ccfg.problems(image(word))
        check(any(field in p for p in probs),
              f"打坏 {field}（BL_CONFIG=0x{word:08X}）却没被点名：{probs}")
        # 其余字段不该被误报
        others = [p for p in probs if field not in p]
        check(not others, f"打坏 {field} 时误报了别的字段：{others}")


def test_short_image_rejected():
    """镜像短于 CCFG 区同样致命：烧录整片擦除，CCFG 变 0xFF，后果一样。
    这条最容易被漏——它不是"值不对"，是"根本没有值"。"""
    probs = cc13_ccfg.problems(image(GOOD, size=1024))
    check(len(probs) == 1, f"短镜像应恰好报一条，实际 {probs}")
    check("覆盖不到 CCFG" in probs[0], f"短镜像报错没说清原因：{probs}")

    # 差一个字节也必须拒（边界）
    probs = cc13_ccfg.problems(image(GOOD, size=cc13_ccfg.BL_CONFIG_END - 1))
    check(len(probs) == 1, "刚好差 1 字节的镜像未被拒")
    # 刚好够就该能解析（值本身另说）
    ok = image(GOOD, size=cc13_ccfg.BL_CONFIG_END)
    check(cc13_ccfg.problems(ok) == [], "长度刚好够的镜像被误拒")


def test_describe_is_readable():
    s = cc13_ccfg.describe(image(GOOD))
    check("0xC5FF0DC5" in s, f"摘要里没有原值：{s}")
    check("DIO13" in s, f"摘要里没有引脚号：{s}")
    s = cc13_ccfg.describe(image(GOOD, size=16))
    check("不可读" in s, f"短镜像的摘要应说明不可读：{s}")


def main():
    test_good_image_passes()
    test_sdk_default_rejected()
    test_blank_ccfg_rejected()
    test_each_field_guarded_individually()
    test_short_image_rejected()
    test_describe_is_readable()
    if FAIL:
        print(f"test_cc13_ccfg: {FAIL} FAIL")
        return 1
    print("test_cc13_ccfg: all OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
