/*
 * test_record_sink_backpressure.c — RX 热路径不得被慢速 sink 同步阻塞（P1-D）。
 *
 *   cc -std=c11 -Wall -Wextra -Werror -O2 -pthread \
 *      -I firmware/test/host_stubs -I firmware/main \
 *      -o /tmp/test_record_sink_backpressure \
 *      firmware/test/test_record_sink_backpressure.c \
 *      firmware/main/record_sink.c firmware/main/record_sink_uart.c -lm \
 *   && /tmp/test_record_sink_backpressure
 *
 * 越界/未定义行为检测（可选加强跑法，默认入口不执行）：
 *   cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -pthread \
 *      -I firmware/test/host_stubs -I firmware/main \
 *      -o /tmp/test_record_sink_backpressure_asan \
 *      firmware/test/test_record_sink_backpressure.c \
 *      firmware/main/record_sink.c firmware/main/record_sink_uart.c -lm \
 *   && /tmp/test_record_sink_backpressure_asan
 *
 * 数据竞争检测（并发部分的专用跑法）：
 *   cc -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=thread -pthread \
 *      -I firmware/test/host_stubs -I firmware/main \
 *      -o /tmp/test_record_sink_backpressure_tsan \
 *      firmware/test/test_record_sink_backpressure.c \
 *      firmware/main/record_sink.c firmware/main/record_sink_uart.c -lm \
 *   && /tmp/test_record_sink_backpressure_tsan
 *
 * 缺陷（修复前）
 * --------------
 * record_dispatch() 由 adsb_link_task 在每一帧 CRC 有效的 Mode-S 报文上同步
 * 调用，而那个任务是 921600 波特链路唯一的 RX 消费者（RX 环 4096 字节）。
 * 默认的 uart sink 在 write() 里直接 printf 到 115200 波特的控制台：一行约
 * 45 字节 ≈ 3.9 ms。繁忙空域每秒几百帧时，RX 任务绝大部分时间在等控制台，
 * UART 环溢出——帧丢在驱动里，**没有任何计数**能看出来。
 *
 * 判据
 * ----
 * 生产者与消费者被刻意解耦之后，"没有同步输出"这件事变得可观测：
 *   - 一轮只 dispatch、完全不 drain，仍然能全部返回，且队列里**留下**了
 *     min(N, DEPTH) 条在途 —— 如果 write() 还是同步打印，在途恒为 0；
 *   - 溢出部分必须计入 dropped，而不是静默消失；
 *   - 恒等式 written + dropped + pending == dispatch 次数 必须始终成立，
 *     这是"不会无声丢帧"的形式化表述；
 *   - 队满时入队**立刻**失败（host 桩把"本该阻塞的入队"计数出来）；
 *   - drain 任务起不来时 sink 必须对外承认自己不存在，而不是报三个 0。
 *
 * 解耦之后还必须证明**输出本身没被改坏**：把 printf 挪到另一个执行流里，
 * 很容易顺手把顺序、时间戳宽度或十六进制大小写弄错，而这些错误一条都不会
 * 影响行数。所以这里用 fd 级重定向抓 drain 真正写出的字节，逐字节比对多条
 * 不同 ts / 不同长度的行——下游 Pilot-Kit/scripts/adsb_to_track.py 是按
 * "<epoch_ms> *<HEX>;" 硬解析的。
 *
 * 本测试不测"快不快"（host 上的墙钟对目标板没有意义），只测**结构与内容**：
 * 生产路径不做输出、不等消费者、过载有计数、账目对得平、吐出来的字节对。
 */

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "record_sink.h"

/* host_stubs 里的两个注入点（见各自头文件的说明）：
 *   pk_host_queue_blocking_sends —— 队满且超时非零的入队次数，必须恒为 0；
 *   pk_host_task_create_fail     —— 强制 xTaskCreatePinnedToCore 失败。 */
