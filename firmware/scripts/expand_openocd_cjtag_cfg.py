#!/usr/bin/env python3
"""把 OpenOCD 的 ti_cjtag_to_4pin_jtag 展开成 TMS 位序列与每次扫描的时钟数。

为什么需要它
------------
TI 的 cJTAG 命令是用「一次 DR 扫描在 Shift-DR 里停了几个时钟」编码的
(TRM SWCU185G §6.2.1)，而 2 线模式下器件**对命令窗不做任何应答**——上板时
没有任何办法二分定位，序列错在哪一步是看不出来的。

OpenOCD tcl/target/ti/cjtag.cfg 里那段 ti_cjtag_to_4pin_jtag 是社区实测能用
的参考实现(在真 CC26xx 上把器件切到 4 线 JTAG)。它全部由 pathmove 的状态列表
构成，可以确定性地展开成 TMS 位，再按 Update-DR 结算出每次扫描的时钟数。

把那串数钉进 test_cjtag.c，就得到了唯一一个**不上板也能判定"我们的命令窗
序列本身对不对"**的判据。2026-09-11 实测两边一致：[0, 0, 1, 2, 9]
= 两次 ZBS + 锁 control level 的 1 位扫描 + CP0=2(STC2) + CP1=9(APFC=01)。

用法
----
    curl -sL -o /tmp/ti_cjtag.cfg \
      https://raw.githubusercontent.com/openocd-org/openocd/master/tcl/target/ti/cjtag.cfg
    python3 firmware/scripts/expand_openocd_cjtag_cfg.py /tmp/ti_cjtag.cfg
"""
import sys
# 标准 1149.1 转移表：state -> (tms=0 目标, tms=1 目标)
NXT = {
 "RESET":("RUN/IDLE","RESET"), "RUN/IDLE":("RUN/IDLE","DRSELECT"),
 "DRSELECT":("DRCAPTURE","IRSELECT"), "DRCAPTURE":("DRSHIFT","DREXIT1"),
 "DRSHIFT":("DRSHIFT","DREXIT1"), "DREXIT1":("DRPAUSE","DRUPDATE"),
 "DRPAUSE":("DRPAUSE","DREXIT2"), "DREXIT2":("DRSHIFT","DRUPDATE"),
 "DRUPDATE":("RUN/IDLE","DRSELECT"),
 "IRSELECT":("IRCAPTURE","RESET"), "IRCAPTURE":("IRSHIFT","IREXIT1"),
 "IRSHIFT":("IRSHIFT","IREXIT1"), "IREXIT1":("IRPAUSE","IRUPDATE"),
 "IRPAUSE":("IRPAUSE","IREXIT2"), "IREXIT2":("IRSHIFT","IRUPDATE"),
 "IRUPDATE":("RUN/IDLE","DRSELECT"),
}
def tms_for(a,b):
    z,o = NXT[a]
    if b==z: return 0
    if b==o: return 1
    raise SystemExit("非法相邻: %s -> %s" % (a,b))

cfg = open(sys.argv[1] if len(sys.argv) > 1 else "/tmp/ti_cjtag.cfg").read()
body = cfg[cfg.index("proc ti_cjtag_to_4pin_jtag"):]
bits=[]; states=[]
for line in body.splitlines():
    line=line.split("#")[0].strip()
    if not line.startswith("pathmove"): continue
    seq=line.split()[1:]
    for a,b in zip(seq, seq[1:]):
        bits.append(tms_for(a,b)); states.append((a,b))

# 按 DRUPDATE 分段，数每段在 DRSHIFT 里待了几个时钟
counts=[]; cur=0
for (a,b) in states:
    if a=="DRSHIFT": cur+=1          # 从 DRSHIFT 发出的每一拍都算"停在 Shift-DR"
    if b=="DRUPDATE":
        counts.append(cur); cur=0
print("OpenOCD ti_cjtag_to_4pin_jtag（只看 pathmove 部分）")
print("  TMS 位数:", len(bits))
print("  TMS 序列:", "".join(map(str,bits)))
print("  各次 DR 扫描在 Shift-DR 里的时钟数（按 Update 结算）:", counts)
