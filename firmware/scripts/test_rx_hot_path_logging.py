#!/usr/bin/env python3
"""RX 热路径不得同步写控制台（P1-D 的结构守卫）。

为什么是结构守卫，以及它证明不了什么
------------------------------------
被约束的东西是**日志等级**：默认生产等级（INFO）下 `ESP_LOGD` 一个字节都不
出，`ESP_LOGI` 会把 115200 波特的控制台真占住。这条差别在 host 上不可执行
——`esp_log.h` 在 host 侧本来就是丢弃桩，两种等级跑出来一模一样。

所以这里测源码结构，并且明说它的边界：证明的是"这些函数里没有 INFO 级
日志"，**不是**"运行时真的不阻塞"。阻塞时间的推算写在各条断言的注释里，
那部分靠的是波特率算术，不是测出来的。

两条纪律
--------
1. **先剥注释**：解释"为什么降成 ESP_LOGD"的注释里必然写着 `ESP_LOGI`。
2. **按函数体定位**：`dashboard_emit_and_reset()` 的 1 Hz 单行 INFO 是
   **允许**的，全文件 grep 会把它一起判死，守卫只好被放宽成无意义。

切片器为什么值得信
------------------
按函数体定位就得切源码，而切错**不会报错、只会漏检**（切短了，后面的
ESP_LOGI 就看不见，绿灯不携带任何信息）。所以下面 `FunctionSlicerTest`
拿含 `"/*"`、`"{"`、`"}"` 的字符串字面量当坏样本先验一遍切片器本身。

用法
----
    python3 firmware/scripts/test_rx_hot_path_logging.py

由 `firmware/test/run_host_tests.py` 自动收进 Python 测试那一组。
"""

from __future__ import annotations

import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"


def strip_comments(text: str) -> str:
    """剥掉 /* */ 与 // 注释；字符串字面量原样保留。

    保留字符串是刻意的：断言看的是 `ESP_LOGI(` 这样的调用，格式串必须留在
    原地。注释用等长空白替换，行列位置不变，报错信息才对得上源文件。
    """
    out, i, n = [], 0, len(text)
    while i < n:
        c, nxt = text[i], text[i + 1 : i + 2]
        if c in '"\'':                       # 字符串/字符字面量：整段照抄
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            out.append(text[i : j + 1])
            i = j + 1
        elif c == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(" " * (j - i))
            i = j
        elif c == "/" and nxt == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def function_body(text: str, name: str) -> str:
    """取出 C 函数 `name` 的函数体（含花括号），注释已剥。

    定义的识别靠本仓库的排版约定：函数定义从第 0 列起，且名字后紧跟形参表。
    调用点（`modes_ingest_init(on_ingest_msg, NULL)`）名字后面没有 `(`，
    声明则以 `;` 收尾——两者都进不了这个锚点。

    找不到就抛异常。"函数改名了所以守卫恒真"是这类测试最常见的失效方式，
    必须响一声。
    """
    code = strip_comments(text)
    m = re.search(rf"^[A-Za-z_][^;\n]*\b{re.escape(name)}\s*\(", code, re.M)
    if m is None:
        raise AssertionError(f"找不到函数定义 {name}()——它是不是改名了？")

    start = code.index("{", m.end())
    depth, i, n = 0, start, len(code)
    while i < n:
        c = code[i]
        if c in '"\'':                       # 字面量里的花括号不算数
            i += 1
            while i < n and code[i] != c:
                i += 2 if code[i] == "\\" else 1
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return code[start : i + 1]
        i += 1
    raise AssertionError(f"{name}: 花括号未配平")


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


# 逐帧路径里一条 INFO 的代价：ESP_LOGI 带 "I (12345) adsb: " 前缀后约 90
# 字节，115200 波特（10 位/字节）≈ 7.8 ms。忙空域每秒几百帧 → RX 任务一秒
# 里要等掉几百个 7.8 ms，而 921600 波特的链路 RX 环只有 4096 字节（≈ 44 ms
# 的数据量）。这就是"帧丢在 UART 驱动里且无计数"的机制。
FORBIDDEN_LEVELS = ("ESP_LOGI(", "ESP_LOGW(", "ESP_LOGE(")