extern unsigned pk_host_queue_blocking_sends;
extern int      pk_host_task_create_fail;

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); \
        printf("         at %s:%d\n", __FILE__, __LINE__); g_fail++; } } while (0)

/* record_sink.c 的默认安装会拉进 file/ble/rec_store 三个 sink，那些各有自己
 * 的 FreeRTOS 依赖。本测试只关心 uart 这一路，所以直接注册它，不走
 * record_sinks_install_defaults()。这三个符号只是为了满足链接。 */
record_sink_t *record_sink_file_create(void) { return NULL; }
record_sink_t *record_sink_ble_create(void) { return NULL; }
record_sink_t *record_sink_rec_store_create(void) { return NULL; }

static void make_record(record_t *rec, int64_t ts_ms, int seq)
{
    memset(rec, 0, sizeof(*rec));
    rec->ts_ms  = ts_ms;
    rec->icao24 = 0x4CA000u + (uint32_t)(seq & 0xFFF);
    rec->df     = 17;
    snprintf(rec->hex, sizeof(rec->hex), "8D4CA%03X994409940838175B284F",
             (unsigned)(seq & 0xFFF));
    rec->hex_len = (uint8_t)strlen(rec->hex);
}

/*
 * 抓取 drain **真正写出去的字节**。
 *
 * 为什么不是重绑 stdout
 * ---------------------
 * 旧写法是 `stdout = fopen("/dev/null")`。它有两个问题：
 *   1. 给 `stdout` 赋值不是可移植的做法——C 标准只保证 stdout 是个能求值成
 *      FILE* 的表达式，不保证是可写的左值（glibc/macOS 恰好是全局变量，
 *      所以"碰巧能跑"）；
 *   2. 更要命的是它只能丢弃、不能核对。真正要断言的是行的内容与顺序，
 *      而不只是"打了几行"。
 *
 * 这里换成 fd 级重定向（dup/dup2）：生产代码里的 printf 无论经过哪一层
 * 缓冲，最终都落到 fd 1，所以抓到的就是生产 drain 这一段的实际输出，不是
 * 它的复制品。
 */
#define CAP_MAX  (64 * 1024)
static char g_cap[CAP_MAX];

static size_t drain_capture(size_t max_lines)
{
    fflush(stdout);
    const int saved = dup(STDOUT_FILENO);
    if (saved < 0) { perror("dup"); exit(2); }

    char tmpl[] = "/tmp/pk_uart_sink_capXXXXXX";
    const int tmp = mkstemp(tmpl);
    if (tmp < 0) { perror("mkstemp"); exit(2); }
    unlink(tmpl);                     /* 只靠 fd，跑完不留文件 */

    if (dup2(tmp, STDOUT_FILENO) < 0) { perror("dup2"); exit(2); }

    const size_t n = record_sink_uart_drain(max_lines);

    fflush(stdout);
    if (dup2(saved, STDOUT_FILENO) < 0) { perror("dup2 restore"); exit(2); }
    close(saved);

    if (lseek(tmp, 0, SEEK_SET) < 0) { perror("lseek"); exit(2); }
    ssize_t got = read(tmp, g_cap, sizeof(g_cap) - 1);
    if (got < 0) { perror("read"); exit(2); }
    g_cap[got] = '\0';
    close(tmp);
    return n;
}

/* 只要计数、不要内容的场合（清场、压力测试）用这个：抓下来直接丢。 */
static size_t drain_quietly(size_t max_lines)
{
    size_t total = 0;
    /* 分批抓，避免一次 drain 的输出超过 CAP_MAX 被截断——被截断本身不影响
     * 计数，但会让 g_cap 里留下半行，误导后面读它的人。 */
    while (total < max_lines) {
        size_t want = max_lines - total;
        if (want > 512) want = 512;
        size_t n = drain_capture(want);
        total += n;
        if (n < want) break;
    }
    return total;
}

