#!/usr/bin/env python3
"""构建期护栏：检查"调度器启动前可用内部堆"还剩多少。

为什么需要这个（2026-08-01 花了一整轮排查才定位）
------------------------------------------------
ESP32-P4 rev<v3 的内存布局里（IDF components/heap/port/esp32p4/memory_layout.c），
高位那两段内部 RAM 被标为 startup_stack——**vTaskStartScheduler 之前不可分配**。
那段窗口里系统能用的只有一段：

    [_heap_start_low, APP_USABLE_DIRAM_END)     ← 本工程实测仅 ~65 KB

而 dsp_task 等模块的静态数据已经把它吃到只剩一两 KB。一旦余量不够，
FreeRTOS 在 vTaskStartScheduler 里分配 2 KB 定时器任务栈就会失败：

    assert failed: vApplicationGetTimerTaskMemory port_common.c:97

现象是开机 boot loop，且**与改动的语义完全无关**——往内部 .bss 里加 2 KB
无意义填充同样必崩，撤掉就好。没有这道检查，下一个往 dram0 加静态数组的人
只会看到一个莫名其妙的启动断言，根本联想不到是自己那个数组。

对策：大的、冷的、不做 DMA 的静态数据一律 EXT_RAM_BSS_ATTR 放 PSRAM
（pk_tile_loader.c 顶部有实例与说明）。

用法：check_early_heap.py <elf> [--min-free BYTES]
"""

import argparse
import sys

# heap/port/esp32p4/memory_layout.c（rev<v3 分支）：
#   ROM_STACK_START      = SOC_ROM_STACK_START = 0x4ff3cfc0
#   APP_USABLE_DIRAM_END = ROM_STACK_START - SOC_ROM_STACK_SIZE(0x2000)
APP_USABLE_DIRAM_END = 0x4FF3AFC0

# 阈值按**实测悬崖位置**定，不是拍脑袋的安全余量——这段窗口在初始化期间几乎
# 被吃干净，链接期看到的 65 KB 里真正剩给定时器任务栈的只有一两 KB：
#
#     64,976 B (0x0FDD0) → 必崩（vApplicationGetTimerTaskMemory 断言）
#     66,368 B (0x10340) → 稳定启动
#     91,664 B (0x16610) → 当前基线（2026-08-03 把 dsp_task 的 s_decoder
#                          32,780 B 移进 PSRAM 之后）
#
# 阈值取 80,000：它不是"崩溃线"（那在 65,000 附近），而是**预警线**——留出
# 约 15 KB 的反应余地，让人在还有得改的时候就看到，而不是等贴着悬崖才报。
#
# 想再腾余量，dram0 里剩下的大户（nm 按地址段查得，别信 map 的 .o 归类，
# 那个会漏）：s_mag_buf 8 KB 与 s_iq_buf 8 KB（**都不要动**：解调内循环按
# 采样点访问，且 IQ 缓冲是 USB 落点、可能要 DMA 能力）、adsb_list 的
# s_scratch 7 KB（其余五份同类快照都已 EXT_RAM_BSS_ATTR，只有它漏了，是
# 现成的下一块）、cpr_decode 的 s_table 5 KB、s_icao_seen 4 KB。
DEFAULT_MIN_FREE = 80000


def read_symbol(elf_path, name):
    from elftools.elf.elffile import ELFFile

    with open(elf_path, "rb") as f:
        elf = ELFFile(f)
        for sec in elf.iter_sections():
            if sec.header["sh_type"] != "SHT_SYMTAB":
                continue
            for sym in sec.iter_symbols():
                if sym.name == name:
                    return sym["st_value"]
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("elf")
    ap.add_argument("--min-free", type=int, default=DEFAULT_MIN_FREE)
    args = ap.parse_args()

    heap_start = read_symbol(args.elf, "_heap_start_low")
    if heap_start is None:
        # 换了芯片修订分支（rev>=v3 用 _heap_start）就不是这套布局了，
        # 那时低区一直延伸到 0x4ffbafc0，本检查失去意义，静默跳过。
        print("[early-heap] 未找到 _heap_start_low，跳过检查（非 rev<v3 布局？）")
        return 0

    free = APP_USABLE_DIRAM_END - heap_start
    msg = (f"[early-heap] 调度器启动前可用内部堆 {free} B "
           f"(_heap_start_low=0x{heap_start:08x})")

    if free < args.min_free:
        print(msg, file=sys.stderr)
        print(f"[early-heap] 错误：余量低于 {args.min_free} B。继续往内部 .bss 加静态数据"
              f"会在开机时触发 vApplicationGetTimerTaskMemory 断言 boot loop。",
              file=sys.stderr)
        print("[early-heap] 处理：把大的冷静态数据加 EXT_RAM_BSS_ATTR 移到 PSRAM"
              "（示例见 main/pk_tile_loader.c 顶部注释），或减小 IRAM 占用。",
              file=sys.stderr)
        return 1

    print(msg + " — OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
