/*
 * power.c — Legacy 2.4-inch carrier soft power-off helper.
 *
 * The current Rev1.2 4.3-inch target uses its board-level Key3 POWER and
 * does not start the former MODE-button task, so pk_power_enter_sleep()
 * currently has no caller in this build (button_task.c is excluded from
 * the CMake sources). The sequence below documents the retained legacy
 * helper only.
 *
 * Pin mapping caveat: everything below was measured on the 2.4-inch SPI
 * carrier, where the backlight PWM was GPIO50 and the panel was an
 * ST7789. On Rev1.2 4.3-inch neither number applies — backlight PWM is
 * GPIO26 into the AP3032 feedback node, LCD reset is GPIO27, the panel
 * is an ST7701 IPS over MIPI-DSI, and GPIO50 is the GPS PPS input. Do
 * not copy the GPIO50 references below into 4.3-inch code.
 *
 * Sequence on entry:
 *
 *   1. Log "power off requested" so the line leaves the UART before
 *      clocks stop (see step 5 for the drain).
 *   2. pk_display_set_brightness(0) — LEDC duty 0 drives the backlight
 *      PWM pin to a continuous low level (the LEDC channel itself stays
 *      configured, but its output is 0).
 *   3. Do not send the panel's DISPOFF command (ST7789 on the 2.4-inch
 *      carrier this helper was written for). Testing
 *      showed that calling esp_lcd_panel_disp_on_off() before sleep can
 *      leave the LCD/SPI peripheral in a state that prevents deep sleep
 *      from being entered. We blank the user-visible output by setting
 *      the backlight duty to 0 and accept the last framebuffer content.
 *   4. Wait for the user to release MODE. The wake source we arm in
 *      step 6 is "GPIO5 low," and the same physical button that
 *      triggered LONG_PRESS is still being held — its line is at 0.
 *      Without a release-wait, esp_deep_sleep_start would enter sleep
 *      and immediately self-wake in microseconds. Poll at 20 ms
 *      intervals; release confirmed after 5 consecutive high reads
 *      (≥100 ms) to debounce mechanical bounce.
 *   5. vTaskDelay 50 ms — UART drain at 115200 baud so the "entering
 *      deep sleep" log line lands before clocks stop.
 *   6. esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown: ESP32-P4's
 *      "wake on GPIO going low during deep sleep" API. IDF configures
 *      the internal pull-up automatically inside esp_deep_sleep_start
 *      (see ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS in esp_sleep.h).
 *      An earlier attempt with esp_sleep_enable_ext1_wakeup_io +
 *      rtc_gpio_pullup_en didn't actually engage the LP-domain pull-up
 *      on P4 — the pad floated, ext1 ANY_LOW saw "low", chip self-woke
 *      in microseconds.
 *   7. esp_deep_sleep_start() — noreturn. Next thing that happens is a
 *      cold boot back into app_main(), exactly as if the user had
 *      power-cycled the board.
 *
 * Silicon limitation: P4 rev < 3.0 (this board reports rev 1.3) does
 * not support per-pin hold during deep sleep (errata DIG-399). We can't
 * pin the backlight PWM pad (GPIO50 on the 2.4-inch carrier; GPIO26 on
 * Rev1.2 4.3-inch) low through the sleep transition — the LED may
 * continue to draw a few mA of bias current even while the panel shows
 * a fully dark display. A previous attempt to gpio_set_direction that
 * pad to plain output + gpio_hold_en before sleeping made things
 * worse: switching the pad out of the LEDC mux on this chip rev left
 * the LCD/LEDC peripheral in a state that blocked the deep sleep
 * transition entirely (chip stayed awake). So we leave the backlight
 * pad alone and accept the residual backlight bias as a
 * cosmetic+power-draw trade-off until rev ≥ 3.0 hardware is in play.
 *
 * The button task's "no long sleeps in the callback" rule (see
 * button_task.h) is deliberately bent in steps 4 + 5. Once we call
 * esp_deep_sleep_start(), the OS scheduler stops and every task is
 * torn down by the clock cut — no other task whose polling could
 * starve while button-task spins waiting for release + drain.
 */

#include "power.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_log.h"

#include "display.h"

static const char *TAG = "power";

/* MODE button GPIO on the legacy 2.4-inch carrier. Must stay in sync
 * with PK_BTN_MODE.gpio in button_task.c — both have to point at the
 * same LP_IO pin so the same physical button that triggered sleep also
 * wakes us up. On Rev1.2 4.3-inch, GPIO5 is only a plain J3-11
 * expansion pin with no button on it; power-off there is Key3 POWER. */
#define PK_POWER_WAKE_GPIO   5

void pk_power_enter_sleep(void)
{
    ESP_LOGW(TAG, "power off requested — backlight off, waiting for MODE (GPIO%d) release",
             PK_POWER_WAKE_GPIO);

    pk_display_set_brightness(0);

    /* Note: a previous version called pk_display_panel_off() here to
     * send the panel DISPOFF command (so the panel goes uniformly dark
     * instead of freezing on the last frame). Observed side effect: the chip then
     * fails to enter deep sleep at all — esp_lcd_panel_disp_on_off seems
     * to leave the LCD/SPI peripheral in a state that blocks the sleep
     * transition on this hardware. Until we figure out a clean way to
     * blank the panel without that side effect, we let the last frame
     * stay on screen and rely on the dim (LEDC duty=0) backlight. */

    /* Step 3: spin until MODE has been released for ≥100 ms. */
    int high_streak = 0;
    while (high_streak < 5) {
        vTaskDelay(pdMS_TO_TICKS(20));
        if (gpio_get_level(PK_POWER_WAKE_GPIO) == 1) {
            high_streak++;
        } else {
            high_streak = 0;
        }
    }

    ESP_LOGW(TAG, "MODE released — entering deep sleep (wake on MODE low)");

    /* Note: we intentionally do NOT try to hold the backlight PWM pad
     * (GPIO50 on the 2.4-inch carrier this helper was written for;
     * GPIO26 on Rev1.2 4.3-inch) low across deep sleep. ESP32-P4
     * silicon rev < 3.0 (this board reports rev 1.3) does not support
     * per-pin hold during deep sleep (errata DIG-399) — gpio_hold_en is
     * a no-op for the deep-sleep transition. Worse, calling
     * gpio_set_direction here to "force the pad low first" switches the
     * pad from the LEDC mux to a plain digital GPIO mux, which on this
     * chip rev appears to leave the LCD/LEDC peripheral in a state that
     * blocks the deep sleep transition entirely (chip never actually
     * sleeps).
     *
     * Because pk_display_panel_off() is intentionally not called, the
     * last framebuffer stays in the panel's RAM. LEDC duty=0 removes
     * the active backlight, and the 4.3-inch ST7701 IPS panel is not
     * readable without it, so the screen looks dark — but a few mA of
     * bias current remain. A rev >= 3.0 silicon would let us hold or
     * cut the backlight GPIO more cleanly; on this rev we accept the
     * residual drain and document it as a hardware limit. */

    vTaskDelay(pdMS_TO_TICKS(50));   /* UART drain */

    /* HP_PERIPH_PD wake path — see esp_sleep.h. IDF handles the
     * internal pull-up automatically. Confirmed working on hardware:
     * GPIO5 low triggers a cold boot with ESP_SLEEP_WAKEUP_GPIO set. */
    const uint64_t wake_mask = 1ULL << PK_POWER_WAKE_GPIO;
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
        wake_mask, ESP_GPIO_WAKEUP_GPIO_LOW));

    esp_deep_sleep_start();
}