/* 队列深度不是公开常量（那是实现细节），从行为上探出来：一直入队直到出现
 * 第一次**新增**丢弃，此时的在途条数就是深度。
 *
 * 注意计数是自启动累计的，必须取增量：直接判 `dropped > 0` 会在上一轮压力
 * 测试留下非零计数时第一次循环就退出，探出来的"深度"是 1，而后续断言仍然
 * 全绿——一个自洽但完全错误的结论。 */
static uint32_t probe_queue_depth(void)
{
    uint32_t base_dropped = 0;
    record_sink_uart_stats(NULL, &base_dropped, NULL);

    uint32_t pending = 0;
    record_t rec;
    for (int i = 0; i < 100000; ++i) {
        make_record(&rec, 1000 + i, i);
        record_dispatch(&rec);
        uint32_t dropped = 0;
        record_sink_uart_stats(NULL, &dropped, &pending);
        if (dropped > base_dropped) break;
    }
    return pending;
}

/*
 * 核心判据：只生产、不消费，dispatch 照样全部返回，而且队列里真的攒下了东西。
 *
 * 「在途 > 0」正是"write() 没有同步打印"的可观测证据：同步实现下每条都被
 * 立刻打掉，在途恒为 0、dropped 恒为 0。
 */
static void test_dispatch_enqueues_instead_of_printing(void)
{
    record_t rec;
    uint32_t w0 = 0, d0 = 0, p0 = 0;
    record_sink_uart_stats(&w0, &d0, &p0);
    CHECK(p0 == 0, "前置条件：队列应为空 pending=%u\n", p0);

    for (int i = 0; i < 8; ++i) {
        make_record(&rec, 2000 + i, i);
        record_dispatch(&rec);
    }

    uint32_t w1 = 0, d1 = 0, p1 = 0;
    record_sink_uart_stats(&w1, &d1, &p1);
    CHECK(p1 == 8, "8 次 dispatch 之后应有 8 条在途，实得 %u"
                   "（0 = write() 仍在同步打印）\n", p1);
    CHECK(w1 == w0, "dispatch 期间不得有任何行被打印 written %u→%u\n", w0, w1);
    CHECK(d1 == d0, "未过载不得丢弃 dropped %u→%u\n", d0, d1);

    /* 消费之后账目要平。 */
    size_t n = drain_quietly(64);
    CHECK(n == 8, "drain 应吐出 8 行，实得 %zu\n", n);
    uint32_t w2 = 0, p2 = 0;
    record_sink_uart_stats(&w2, NULL, &p2);
    CHECK(w2 == w0 + 8, "written 应 +8，实得 %u→%u\n", w0, w2);
    CHECK(p2 == 0, "drain 之后在途应清零，实得 %u\n", p2);
}

/*
 * 输出的**内容**与**顺序**。
 *
 * 只数行数是不够的：一个把两条记录调换、或者把 ts 打成 %d（epoch 毫秒
 * 塞不进 32 位）、或者把 hex 小写掉的实现，行数完全正确。而下游
 * Pilot-Kit/scripts/adsb_to_track.py 是按 "<epoch_ms> *<HEX>;" 逐字节解析
 * 的——这三种错误里的任何一种都会让整份 dump 解析不了或时间轴错乱。
 *
 * 取材刻意分散：
 *   - ts 用真实量级的 epoch 毫秒（1.7e12，超过 2^32），%d/%u 会截断；
 *   - 短帧（DF11，56 bit → 14 个 hex 字符）与长帧（DF17，112 bit → 28 个）
 *     混排，写死长度的实现会露馅；
 *   - 各行内容互不相同，交换任意两条都能被逐字节比较抓到。
 */
struct expect_line {
    int64_t     ts_ms;
    const char *hex;
};

