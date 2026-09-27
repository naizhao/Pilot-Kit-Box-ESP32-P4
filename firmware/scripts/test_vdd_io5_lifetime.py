#!/usr/bin/env python3
"""VDD_IO_5 生命周期合同：SD 热插拔不得切断 AIRBAND 的 GPIO47/48。"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def code_only(path: Path) -> str:
    text = path.read_text(encoding="utf-8")
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//.*", "", text)


class VddIo5LifetimeContract(unittest.TestCase):
    def test_sd_card_power_is_separate_from_shared_io_rail(self) -> None:
        sd = code_only(ROOT / "main" / "pk_sdcard.c")
        self.assertRegex(sd, r"#define\s+SD_PIN_PWR_EN\s+45\b")
        self.assertIn("gpio_set_level(SD_PIN_PWR_EN, 0)", sd)
        self.assertIn("gpio_set_level(SD_PIN_PWR_EN, 1)", sd)
        self.assertNotIn("sd_pwr_ctrl_del_on_chip_ldo", sd)

    def test_airband_consumes_gpio47_and_gpio48_from_that_rail(self) -> None:
        airband = code_only(ROOT / "main" / "airband_task.c")
        self.assertRegex(airband, r"VHF_SDATA_GPIO\s+GPIO_NUM_47\b")
        self.assertRegex(airband, r"VHF_DOUT_GPIO\s+GPIO_NUM_48\b")

    def test_sdmmc_shares_host_with_the_verified_sdio_transport(self) -> None:
        sdkconfig = (ROOT / "sdkconfig").read_text(encoding="utf-8")
        self.assertRegex(sdkconfig, r"(?m)^CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6=y$")
        self.assertRegex(sdkconfig, r"(?m)^CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE=y$")
        self.assertRegex(sdkconfig, r"(?m)^CONFIG_ESP_HOSTED_SDIO_SLOT_1=y$")
        self.assertNotRegex(sdkconfig, r"(?m)^CONFIG_ESP_HOSTED_SPI_HOST_INTERFACE=y$")

        sd = code_only(ROOT / "main" / "pk_sdcard.c")
        self.assertIn("SDMMC_HOST_DEFAULT()", sd)
        self.assertIn("CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE", sd)
        self.assertIn("sdmmc_host_init_dummy", sd)
        self.assertIn("sdmmc_host_deinit_dummy", sd)
        self.assertIn("sdmmc_host_deinit_slot", sd)


if __name__ == "__main__":
    unittest.main()
