#!/usr/bin/env python3
"""pk_logd — 盒子串口日志守护进程。

为什么需要它
------------
硬件调试的常态是：拔线、关机、上烙铁、换板子，还经常有好几个进程同时抢同一个
串口。靠"临时开个窗口 tail 一下"必然丢数据——飞机从头顶飞过那一刻的日志没留，
现象就只能靠回忆复述。而一旦要烧录，抓日志的进程还会把串口占住让 esptool 失败
（而且 esptool 会照样返回 0，只有对着 uptime 才看得出没烧进去）。

所以这个守护进程的三条硬要求：
  1. **持久化** —— 日志落盘，按天分文件，板子关机/拔线都不影响已写入的内容；
  2. **有开关** —— start/stop/pause/resume，烧录前一条命令让出串口；
  3. **不抢端口** —— 打不开就安静重试，绝不因为别人占着而退出或刷屏。

用法
----
    pk_logd.py start            # 启动（后台）
    pk_logd.py status           # 在不在跑、抓到哪些口、各文件多大
    pk_logd.py pause [--secs N] # 让出串口给 esptool，N 秒后自动恢复（默认 180）
    pk_logd.py resume
    pk_logd.py stop
    pk_logd.py tail [-n 50] [--role p4|rp]
    pk_logd.py grep "RP health" [--role p4] [-n 20]
    pk_logd.py flash -- <命令…>  # 自动 pause → 跑命令 → resume

pause 一定带超时自动恢复：忘了 resume 的代价是"以为在记其实没记"，比多记一段
无用日志糟得多。
"""
import argparse
import glob
import os
import signal
import subprocess
import sys
import termios
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LOG_DIR = os.environ.get("PK_LOG_DIR", os.path.join(ROOT, ".pk_logs"))
PID_FILE = os.path.join(LOG_DIR, "daemon.pid")
PAUSE_FILE = os.path.join(LOG_DIR, "paused")      # 内容 = 自动恢复的时间戳
CH340_HINT = "5B90"        # P4 侧 USB-UART 的序列号片段；其余 usbmodem 视作 RP2040


# ── 串口 ────────────────────────────────────────────────────────────────
def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = a[1] = a[3] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    # 115200 固定。**绝不能用 1200**——那是 pico-sdk 的 BOOTSEL 触发条件，
    # 一个抓日志的工具把板子弹进 BOOTSEL 是灾难性的副作用。
    a[4] = a[5] = termios.B115200
    a[6] = list(a[6])
    a[6][termios.VMIN] = 0
    a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


def role_of(path):
    return "p4" if CH340_HINT in path else "rp"


def log_path(role):
    return os.path.join(LOG_DIR, "%s-%s.log" % (role, time.strftime("%Y%m%d")))


# ── 守护进程主体 ─────────────────────────────────────────────────────────
def paused_until():
    """返回自动恢复的时间戳；未暂停返回 0。"""
    try:
        with open(PAUSE_FILE) as f:
            return float(f.read().strip() or 0)
    except (OSError, ValueError):
        return 0.0