static const struct expect_line kLines[] = {
    { 1715432198765LL, "8D4840D6202CC371C32CE0576098" },   /* 长帧 */
    { 1715432198766LL, "5D4CA9C1B5F2E7"                 },   /* 短帧 */
    { 1715432198999LL, "8D4CA2D699108F9C58C004A1B2C3" },
    { 1715432199000LL, "02E19838B19B0F"                 },
    { 1799999999999LL, "8DA4B8C258C3860C2E5C7F1D9E4A" },   /* 更大的 ts */
};
#define N_LINES  ((int)(sizeof(kLines) / sizeof(kLines[0])))

static void dispatch_expect_line(int i)
{
    record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.ts_ms   = kLines[i].ts_ms;
    rec.df      = 17;
    rec.icao24  = 0x4CA000u + (uint32_t)i;
    snprintf(rec.hex, sizeof(rec.hex), "%s", kLines[i].hex);
    rec.hex_len = (uint8_t)strlen(rec.hex);
    record_dispatch(&rec);
}

/* 期望文本：从 kLines[from] 到 kLines[to-1] 的连续若干行。 */
static void build_expected(char *out, size_t cap, int from, int to)
{
    size_t pos = 0;
    out[0] = '\0';
    for (int i = from; i < to; ++i) {
        int n = snprintf(out + pos, cap - pos, "%lld *%s;\n",
                         (long long)kLines[i].ts_ms, kLines[i].hex);
        if (n < 0 || (size_t)n >= cap - pos) { out[0] = '\0'; return; }
        pos += (size_t)n;
    }
}

static void report_mismatch(const char *what, const char *want, const char *got)
{
    printf("  [FAIL] %s\n", what);
    printf("         期望:\n%s", want);
    printf("         实得:\n%s", got);
    g_fail++;
}

static void test_drain_output_is_exact_and_in_order(void)
{
    drain_quietly(1000000);
    uint32_t pending = 0;
    record_sink_uart_stats(NULL, NULL, &pending);
    CHECK(pending == 0, "前置条件：队列应为空 pending=%u\n", pending);

    for (int i = 0; i < N_LINES; ++i) dispatch_expect_line(i);

    char want[1024];

    /* 第一段：限量 drain，必须**恰好**是最早入队的两条，一字不差。
     * 限量本身也是判据：drain 必须尊重 max_lines，否则持续过载时它会独占
     * 控制台，把别的任务的日志全卡住。 */
    size_t n = drain_capture(2);
    CHECK(n == 2, "限量 drain(2) 应只吐 2 行，实得 %zu\n", n);
    build_expected(want, sizeof(want), 0, 2);
    if (strcmp(want, g_cap) != 0)
        report_mismatch("drain 的前 2 行与入队顺序/格式不符", want, g_cap);

    record_sink_uart_stats(NULL, NULL, &pending);
    CHECK(pending == (uint32_t)(N_LINES - 2),
          "限量 drain 之后应剩 %d 条，实得 %u\n", N_LINES - 2, pending);

    /* 第二段：剩下的必须接着上一段继续，不能重发也不能跳过。 */
    n = drain_capture(64);
    CHECK(n == (size_t)(N_LINES - 2), "剩余 drain 应吐 %d 行，实得 %zu\n",
          N_LINES - 2, n);
    build_expected(want, sizeof(want), 2, N_LINES);
    if (strcmp(want, g_cap) != 0)
        report_mismatch("drain 的后续行与入队顺序/格式不符", want, g_cap);

    record_sink_uart_stats(NULL, NULL, &pending);
    CHECK(pending == 0, "清空后应剩 0 条，实得 %u\n", pending);

    /* 空队列上再 drain 一次：不得有任何输出（哪怕一个换行）。 */
    n = drain_capture(8);
    CHECK(n == 0, "空队列 drain 应返回 0，实得 %zu\n", n);
    CHECK(g_cap[0] == '\0', "空队列 drain 却写出了字节：%s\n", g_cap);
}

