#!/usr/bin/env python3
"""构建后门禁：核对镜像里 CCFG 的 BL_CONFIG 字段。

为什么要在构建里卡这一道
------------------------
SimpleLink SDK 自带的 ccfg.c 默认 `BOOTLOADER_ENABLE = 0x00`（禁用 ROM
bootloader）。只要有人把 Makefile 里的本地 ccfg.c 去掉、或者升级 SDK 时
把它换回默认，编译**照样通过**、镜像**照样能跑**，唯一的区别是这颗片子
从此只能靠 cJTAG 仿真器更新——而 RP2040 的 SSI0 代刷通道会彻底失效。

这种退化没有任何运行期症状，等发现时板子已经装机了。所以判据必须落在
**产物字节**上，而不是"编译过了"。

字段定义：TRM SWCU185G 表 11-15（BL_CONFIG，CCFG 偏移 0x1FD8）
    bits 31-24 BOOTLOADER_ENABLE  只有 0xC5 是 enabled，其余一律 disabled
    bit  16    BL_LEVEL           backdoor 引脚的有效电平
    bits 15-8  BL_PIN_NUMBER      backdoor 引脚的 DIO 号
    bits 7-0   BL_ENABLE          0xC5 = backdoor 使能
"""
import sys
import pathlib

# 位域定义只留一份：tools/cc13_ccfg.py。两处各抄一份迟早会漂，而漂掉的那次
# 不会有任何运行期症状——只会在某天需要升级时发现通道没了。
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
try:
    import cc13_ccfg
except ImportError:
    print("[ccfg] 找不到 tools/cc13_ccfg.py —— 位域判据在那里，不能绕过",
          file=sys.stderr)
    sys.exit(2)


def main(argv):
    if len(argv) != 2:
        print("用法: check_ccfg.py <镜像.bin>", file=sys.stderr)
        return 2
    data = pathlib.Path(argv[1]).read_bytes()
    probs = cc13_ccfg.problems(data)
    if probs:
        print("[ccfg] 产物的 BL_CONFIG 不符合预期：", file=sys.stderr)
        for x in probs:
            print(f"  {x}", file=sys.stderr)
        print("\n  检查 firmware/cc1312r/ccfg.c 是否仍被 Makefile 编入"
              "（SDK 自带的那份默认禁用 bootloader）。", file=sys.stderr)
        return 1
    print(f"ok  ccfg: {cc13_ccfg.describe(data)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
