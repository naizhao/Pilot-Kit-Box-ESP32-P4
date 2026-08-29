"""由 gen_ifa_footprint.py 自动生成，请勿手改。改天线跑那个脚本，这里会同步。"""
KEEPOUT_LOCAL = (-7.7380, -7.6730, 49.7620, 0.0120)
BBOX_LOCAL = (-5.7380, -6.7380, 47.7620, 0.7620)
ARM_SPAN = 52.0000   # 主臂中心线跨度 = 谐振长度
LEG_END_LOCAL = 0.0120   # 脚末端 y（= 净空下沿）
FEED_LOCAL = (-0.0000, 0.0000)
FEED_CHANNEL_WIDTH = 2.4000   # F.Cu无铜通道；容纳1.5mm taper及两侧各0.45mm间隙
FEED_CHANNEL_DEPTH = 2.2500   # 中心线端点→taper窄端：0.75mm等宽段+1.5mm taper
RF_WIDTH = 0.1500   # JLC06161H-3313六层50Ω微带线宽
FEED_LEG_END = (0.0120, 0.0120)   # 6.0mm馈电脚中心线端点；到铜箔外沿还有0.75mm等宽段
FEED_LEG_COPPER_END = (0.0120, 0.7620)   # 馈电脚真实铜箔外沿 / taper宽端
FEED_TAPER_START = (0.0120, 0.7620)   # 完整1.5mm taper宽端
FEED_TAPER_END = (0.0120, 2.2620)   # taper窄端；外部0.15mm微带从这里接出
TAPER_LENGTH = 1.5000
