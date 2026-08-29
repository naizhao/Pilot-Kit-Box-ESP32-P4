#!/usr/bin/env python3
"""Interface_J3 页：2×20 排母直插微雪载板 J3（HAT 堆叠）。

引脚网络映射依据 PINMAP.md §1 + docs/hardware/board_pinout-zh_CN.md J3 全表：
上排(偶) 40=GND 38=GPIO52 36=GNSS_TXD 34=GNSS_PPS 32=GNSS_RXD 30=BOOT 28=IMU_INT
        26=GND 24=BARO_INT 22/20=GPIO30/29 18=ESP_3V3 16=IMU_RST 14/12=GPIO4/3
        10=GND 8=GPIO2 6=I2C_SCL 4=I2C_SDA 2=ESP_3V3
下排(奇) 39/37=GPIO48/47 35=ADSB_TXD 33=GND 31=ADSB_RXD 29=GND 27/25=USB 23/21=GPIO25/24
        19=GND 17/15=GPIO22/21 13=GND 11/9/7=GPIO5/38/37 5=GND 3/1=VCC_5V
未用脚一律 NC（物理直插过孔，电气不引出）。
"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from gen_sch import Sheet

s = Sheet("interface", "Expansion Board V1 - Interface: J3 2x20 HAT socket")

J3 = {}
# 电源与地
for p in ("1", "3"):
    J3[p] = "VCC_5V"
for p in ("5", "10", "13", "19", "26", "29", "33", "40"):
    J3[p] = "GND"
# 信号（PINMAP §1）
J3.update({
    "4": "I2C_SDA", "6": "I2C_SCL",
    "32": "GNSS_RXD", "36": "GNSS_TXD", "34": "GNSS_PPS",
    "28": "IMU_INT", "16": "IMU_RST", "24": "BARO_INT",
    "35": "ADSB_TXD", "31": "ADSB_RXD",
})
# 其余全部 NC
for n in range(1, 41):
    J3.setdefault(str(n), "NC")
assert len(J3) == 40
assert sum(1 for v in J3.values() if v == "VCC_5V") == 2
assert sum(1 for v in J3.values() if v == "GND") == 8

s.place("J1", "Connector_Generic", "Conn_02x20_Odd_Even", 101.6, 88.9, J3,
        value="J3_HAT_2x20_SMD排针",
        footprint="Connector_PinHeader_2.54mm:PinHeader_2x20_P2.54mm_Vertical_SMD")

s.write()
