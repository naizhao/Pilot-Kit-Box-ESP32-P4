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
CC13x2/CC26x2 TRM §8、§10 与 IMAGE_VALID_CONF 的定义明确支持空片首刷：没有
合法镜像时，启动代码自动进入 ROM 串行 bootloader；BL_CONFIG 的擦除/复位语义
为 0xC5FFFFFF，其中 BOOTLOADER_ENABLE=0xC5。TI E2E 对裸片问题也给出了同样
结论。空片不需要先经外部仿真器烧一次。

这道门禁保护的是**烧完以后**：一旦写入合法镜像，同时把 bootloader 或本板
使用的 GPIO backdoor 关掉，后续 SSI0 更新就会失效，只能靠调试口恢复。

镜像**短于 CCFG 区**也必须拒绝，但原因不是“空白 CCFG 会关 bootloader”：它
无法写入合法 IMAGE_VALID_CONF 和本板要求的 backdoor 配置，应用不会形成完整、
可启动且可现场升级的镜像。擦除态仍应留在 ROM bootloader，属于可恢复失败。
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
                f"无法写入 IMAGE_VALID_CONF 与本板 backdoor 配置；"
                f"应用不会成为完整可启动镜像"]
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