class FunctionSlicerTest(unittest.TestCase):
    """先验切片器本身：它切错只会漏检，不会报错。"""

    SAMPLE = '''
static void decoy(void) { ESP_LOGI(TAG, "decoy"); }

/* 注释里提到 ESP_LOGI(TAG, "历史原因") 也不该被算进函数体。 */
static void target(int x)
{
    const char *s = "}{ /* 这不是注释 */ // 也不是";
    if (x) {
        ESP_LOGD(TAG, "inner");
    }
}

static void after(void) { ESP_LOGI(TAG, "after"); }
'''

    def test_slices_exactly_the_target_body(self) -> None:
        body = function_body(self.SAMPLE, "target")
        self.assertIn("ESP_LOGD", body)
        # 切早了会带上 decoy 的、切晚了会带上 after 的。
        self.assertNotIn("decoy", body)
        self.assertNotIn("after", body)
        # 字面量里的 '}' 不能提前收尾，'/*' 不能吃掉后面的代码。
        self.assertIn('"}{ /* 这不是注释 */ // 也不是"', body)
        self.assertTrue(body.endswith("}"))

    def test_comment_mention_is_not_a_hit(self) -> None:
        """注释里的 ESP_LOGI 必须被剥掉，否则等于禁止写解释。"""
        self.assertNotIn("ESP_LOGI", function_body(self.SAMPLE, "target"))

    def test_missing_function_is_loud(self) -> None:
        with self.assertRaises(AssertionError):
            function_body(self.SAMPLE, "no_such_function")


class RxHotPathLoggingTest(unittest.TestCase):
    """adsb_link_task.c：逐帧与长报告不得是生产等级日志。"""

    def setUp(self) -> None:
        self.link = read(MAIN / "adsb_link_task.c")

    def test_per_frame_handler_has_no_production_level_logging(self) -> None:
        """on_ingest_msg() 每一帧 CRC-ok 报文调用一次——最热的那一段。

        允许 ESP_LOGD：CONFIG_LOG_MAXIMUM_LEVEL=INFO（本工程默认）时它在
        编译期就没了，阻塞预算恒为 0；要看逐帧解码就把等级开到 DEBUG，那是
        排障场景，丢帧可以接受。
        """
        body = function_body(self.link, "on_ingest_msg")
        for lvl in FORBIDDEN_LEVELS:
            # 用 assertFalse 而不是 assertNotIn：后者失败时会把整个函数体
            # 打进报错信息，判据被自己的输出淹掉。
            self.assertFalse(
                lvl in body,
                f"on_ingest_msg() 里出现 {lvl}——逐帧日志把 RX 任务按在 115200 "
                f"波特的控制台上（约 7.8 ms/条），链路 4096 字节的 RX 环会溢出，"
                f"帧丢在驱动里且无计数。改用 ESP_LOGD。")

    def test_aircraft_summary_has_no_production_level_logging(self) -> None:
        """30 分钟一次的多行汇总：最多 ~26 行 × ~10 ms ≈ 260 ms 连续阻塞。

        它跑在 RX 任务的循环里。那 260 ms 内 921600 波特的链路能灌进约
        30 KB，而 RX 环是 4096 字节——溢出是必然，不是概率。
        """
        body = function_body(self.link, "aircraft_summary_emit")
        for lvl in FORBIDDEN_LEVELS:
            self.assertFalse(
                lvl in body,
                f"aircraft_summary_emit() 里出现 {lvl}——这段约 260 ms 阻塞，"
                f"跑在 RX 任务里必然打爆 4096 字节的 RX 环。")

    def test_aircraft_summary_skips_work_when_debug_disabled(self) -> None:
        """光降等级不够：快照 + 逐行 snprintf 不在宏里，仍会照跑。

        aircraft_state_snapshot() 要拷最多 64 个 aircraft_t 且要拿表锁，
        format_aircraft_line() 每架一次 snprintf。所以函数开头要有等级门，
        默认等级下整段直接返回，代价只剩一次 esp_log_level_get()。
        """
        body = function_body(self.link, "aircraft_summary_emit")
        self.assertTrue(
            "esp_log_level_get" in body,
            "aircraft_summary_emit() 缺少日志等级门——ESP_LOGD 不出字，但快照"
            "与格式化仍在 RX 任务里跑，还带着 aircraft_state 的表锁。")
        self.assertLess(
            body.index("esp_log_level_get"), body.index("aircraft_state_snapshot"),
            "等级门排在 aircraft_state_snapshot() 之后——快照照做，等于没门。")

    def test_one_hz_dashboard_may_still_log_at_info(self) -> None:
        """反向断言：1 Hz 单行 dashboard **允许**保留 INFO。

        没有这一条，上面两条会被下一个人顺手扩成全文件禁用 ESP_LOGI，把这
        条链路唯一的常驻可观测输出也删掉。预算：单行约 110 字节 ≈ 9.5 ms，
        每秒一次 ≈ RX 任务 1% 的时间，这一秒内 RX 环最多进 ~1 KB，远低于
        4096 字节。
        """
        body = function_body(self.link, "dashboard_emit_and_reset")
        self.assertTrue("ESP_LOGI(" in body,
                        "1 Hz dashboard 的 INFO 行不见了——那是这条链路唯一的"
                        "常驻可观测输出，不该被热路径的守卫顺手带走。")


