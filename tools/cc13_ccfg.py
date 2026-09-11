#!/usr/bin/env python3
"""cc13_ccfg.py — CC13x2 镜像里 CCFG/BL_CONFIG 的解析与防砖判据（单一出处）。

为什么要单独一个模块
--------------------
同一套位域现在有两个消费者：
  * firmware/cc1312r/check_ccfg.py —— 构建门禁，保证**我们编出来的**镜像是对的
  * tools/cc13_flash.py            —— 烧录门禁，保证**要烧进去的**镜像是对的
两处各抄一份位域迟早会漂，而漂掉的那一次不会有任何运行期症状——只会在某天
需要升级时发现通道没了。本项目在"写死副本"上栽过不止一次，所以这里只留一份。

判据来源：TRM SWCU185G 表 11-15（BL_CONFIG，CCFG 偏移 0x1FD8）
    bits 31-24 BOOTLOADER_ENABLE  只有 0xC5 是 enabled，其余一律 disabled
    bit  16    BL_LEVEL           backdoor 引脚的有效电平
    bits 15-8  BL_PIN_NUMBER      backdoor 引脚的 DIO 号
    bits 7-0   BL_ENABLE          0xC5 = backdoor 使能

为什么这是"防砖"而不是"洁癖"
----------------------------
CC1312R 的串行 bootloader 是**现场升级**机制，不是**首次烧录**机制：空片的
CCFG 是擦除态全 0xFF，BOOTLOADER_ENABLE≠0xC5，bootloader 根本不响应（实测
16 个变体全 0xFF）。所以一旦烧进一版把 bootloader 关掉的镜像，就**永久失去
升级通道**，只能再上一次 cJTAG 仿真器——而本板既没有调试座、也没有这三根
网络的测试点，得拆机焊 QFN 引脚。

顺带：镜像**短于 CCFG 区**同样致命。烧录前会整片擦除，CCFG 区随之变成 0xFF；
镜像又没覆盖到那里，结果和"显式关掉 bootloader"一模一样。
"""

# CCFG 在 flash 末尾 8KB 区的尾部；352KB 器件上 CCFG 基址 = 0x58000-0x2000。
# BL_CONFIG 偏移 0x1FD8 → 绝对 0x57FD8。整片镜像从 0x0 起，故文件偏移相同。
BL_CONFIG_OFFSET = 0x57FD8
BL_CONFIG_END = BL_CONFIG_OFFSET + 4

# 本板要求的取值。BL_PIN_NUMBER=13 是 DIO13 = SUBG_SYNC ← RP2040 GPIO15；
# BL_LEVEL=1 是高有效（GPIO15 复位后输入+下拉，低有效会让每次上电都进
# bootloader，等于把产品做死）。详见 firmware/cc1312r/ccfg.c。
EXPECT = {
    "BOOTLOADER_ENABLE": 0xC5,
    "BL_ENABLE": 0xC5,
    "BL_PIN_NUMBER": 13,
    "BL_LEVEL": 1,
}

WHY = {
    "BOOTLOADER_ENABLE": "ROM bootloader 会被禁用 → 永久失去 SSI0 升级通道，"
                         "只能拆机焊 QFN 引脚再上 cJTAG 仿真器",
    "BL_ENABLE": "backdoor 会被禁用 → 一旦烧进跑不起来的固件就救不回来",
    "BL_PIN_NUMBER": "backdoor 必须挂在 DIO13（= 本板 SUBG_SYNC ← RP2040 GPIO15），"
                     "挂错脚等于没有",
    "BL_LEVEL": "必须高有效：GPIO15 复位后是输入+下拉，低有效会让 CC1312R "
                "每次上电都进 bootloader 而不跑应用固件",
}


def parse(data: bytes) -> dict:
    """从整片镜像里取出 BL_CONFIG 各字段。镜像太短则抛 ValueError。"""
    if len(data) < BL_CONFIG_END:
        raise ValueError(
            f"镜像只有 {len(data)} 字节，覆盖不到 CCFG（需要 ≥ {BL_CONFIG_END}）")
    word = int.from_bytes(data[BL_CONFIG_OFFSET:BL_CONFIG_END], "little")
    return {
        "_word": word,
        "BOOTLOADER_ENABLE": (word >> 24) & 0xFF,
        "BL_ENABLE": word & 0xFF,
        "BL_PIN_NUMBER": (word >> 8) & 0xFF,
        "BL_LEVEL": (word >> 16) & 0x01,
    }


def problems(data: bytes) -> list:
    """返回这份镜像的防砖问题清单；空列表 = 可以安全烧录。

    刻意**不抛异常**：调用方要能把所有问题一次列全给用户看，而不是修一个
    报一个。镜像过短单独成一条，因为它的后果和"显式关掉"一样但原因完全不同。
    """
    if len(data) < BL_CONFIG_END:
        return [f"镜像只有 {len(data)} 字节，覆盖不到 CCFG 区"
                f"（需要 ≥ {BL_CONFIG_END} 字节）—— 烧录会整片擦除，"
                f"CCFG 随之变成 0xFF，后果与显式关掉 bootloader 相同"]
    f = parse(data)
    out = []
    for name, want in EXPECT.items():
        got = f[name]
        if got != want:
            out.append(f"{name} = 0x{got:02X}（应为 0x{want:02X}）—— {WHY[name]}")
    return out


def describe(data: bytes) -> str:
    """一行摘要，给日志用。"""
    try:
        f = parse(data)
    except ValueError as e:
        return f"BL_CONFIG 不可读：{e}"
    return (f"BL_CONFIG=0x{f['_word']:08X} "
            f"(bootloader={'开' if f['BOOTLOADER_ENABLE'] == 0xC5 else '关'}, "
            f"backdoor={'开' if f['BL_ENABLE'] == 0xC5 else '关'}, "
            f"引脚=DIO{f['BL_PIN_NUMBER']}, "
            f"{'高' if f['BL_LEVEL'] else '低'}有效)")