/*
 * 持续过载：模拟 100 / 200 / 500 fps 各持续 10 秒，而控制台每秒只吐得动
 * 约 256 行（115200 波特下 45 字节/行的物理上限）。
 *
 * 要证的两件事：
 *   1. 每一次 dispatch 都返回（生产侧无回压）——循环能跑完本身就是证据，
 *      同步实现下这里会真的去写 5000 次 stdout；
 *   2. 账目恒等：written + dropped + pending == 总 dispatch 次数。丢了多少
 *      必须能被读出来，这正是原实现做不到的（帧丢在 UART 驱动里，无计数）。
 */
static void test_sustained_overload_accounts_for_every_record(void)
{
    static const int FPS[] = { 100, 200, 500 };
    const int SECONDS = 10;
    /* 115200 波特 / (45 字节 × 10 位) ≈ 256 行每秒。 */
    const size_t CONSOLE_LINES_PER_SEC = 256;

    for (size_t k = 0; k < sizeof(FPS) / sizeof(FPS[0]); ++k) {
        const int fps = FPS[k];

        /* 归零：把上一轮残留清干净，账目才是这一轮自己的。 */
        drain_quietly(1000000);
        uint32_t w0 = 0, d0 = 0, p0 = 0;
        record_sink_uart_stats(&w0, &d0, &p0);
        CHECK(p0 == 0, "%d fps 前置条件：队列应为空\n", fps);

        /* 一秒切成 10 个 100 ms 片：帧是均匀到达的，drain 任务也是一直在跑
         * 的。把整秒的量一次性灌进去再一次性排空，模拟的是"生产者突发、
         * 消费者停摆"，那会让 200 fps（明明在带宽之内）也溢出队列——那是
         * 建模错误，不是被测行为。 */
        const int SLICES = 10;
        record_t rec;
        long dispatched = 0;
        for (int s = 0; s < SECONDS; ++s) {
            for (int sl = 0; sl < SLICES; ++sl) {
                for (int i = 0; i < fps / SLICES; ++i) {
                    make_record(&rec, 10000 + s * 1000 + sl * 100 + i, i);
                    record_dispatch(&rec);
                    ++dispatched;
                }
                drain_quietly(CONSOLE_LINES_PER_SEC / (size_t)SLICES);
            }
        }

        uint32_t w1 = 0, d1 = 0, p1 = 0;
        record_sink_uart_stats(&w1, &d1, &p1);
        const long written = (long)(w1 - w0);
        const long dropped = (long)(d1 - d0);
        const long pending = (long)p1;

        CHECK(written + dropped + pending == dispatched,
              "%d fps：账目不平 written=%ld dropped=%ld pending=%ld "
              "dispatched=%ld（差额就是无声丢掉的记录）\n",
              fps, written, dropped, pending, dispatched);

        if (fps <= (int)CONSOLE_LINES_PER_SEC) {
            /* 控制台吃得下：一条都不该丢。 */
            CHECK(dropped == 0,
                  "%d fps 在控制台带宽之内，不该有丢弃，实得 %ld\n", fps, dropped);
        } else {
            /* 超出物理带宽必然要丢——关键是丢得有计数，而不是无声无息。 */
            CHECK(dropped > 0,
                  "%d fps 已超过控制台带宽，丢弃计数却是 0——过载被无声吞掉了\n",
                  fps);
        }
    }
}

/*
 * 队满时入队必须**立刻**失败，不能等消费者。
 *
 * 这是 P1-D 的核心契约：xQueueSend(..., 0)。把超时改成非零，行为上的差别
 * 是 record_dispatch()（也就是 RX 任务）会睡在满队列上——正是这次要消灭的
 * 回压。host 桩不真的睡（真睡了测试会挂死，挂死跑不出退出码），而是把
 * "本该睡的那一次"记进 pk_host_queue_blocking_sends，这里读它。
 */