def run_daemon():
    os.makedirs(LOG_DIR, exist_ok=True)
    with open(PID_FILE, "w") as f:
        f.write(str(os.getpid()))

    fds = {}          # path -> [fd, role, 未完成的半行]
    was_paused = False
    running = [True]

    def on_term(_sig, _frm):
        running[0] = False
    signal.signal(signal.SIGTERM, on_term)
    signal.signal(signal.SIGINT, on_term)

    def write(role, text):
        with open(log_path(role), "a", encoding="utf-8", errors="replace") as f:
            f.write(text)

    def close_all(why):
        for path, (fd, role, _) in list(fds.items()):
            try:
                os.close(fd)
            except OSError:
                pass
            write(role, "%s --- 断开(%s): %s ---\n"
                  % (time.strftime("%H:%M:%S"), why, path))
        fds.clear()

    while running[0]:
        # 暂停：关掉串口让别人用（烧录）。到点自动恢复——忘了 resume 的代价是
        # "以为在记其实没记"，比多记一段无用日志糟得多。
        until = paused_until()
        if until and time.time() < until:
            if not was_paused:
                close_all("pause")
                was_paused = True
            time.sleep(0.2)
            continue
        if was_paused:
            was_paused = False
            try:
                os.unlink(PAUSE_FILE)
            except OSError:
                pass
        elif until:                      # 超时了，自动清掉
            try:
                os.unlink(PAUSE_FILE)
            except OSError:
                pass

        # 热插拔：掉线的关掉，新出现的打开。打不开就安静跳过（多半是别的进程
        # 正占着），下一轮再试——绝不退出、不刷屏。
        present = set(glob.glob("/dev/cu.usbmodem*"))
        for path in list(fds):
            if path not in present:
                fd, role, _ = fds.pop(path)
                try:
                    os.close(fd)
                except OSError:
                    pass
                write(role, "%s --- 掉线: %s ---\n"
                      % (time.strftime("%H:%M:%S"), path))
        for path in present - set(fds):
            try:
                fd = open_port(path)
            except OSError:
                continue
            role = role_of(path)
            fds[path] = [fd, role, ""]
            write(role, "%s === 接入: %s ===\n"
                  % (time.strftime("%Y-%m-%d %H:%M:%S"), path))

        got = False
        for path, st in list(fds.items()):
            fd, role, pend = st
            try:
                chunk = os.read(fd, 8192)
            except BlockingIOError:
                continue
            except OSError:
                continue
            if not chunk:
                continue
            got = True
            pend += chunk.decode("utf-8", "replace")
            *lines, pend = pend.split("\n")
            st[2] = pend
            ts = time.strftime("%H:%M:%S")
            out = "".join("%s %s\n" % (ts, ln.rstrip("\r"))
                          for ln in lines if ln.strip())
            if out:
                write(role, out)
        time.sleep(0.005 if got else 0.05)

    close_all("stop")
    try:
        os.unlink(PID_FILE)
    except OSError:
        pass


# ── 控制命令 ─────────────────────────────────────────────────────────────
def daemon_pid():
    try:
        with open(PID_FILE) as f:
            pid = int(f.read().strip())
    except (OSError, ValueError):
        return 0
    try:
        os.kill(pid, 0)
        return pid
    except OSError:
        return 0            # pid 文件是陈旧的


def cmd_start(_a):
    if daemon_pid():
        print("已经在跑了 (pid %d)" % daemon_pid())
        return 0
    os.makedirs(LOG_DIR, exist_ok=True)
    pid = os.fork()
    if pid == 0:
        os.setsid()
        with open(os.devnull, "rb") as z, \
             open(os.path.join(LOG_DIR, "daemon.out"), "a") as o:
            os.dup2(z.fileno(), 0)
            os.dup2(o.fileno(), 1)
            os.dup2(o.fileno(), 2)
        run_daemon()
        os._exit(0)
    time.sleep(0.4)
    print("已启动 (pid %d)，日志目录 %s" % (daemon_pid(), LOG_DIR))
    return 0


def cmd_stop(_a):
    pid = daemon_pid()
    if not pid:
        print("没在跑")
        return 0
    os.kill(pid, signal.SIGTERM)
    for _ in range(40):
        if not daemon_pid():
            print("已停止")
            return 0
        time.sleep(0.1)
    print("停不下来，pid %d 还在" % pid)
    return 1


def cmd_pause(a):
    os.makedirs(LOG_DIR, exist_ok=True)
    with open(PAUSE_FILE, "w") as f:
        f.write(str(time.time() + a.secs))
    time.sleep(0.5)                      # 等守护进程真的把串口放掉
    print("已暂停 %d 秒（到点自动恢复）" % a.secs)
    return 0


def cmd_resume(_a):
    try:
        os.unlink(PAUSE_FILE)
    except OSError:
        pass
    print("已恢复")
    return 0


