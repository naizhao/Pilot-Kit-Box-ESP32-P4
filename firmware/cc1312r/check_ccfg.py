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

# CCFG 在 flash 末尾 8KB 区的尾部；352KB 器件上 CCFG 基址 = 0x58000-0x2000。
# BL_CONFIG 偏移 0x1FD8 → 绝对 0x57FD8。整片镜像从 0x0 起，故文件偏移相同。
BL_CONFIG_OFFSET = 0x57FD8

WANT = {
    "BOOTLOADER_ENABLE": (0xC5, lambda w: (w >> 24) & 0xFF),
    "BL_ENABLE":         (0xC5, lambda w: w & 0xFF),
    "BL_PIN_NUMBER":     (13,   lambda w: (w >> 8) & 0xFF),   # DIO13 = SUBG_SYNC
    "BL_LEVEL":          (1,    lambda w: (w >> 16) & 0x01),  # 高有效
}

WHY = {
    "BOOTLOADER_ENABLE": "ROM bootloader 会被禁用，以后只能靠 cJTAG 仿真器更新",
    "BL_ENABLE":         "backdoor 会被禁用，有有效镜像时进不去 bootloader",
    "BL_PIN_NUMBER":     "backdoor 引脚必须是 DIO13（= 本板 SUBG_SYNC ← RP2040 GPIO15）",
    "BL_LEVEL":          "必须高有效：GPIO15 复位后是输入+下拉，低有效会让每次上电都进 bootloader",
}


def main(argv):
    if len(argv) != 2:
        print("用法: check_ccfg.py <镜像.bin>", file=sys.stderr)
        return 2
    path = pathlib.Path(argv[1])
    data = path.read_bytes()
    if len(data) < BL_CONFIG_OFFSET + 4:
        print(f"[ccfg] 镜像只有 {len(data)} 字节，放不下 CCFG"
              f"（需要 ≥ {BL_CONFIG_OFFSET + 4}）", file=sys.stderr)
        return 1

    word = int.from_bytes(data[BL_CONFIG_OFFSET:BL_CONFIG_OFFSET + 4], "little")
    bad = []
    for name, (want, get) in WANT.items():
        got = get(word)
        if got != want:
            bad.append(f"  {name}: 实际 0x{got:02X}，期望 0x{want:02X} —— {WHY[name]}")

    if bad:
        print(f"[ccfg] BL_CONFIG = 0x{word:08X} 不符合预期：", file=sys.stderr)
        print("\n".join(bad), file=sys.stderr)
        print("\n  检查 firmware/cc1312r/ccfg.c 是否仍被 Makefile 编入"
              "（SDK 自带的那份默认禁用 bootloader）。", file=sys.stderr)
        return 1

    print(f"ok  ccfg: BL_CONFIG=0x{word:08X} "
          f"(bootloader+backdoor 已开, 引脚 DIO13 高有效)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