class UartSinkDropVisibilityTest(unittest.TestCase):
    """record_sink_uart 的 dropped 必须有生产消费者。"""

    def setUp(self) -> None:
        self.sink = read(MAIN / "record_sink_uart.c")
        self.diag = read(MAIN / "diag_page.c")
        self.link = read(MAIN / "adsb_link_task.c")

    def test_write_path_never_prints(self) -> None:
        """write() 是 record_dispatch() 的同步调用点，绝不能碰 stdout。"""
        body = function_body(self.sink, "uart_write")
        for bad in ("printf(", "fwrite(", "fputs(", "puts(", "ESP_LOG"):
            self.assertFalse(
                bad in body,
                f"uart_write() 里出现 {bad}——生产侧一旦做输出，P1-D 就退回"
                f"原样了（RX 任务同步等控制台）。")

    def test_enqueue_uses_zero_timeout(self) -> None:
        """超时必须是字面 0：非零超时 = 队满时回压 RX 任务。"""
        body = function_body(self.sink, "uart_write")
        self.assertRegex(
            body, r"xQueueSend\s*\(\s*s_queue\s*,\s*&item\s*,\s*0\s*\)",
            "入队没有用 xQueueSend(s_queue, &item, 0)——非零超时会让队满时"
            "阻塞 record_dispatch()，也就是阻塞 RX 任务。")

    def test_task_create_failure_releases_the_queue(self) -> None:
        """起不来 drain 任务就不注册 sink，队列必须还回去。

        128 槽 × 40 字节 ≈ 5 KB。P4 rev<v3 在调度器启动前只有 65 KB 内部堆、
        余量约 1.5 KB（项目记忆 early-heap cliff），泄漏这一块不是"反正也不大"。
        """
        body = function_body(self.sink, "record_sink_uart_create")
        self.assertTrue(
            "vQueueDelete" in body,
            "record_sink_uart_create() 在 xTaskCreatePinnedToCore 失败后没有"
            "vQueueDelete(s_queue)——队列泄漏，且 s_queue 非空会让 stats() "
            "谎报 sink 还活着。")

    def test_dropped_counter_has_a_production_consumer(self) -> None:
        """没有消费者的计数器等于没有计数器。

        P1-D 之前丢帧是"丢在 UART 驱动里、无计数"。如果修完只是把丢弃搬进
        自己的计数器却没人读，运维侧看到的东西一点没变。
        """
        self.assertTrue(
            "record_sink_uart_stats(" in strip_comments(self.diag),
            "diag_page.c 没有调用 record_sink_uart_stats()——UART sink 的丢弃"
            "数没有任何生产消费者。")

    def test_dashboard_reports_drops_only_when_dropping(self) -> None:
        """看串口的人恰恰是被丢弃直接影响的那个人，得当场告诉他。

        但只在真丢的时候出一行：常驻打印会给已经拥塞的控制台再添噪声。
        预算：这一行约 80 字节 ≈ 7 ms，1 Hz 且仅在丢弃期间，相对同期
        256 行/秒的正常输出是 0.4%。
        """
        body = function_body(self.link, "dashboard_emit_and_reset")
        self.assertTrue("record_sink_uart_stats(" in body,
                        "UART 丢弃行不在 1 Hz dashboard 里——别的位置都在更热"
                        "的路径上。")
        # 门必须是"本窗口有**新增**丢弃"，也就是与窗口基线比较。
        # 只写 `if (record_sink_uart_stats(...))` 是无条件打印：计数是自启动
        # 累计的，一旦丢过一次就会每秒都打，而那正是拥塞时最不该做的事。
        # 早先这里写的是"函数体里出现过 uart_drop 就算过"——那种写法连
        # `if (0 && ...)` 都杀不掉，绿灯不携带信息。
        self.assertRegex(
            body, r"uart_drop\s*!=\s*s_win_uart_drop",
            "UART 丢弃行没有与 1 Hz 窗口基线比较——计数是自启动累计的，"
            "不比基线就会在丢过一次之后每秒都打，给已经拥塞的控制台加负载。")


if __name__ == "__main__":
    unittest.main(verbosity=2)