def cmd_status(_a):
    pid = daemon_pid()
    print("守护进程 : %s" % ("运行中 pid %d" % pid if pid else "未运行"))
    until = paused_until()
    if until:
        left = until - time.time()
        print("状态     : 暂停中，%s" % ("%.0f 秒后自动恢复" % left if left > 0
                                          else "已超时，下一轮自动恢复"))
    else:
        print("状态     : 抓取中" if pid else "状态     : —")
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    print("在线串口 : %s" % (", ".join("%s(%s)" % (p, role_of(p))
                                       for p in ports) or "无"))
    for f in sorted(glob.glob(os.path.join(LOG_DIR, "*.log"))):
        st = os.stat(f)
        print("  %-28s %8.1f KB  最后写入 %s"
              % (os.path.basename(f), st.st_size / 1024.0,
                 time.strftime("%m-%d %H:%M:%S", time.localtime(st.st_mtime))))
    return 0


def _files(role):
    pat = "%s-*.log" % role if role else "*.log"
    return sorted(glob.glob(os.path.join(LOG_DIR, pat)))


def cmd_tail(a):
    fs = _files(a.role)
    if not fs:
        print("还没有日志文件")
        return 1
    subprocess.run(["tail", "-n", str(a.n), fs[-1]])
    return 0


def cmd_grep(a):
    fs = _files(a.role)
    if not fs:
        print("还没有日志文件")
        return 1
    p = subprocess.run(["grep", "-h", a.pattern] + fs,
                       capture_output=True, text=True)
    lines = p.stdout.splitlines()
    for ln in lines[-a.n:]:
        print(ln)
    print("--- 命中 %d 行，显示最后 %d 行 ---" % (len(lines), min(a.n, len(lines))))
    return 0


def cmd_flash(a):
    """自动 pause → 跑命令 → resume。烧录必须走这个，否则 esptool 拿不到独占
    而**照样返回 0**——只有对着 uptime 才看得出根本没烧进去。"""
    argv = list(a.argv)
    if argv and argv[0] == "--":      # argparse 不会替我们剥掉它
        argv = argv[1:]
    if not argv:
        print("用法: pk_logd.py flash -- <命令…>")
        return 2
    had = daemon_pid()
    if had:
        cmd_pause(argparse.Namespace(secs=a.secs))
    try:
        return subprocess.run(argv).returncode
    finally:
        if had:
            cmd_resume(None)


def main():
    ap = argparse.ArgumentParser(description="盒子串口日志守护")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("start").set_defaults(fn=cmd_start)
    sub.add_parser("stop").set_defaults(fn=cmd_stop)
    sub.add_parser("status").set_defaults(fn=cmd_status)
    sub.add_parser("resume").set_defaults(fn=cmd_resume)
    sub.add_parser("_run").set_defaults(fn=lambda _a: run_daemon())

    p = sub.add_parser("pause"); p.add_argument("--secs", type=int, default=180)
    p.set_defaults(fn=cmd_pause)

    p = sub.add_parser("tail")
    p.add_argument("-n", type=int, default=50)
    p.add_argument("--role", choices=["p4", "rp"])
    p.set_defaults(fn=cmd_tail)

    p = sub.add_parser("grep")
    p.add_argument("pattern")
    p.add_argument("-n", type=int, default=20)
    p.add_argument("--role", choices=["p4", "rp"])
    p.set_defaults(fn=cmd_grep)

    p = sub.add_parser("flash")
    p.add_argument("--secs", type=int, default=300)
    # dest 不能叫 cmd：会和 add_subparsers(dest="cmd") 撞名，
    # 子命令名被 REMAINDER 覆盖掉，"--" 就剥不掉了。
    p.add_argument("argv", nargs=argparse.REMAINDER)
    p.set_defaults(fn=cmd_flash)

    a = ap.parse_args()
    sys.exit(a.fn(a))


if __name__ == "__main__":
    main()
