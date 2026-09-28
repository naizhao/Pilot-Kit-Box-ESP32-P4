#!/usr/bin/env python3
"""AIRBAND BLE 控制面的加密访问合同。

这个测试刻意检查实际注册给 NimBLE 的 characteristic 和 Security Manager
配置，而不是只搜一个散落在注释里的关键词。生产代码若退回以下任一缺陷，测试
必须变红：

* ``...0006`` 不再带 ``WRITE_ENC``，明文客户端可直接 MUTE 本机；
* Security Manager 不再建立加密/绑定所需的密钥；
* 链路失去加密后仍保留 PHONE/MUTE 路由。
"""

from __future__ import annotations

import re
import os
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get(
    "PK_BLE_GATT_SOURCE", ROOT / "firmware" / "main" / "ble_gatt.c"
))


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def initializer_entry(source: str, uuid_symbol: str) -> str:
    match = re.search(
        rf"\{{\s*\.uuid\s*=\s*&{re.escape(uuid_symbol)}\.u\s*,(.*?)"
        r"\.val_handle\s*=\s*&[^,}]+[,\s]*\}",
        source,
        flags=re.S,
    )
    assert match, f"没有找到 {uuid_symbol} 的 GATT characteristic 定义"
    return match.group(0)


def event_case(source: str, event_name: str) -> str:
    match = re.search(
        rf"case\s+{re.escape(event_name)}\s*:(.*?)(?=\n\s*case\s+BLE_GAP_EVENT_|\n\s*default\s*:)",
        source,
        flags=re.S,
    )
    assert match, f"没有找到 {event_name} 处理分支"
    return match.group(1)


def main() -> int:
    source = strip_comments(SOURCE.read_text(encoding="utf-8"))

    control = initializer_entry(source, "s_chr_audio_ctl_uuid")
    assert "BLE_GATT_CHR_F_WRITE" in control, "AIRBAND Control 必须保持可写"
    assert "BLE_GATT_CHR_F_WRITE_ENC" in control, (
        "AIRBAND Control 写入必须要求链路加密；否则未配对客户端能切换 PHONE/MUTE"
    )

    callback = re.search(
        r"static int chr_audio_ctl_access_cb\([^;]*?\)\s*\{.*?\n\}",
        source,
        flags=re.S,
    )
    assert callback, "没有找到 AIRBAND Control 回调"
    callback_text = callback.group(0)
    assert "owner_authorize_control" in callback_text, (
        "加密不等于机主身份：控制回调必须执行 owner / 本机授权窗口判定"
    )

    required_sm = {
        "ble_hs_cfg.sm_io_cap": "BLE_SM_IO_CAP_NO_IO",
        "ble_hs_cfg.sm_bonding": "1",
        "ble_hs_cfg.sm_sc": "1",
        "ble_hs_cfg.sm_mitm": "0",
    }
    for lhs, rhs in required_sm.items():
        assert re.search(rf"{re.escape(lhs)}\s*=\s*{re.escape(rhs)}\s*;", source), (
            f"缺少 Security Manager 配置：{lhs} = {rhs}"
        )
    for side in ("sm_our_key_dist", "sm_their_key_dist"):
        assert re.search(
            rf"ble_hs_cfg\.{side}\s*=\s*"
            r"BLE_SM_PAIR_KEY_DIST_ENC\s*\|\s*BLE_SM_PAIR_KEY_DIST_ID\s*;",
            source,
        ), f"{side} 必须分发加密密钥与身份密钥"
    assert "ble_store_config_init();" in source, "bonding 前必须安装 NimBLE key store callbacks"

    enc_change = event_case(source, "BLE_GAP_EVENT_ENC_CHANGE")
    assert "event->enc_change.status" in enc_change, "加密变化处理必须检查协商状态"
    assert "audio_route_apply(PK_AIRBAND_BLE_ROUTE_DEVICE)" in enc_change, (
        "加密失败或丢失后必须撤销 PHONE/MUTE，恢复本机播放"
    )

    repeat = event_case(source, "BLE_GAP_EVENT_REPEAT_PAIRING")
    assert "ble_store_util_delete_peer" in repeat, "手机遗忘旧 bond 后必须清掉设备侧旧密钥"
    assert "BLE_GAP_REPEAT_PAIRING_RETRY" in repeat, "清掉旧 bond 后必须让 NimBLE 重试配对"

    print("BLE AIRBAND security contract: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
