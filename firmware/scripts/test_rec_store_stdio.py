#!/usr/bin/env python3
"""session.json 写入不得恢复到会破坏 ESP32-P4 堆的 stdio 缓冲路径。"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def code_only(path: Path) -> str:
    text = path.read_text(encoding="utf-8")
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//.*", "", text)


class RecStoreStdioContract(unittest.TestCase):
    def test_session_json_disables_stdio_buffer_before_first_write(self) -> None:
        source = code_only(ROOT / "main" / "pk_rec_store_fs.c")
        start = source.index("static void write_session_json_locked")
        body = source[start : source.index("static bool ensure_session_open_locked", start)]
        unbuffered = body.index("setvbuf(fp, NULL, _IONBF, 0)")
        first_write = body.index("fprintf(fp")
        self.assertLess(unbuffered, first_write)

    def test_temporary_heap_probes_are_not_shipped(self) -> None:
        source = code_only(ROOT / "main" / "pk_rec_store_fs.c")
        self.assertNotIn("rec_store_heap_probe", source)
        self.assertNotIn("heap_caps_check_integrity_all", source)


if __name__ == "__main__":
    unittest.main()
