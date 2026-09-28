#!/usr/bin/env python3
"""ESP32-P4 启动任务栈不得落入 8 KiB SPM heap。

ESP-IDF #19020：P4 的 SPM 被当作通用 MALLOC_CAP_INTERNAL heap，但当前
cache-disable 安全检查不把 0x30100000..0x30102000 识别为 DRAM。main task
栈一旦装进该池，首次 NVS/partition 操作就会在关 Cache 前断言。

项目暂时采用 Espressif issue 给出的配置规避：main task 栈至少等于整个
8 KiB SPM 区，使动态分配不可能装入扣除 .spm.text/.spm.data 后的剩余池。
这个测试同时约束可复现构建默认值与当前已生成配置，避免“本机能启动、干净
构建又回到 4096”的分叉。
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path


FIRMWARE = Path(__file__).resolve().parents[1]
SPM_TOTAL_BYTES = 0x2000
KEY = "CONFIG_ESP_MAIN_TASK_STACK_SIZE"


def read_config_int(path: Path, key: str) -> int:
    text = path.read_text(encoding="utf-8")
    matches = re.findall(rf"(?m)^{re.escape(key)}=(\d+)$", text)
    if len(matches) != 1:
        raise AssertionError(
            f"{path}: {key} 应且只能出现一次，实际 {len(matches)} 次"
        )
    return int(matches[0])


class StartupStackMemoryContract(unittest.TestCase):
    def test_main_task_stack_cannot_fit_in_spm_heap(self) -> None:
        for name in ("sdkconfig.defaults", "sdkconfig"):
            path = FIRMWARE / name
            with self.subTest(config=name):
                stack_bytes = read_config_int(path, KEY)
                self.assertGreaterEqual(
                    stack_bytes,
                    SPM_TOTAL_BYTES,
                    f"{name}: main task 栈 {stack_bytes} B 仍能装入 P4 的 8 KiB "
                    "SPM heap；首次 NVS/partition 访问会触发 cache-disable "
                    "栈地址断言（ESP-IDF #19020）",
                )


if __name__ == "__main__":
    unittest.main()