static void test_enqueue_never_waits_for_the_consumer(void)
{
    drain_quietly(1000000);
    const unsigned before = pk_host_queue_blocking_sends;

    (void)probe_queue_depth();           /* 灌到满 */
    record_t rec;
    for (int i = 0; i < 500; ++i) {      /* 满队列上继续灌 */
        make_record(&rec, 60000 + i, i);
        record_dispatch(&rec);
    }

    CHECK(pk_host_queue_blocking_sends == before,
          "满队列上出现了 %u 次「本该阻塞」的入队——入队超时不是 0，"
          "record_dispatch()（RX 任务）会睡在满队列上\n",
          pk_host_queue_blocking_sends - before);

    drain_quietly(1000000);
}

/*
 * drain 任务起不来：sink 必须彻底不可用，而不是"看起来活着但没人排空"。
 *
 * 真机上这条路径的典型触发原因就是内存紧张（P4 rev<v3 调度器启动前只有
 * 65 KB 内部堆），而此时如果 s_queue 还留着，record_sink_uart_stats() 会
 * 返回 true 并报出三个 0——对外就是"UART sink 活着，一条都没丢"。那是假话：
 * sink 根本没注册，永远不会有人入队，那三个 0 不代表任何事实。
 *
 * 队列本身有没有被 vQueueDelete 释放，这里测不到（host 上没有可靠的泄漏
 * 判据，macOS 的 ASan 默认不带 LeakSanitizer）。那一条由
 * firmware/scripts/test_rx_hot_path_logging.py 的结构守卫覆盖。
 */
static void test_task_create_failure_disables_the_sink(void)
{
    pk_host_task_create_fail = 1;
    record_sink_t *sink = record_sink_uart_create();
    pk_host_task_create_fail = 0;

    CHECK(sink == NULL,
          "drain 任务起不来却仍然返回了 sink——注册上去就没人排空，"
          "队列灌满后全部静默丢弃\n");

    uint32_t w = 0xDEAD, d = 0xDEAD, p = 0xDEAD;
    CHECK(record_sink_uart_stats(&w, &d, &p) == false,
          "sink 没起来，stats() 却返回 true——诊断页会显示「丢弃 0」，"
          "而事实是这一路压根不存在\n");
}

/* 队列满之后生产侧仍然只是入队失败，绝不阻塞，也不会破坏已有数据。 */
static void test_queue_full_drops_are_counted_not_blocking(void)
{
    drain_quietly(1000000);
    const uint32_t depth = probe_queue_depth();
    CHECK(depth > 0, "探测到的队列深度应大于 0，实得 %u\n", depth);

    uint32_t w0 = 0, d0 = 0, p0 = 0;
    record_sink_uart_stats(&w0, &d0, &p0);
    CHECK(p0 == depth, "队列此时应是满的 pending=%u depth=%u\n", p0, depth);

    /* 再灌 1000 条：全部应计入 dropped，在途保持不变（不覆盖已排队的数据）。 */
    record_t rec;
    for (int i = 0; i < 1000; ++i) {
        make_record(&rec, 40000 + i, i);
        record_dispatch(&rec);
    }
    uint32_t w1 = 0, d1 = 0, p1 = 0;
    record_sink_uart_stats(&w1, &d1, &p1);
    CHECK(d1 - d0 == 1000, "满队列下 1000 次 dispatch 应全部计入丢弃，实得 %u\n",
          d1 - d0);
    CHECK(p1 == depth, "丢弃不得改变在途条数 %u→%u\n", p0, p1);
    CHECK(w1 == w0, "丢弃路径不得产生任何打印 written %u→%u\n", w0, w1);

    drain_quietly(1000000);
}

/*
 * 多生产者安全。
 *
 * record_sink.h 写明 record_dispatch() 可由任意任务调用，实际也确有第二个
 * 生产者：pk_rec_selftest.c 在自检任务里调它。所以入队必须是多生产者安全的。
 *
 * 这条测试专门证伪"单生产者环形缓冲"那一版：两个线程各自读到同一个 head、
 * 各写同一个槽位、各把 head 加一，结果是记录被静默覆盖——written+dropped+
 * pending 会小于总调用次数，而且谁都不会报错。
 *
 * 边灌边排，让队列在满与不满之间反复穿越（只灌不排的话队列一直满，几乎所有
 * 调用都走"丢弃"这一条分支，入队的竞态窗口根本不会被压到）。
 */
