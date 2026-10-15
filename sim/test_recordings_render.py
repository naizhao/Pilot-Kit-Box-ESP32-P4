"""Pixel checks against the firmware's real recordings renderer."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


class RecordingsRenderTest(unittest.TestCase):
    def render(self, **settings):
        executable = Path(os.environ.get(
            "PK_SIM_EXECUTABLE",
            str(Path(__file__).resolve().parent / "build/pkbox_sim")))
        with tempfile.TemporaryDirectory(prefix="pk-audio-render-") as directory:
            shot = Path(directory) / "page.bmp"
            environment = dict(os.environ, PK_SIM_PAGE="recordings", PK_SIM_LANG="en",
                               PK_SIM_RECORDINGS_SELECT="1",
                               PK_SIM_RECORDINGS_INVALID="0")
            environment.update(settings)
            subprocess.run([str(executable), "--shot", "0", str(shot)],
                           env=environment, check=True, capture_output=True)
            image = shot.read_bytes()
        offset = struct.unpack_from("<I", image, 10)[0]
        width, height = struct.unpack_from("<ii", image, 18)
        bits = struct.unpack_from("<H", image, 28)[0]
        self.assertEqual((width, abs(height), bits), (800, 480, 24))
        stride = (width * 3 + 3) & ~3

        def pixel(x, y):
            row = height - 1 - y if height > 0 else y
            start = offset + row * stride + x * 3
            return image[start:start + 3]
        return pixel

    def test_partial_rows_do_not_paint_header_or_footer(self):
        image = self.render(PK_SIM_RECORDINGS_SCROLL="30")
        background = image(799, 479)
        self.assertEqual(image(600, 65), background)
        for y in (382, 390, 397, 410, 435):
            self.assertEqual(image(600, y), background, f"row leaked at y={y}")

    def test_each_enabled_control_has_pressed_background(self):
        normal = self.render()
        for x in (52, 138, 224, 314, 400, 486):
            with self.subTest(x=x):
                pressed = self.render(PK_SIM_RECORDINGS_PRESS=f"{x},430")
                self.assertNotEqual(pressed(x, 412), normal(x, 412))

    def test_volume_percentage_is_not_permanently_above_buttons(self):
        image = self.render()
        background = image(799, 479)
        for y in range(382, 398):
            for x in range(102, 174):
                self.assertEqual(image(x, y), background,
                                 "volume should appear only in the click popup")

    def test_repeated_idle_release_does_not_clear_quick_tap_feedback(self):
        held = self.render(PK_SIM_RECORDINGS_PRESS="224,430")
        released = self.render(PK_SIM_RECORDINGS_PRESS="224,430",
                               PK_SIM_RECORDINGS_RELEASES="2")
        self.assertEqual(released(224, 412), held(224, 412))

    def test_gap_metadata_does_not_change_recordings_row_pixels(self):
        normal = self.render(PK_SIM_RECORDINGS_GAP="0")
        interrupted = self.render(PK_SIM_RECORDINGS_GAP="1")
        for y in range(80, 382):
            for x in range(16, 784):
                self.assertEqual(interrupted(x, y), normal(x, y),
                                 f"GAP changed row pixel at {x},{y}")

    def test_volume_release_shows_popup_and_updated_value(self):
        normal = self.render()
        up = self.render(PK_SIM_RECORDINGS_VOLUME_TAPS="1")
        # AIRBAND's popup background is RGB(18,26,44), rounded to RGB565.
        self.assertEqual(up(400, 191), bytes((41, 24, 16)))
        self.assertNotEqual(up(400, 191), normal(400, 191))
        # Both user paths reach 40%; stale pre-click values (35/45) differ.
        down = self.render(PK_SIM_RECORDINGS_VOLUME="45",
                           PK_SIM_RECORDINGS_VOLUME_TAPS="-1")
        reference = self.render(PK_SIM_PAGE="airband", PK_SIM_AIRBAND_PRESS="730,440",
                                PK_SIM_AIRBAND_RELEASES="1")
        for y in range(205, 246):
            for x in range(310, 490):
                self.assertEqual(up(x, y), down(x, y))
                # Existing page text palettes differ by at most 9 RGB levels;
                # a wrong glyph/value changes foreground into background.
                self.assertLessEqual(max(abs(a - b) for a, b in
                                         zip(up(x, y), reference(x, y))), 9,
                                     "playback popup must show AIRBAND's same 40% value")

    def test_volume_popup_expires_and_each_tap_restarts_timeout(self):
        visible = self.render(PK_SIM_RECORDINGS_VOLUME_TAPS="2",
                              PK_SIM_RECORDINGS_TAP_INTERVAL_MS="1000",
                              PK_SIM_RECORDINGS_DELAY_MS="1199")
        expired = self.render(PK_SIM_RECORDINGS_VOLUME_TAPS="2",
                              PK_SIM_RECORDINGS_TAP_INTERVAL_MS="1000",
                              PK_SIM_RECORDINGS_DELAY_MS="1200")
        self.assertEqual(visible(400, 191), bytes((41, 24, 16)))
        self.assertNotEqual(expired(400, 191), visible(400, 191))

    def test_disabled_volume_and_page_reopen_do_not_show_popup(self):
        for settings in (
            {"PK_SIM_RECORDINGS_VOLUME": "100", "PK_SIM_RECORDINGS_VOLUME_TAPS": "1"},
            {"PK_SIM_RECORDINGS_VOLUME": "0", "PK_SIM_RECORDINGS_VOLUME_TAPS": "-1"},
            {"PK_SIM_RECORDINGS_VOLUME_TAPS": "1", "PK_SIM_RECORDINGS_REOPEN": "1"},
        ):
            with self.subTest(settings=settings):
                image = self.render(**settings)
                self.assertNotEqual(image(400, 191), bytes((41, 24, 16)))

    def test_volume_popup_timeout_survives_tick_wrap(self):
        visible = self.render(PK_SIM_UPTIME_S="4294967",
                              PK_SIM_RECORDINGS_VOLUME_TAPS="2",
                              PK_SIM_RECORDINGS_TAP_INTERVAL_MS="1000",
                              PK_SIM_RECORDINGS_DELAY_MS="1199")
        expired = self.render(PK_SIM_UPTIME_S="4294967",
                              PK_SIM_RECORDINGS_VOLUME_TAPS="2",
                              PK_SIM_RECORDINGS_TAP_INTERVAL_MS="1000",
                              PK_SIM_RECORDINGS_DELAY_MS="1200")
        self.assertEqual(visible(400, 191), bytes((41, 24, 16)))
        self.assertNotEqual(expired(400, 191), visible(400, 191))


if __name__ == "__main__":
    unittest.main()