#define PRODUCERS       4
#define PER_PRODUCER    5000

static atomic_int g_producers_done;

static void *producer_thread(void *arg)
{
    const int id = *(const int *)arg;
    record_t rec;
    for (int i = 0; i < PER_PRODUCER; ++i) {
        make_record(&rec, 50000 + i, id * 1000 + i);
        record_dispatch(&rec);
    }
    atomic_fetch_add(&g_producers_done, 1);
    return NULL;
}

static void test_multiple_producers_lose_nothing_silently(void)
{
    drain_quietly(1000000);
    uint32_t w0 = 0, d0 = 0, p0 = 0;
    record_sink_uart_stats(&w0, &d0, &p0);
    CHECK(p0 == 0, "前置条件：队列应为空 pending=%u\n", p0);

    pthread_t th[PRODUCERS];
    int ids[PRODUCERS];
    atomic_store(&g_producers_done, 0);

    for (int i = 0; i < PRODUCERS; ++i) {
        ids[i] = i;
        CHECK(pthread_create(&th[i], NULL, producer_thread, &ids[i]) == 0,
              "pthread_create 失败\n");
    }

    /* 主线程是唯一的消费者，边灌边排：队列因此在"满"与"不满"之间反复穿越，
     * 入队的竞态窗口才会被真正压到。只灌不排的话队列一直满，几乎所有调用都
     * 走"丢弃"那条分支，覆盖不到并发入队。 */
    while (atomic_load(&g_producers_done) < PRODUCERS) {
        drain_quietly(32);
    }
    for (int i = 0; i < PRODUCERS; ++i) pthread_join(th[i], NULL);

    drain_quietly(1000000);
    uint32_t w1 = 0, d1 = 0, p1 = 0;
    record_sink_uart_stats(&w1, &d1, &p1);

    const long total = (long)PRODUCERS * PER_PRODUCER;
    const long acct  = (long)(w1 - w0) + (long)(d1 - d0) + (long)p1;
    CHECK(acct == total,
          "%d 个生产者并发 dispatch：账目不平 written+dropped+pending=%ld "
          "总调用=%ld，差额 %ld 条被静默吞掉（入队非多生产者安全）\n",
          PRODUCERS, acct, total, total - acct);
    CHECK(p1 == 0, "全部排空后在途应为 0，实得 %u\n", p1);
}

int main(void)
{
    /* 先测失败路径，再建真的：create() 只能成功一次（内部 static 状态），
     * 顺序反过来就注入不进去了。 */
    test_task_create_failure_disables_the_sink();

    record_sink_t *uart = record_sink_uart_create();
    if (uart == NULL) {
        printf("test_record_sink_backpressure: uart sink create failed\n");
        return 1;
    }
    if (record_sink_register(uart) != 0) {
        printf("test_record_sink_backpressure: register failed\n");
        return 1;
    }

    test_dispatch_enqueues_instead_of_printing();
    test_drain_output_is_exact_and_in_order();
    test_sustained_overload_accounts_for_every_record();
    test_queue_full_drops_are_counted_not_blocking();
    test_enqueue_never_waits_for_the_consumer();
    test_multiple_producers_lose_nothing_silently();

    /* 兜底：以上任何一段里只要出现过一次"本该阻塞"的入队，都是回压回归。
     * 单独一条是因为各测试各自只看自己那一段的增量。 */
    CHECK(pk_host_queue_blocking_sends == 0,
          "整轮下来共有 %u 次入队本该阻塞——RX 热路径的非阻塞契约破了\n",
          pk_host_queue_blocking_sends);

    if (g_fail == 0) {
        printf("test_record_sink_backpressure: all OK\n");
        return 0;
    }
    printf("test_record_sink_backpressure: %d FAIL\n", g_fail);
    return 1;
}
