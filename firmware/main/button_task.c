/*
 * button_task.c — generic N-button polling + short/long/combo dispatch.
 *
 * Each button has its own small FSM:
 *
 *   RELEASED ─[level→0]──→ PRESSING (start debounce window)
 *
 *   PRESSING ─[level=1 within debounce]──→ RELEASED (bounce, drop)
 *   PRESSING ─[level=0 past debounce]───→ HELD_SHORT
 *
 *   HELD_SHORT ─[level=1]──────────────→ RELEASED (emit SHORT_PRESS if
 *                                                  not consumed by combo)
 *   HELD_SHORT ─[held ≥ LONG threshold]→ HELD_LONG (emit LONG_PRESS if
 *                                                   not combo-eligible
 *                                                   and not consumed)
 *
 *   HELD_LONG  ─[level=1]──────────────→ RELEASED  (silent — already fired
 *                                                   or quietly suppressed)
 *
 * Combo (UP + DOWN held ≥ 5 s) is detected after the per-button FSMs
 * advance: if both UP and DOWN are in HELD_SHORT or HELD_LONG, were
 * pressed within 1 s of each other, and have been held collectively
 * past the combo threshold, fire COMBO_BLE_PAIR once and mark both
 * buttons as consumed (so their eventual release won't emit a short
 * press).
 *
 * Why UP and DOWN don't emit single-button long press
 * ---------------------------------------------------
 * If they did, holding UP alone for 3 s would emit UP_LONG, and only
 * 2 s later if DOWN joined in would the combo fire — making the combo
 * arrive after the UP_LONG. Two distinct events fire for what feels
 * like one gesture. Suppressing UP/DOWN long-press resolves this
 * cleanly. TARE and MODE still emit long presses (they're not part of
 * any combo).
 */

#include "button_task.h"

#include <stdlib.h>     /* llabs */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "btn";

/* --- Tunables -------------------------------------------------------- */
#define BTN_POLL_MS              20             /* 50 Hz */
#define BTN_DEBOUNCE_US          (40   * 1000)
#define BTN_LONG_PRESS_US        (3000 * 1000)  /* single-button long press */
#define BTN_COMBO_PRESS_US       (5000 * 1000)  /* UP+DOWN combo */
#define BTN_COMBO_WINDOW_US      (1000 * 1000)  /* the two presses must land
                                                   within this of each other
                                                   to count as "together" */

/* --- Per-button state ----------------------------------------------- */
typedef enum {
    BTN_RELEASED,
    BTN_PRESSING,
    BTN_HELD_SHORT,
    BTN_HELD_LONG,
} btn_state_t;

typedef struct {
    pk_button_id_t  id;
    int             gpio;
    bool            combo_eligible;     /* part of UP+DOWN combo */
    bool            emits_long_press;   /* single-button long press allowed */
    const char     *label;              /* for logs */

    btn_state_t     state;
    int64_t         down_us;            /* when current press started */
    bool            consumed_by_combo;  /* this press has been swallowed by
                                           the combo handler — don't emit
                                           SHORT_PRESS on release */
} button_t;

static button_t s_buttons[PK_BTN_COUNT] = {
    [PK_BTN_TARE] = {
        .id = PK_BTN_TARE, .gpio = 26,
        .combo_eligible = false, .emits_long_press = true,
        .label = "TARE",
    },
    [PK_BTN_MODE] = {
        .id = PK_BTN_MODE, .gpio = 27,
        .combo_eligible = false, .emits_long_press = true,
        .label = "MODE",
    },
    [PK_BTN_UP] = {
        .id = PK_BTN_UP, .gpio = 22,
        .combo_eligible = true, .emits_long_press = false,
        .label = "UP",
    },
    [PK_BTN_DOWN] = {
        .id = PK_BTN_DOWN, .gpio = 23,
        .combo_eligible = true, .emits_long_press = false,
        .label = "DOWN",
    },
};

static pk_button_callback_t s_user_cb;

/* --- Event dispatch -------------------------------------------------- */
static const char *evt_name(pk_button_event_t evt)
{
    switch (evt) {
    case PK_BTN_EVT_SHORT_PRESS:    return "SHORT_PRESS";
    case PK_BTN_EVT_LONG_PRESS:     return "LONG_PRESS";
    case PK_BTN_EVT_COMBO_BLE_PAIR: return "COMBO_BLE_PAIR";
    default:                        return "?";
    }
}

static void emit(pk_button_id_t id, pk_button_event_t evt)
{
    const char *btn_label = (id < PK_BTN_COUNT) ? s_buttons[id].label : "?";
    ESP_LOGI(TAG, "%s %s", btn_label, evt_name(evt));
    if (s_user_cb) s_user_cb(id, evt);
}

/* --- Per-button FSM tick -------------------------------------------- */
static void poll_button(button_t *b, int64_t now)
{
    int level = gpio_get_level(b->gpio);   /* 0 = pressed (pull-up + GND) */

    switch (b->state) {
    case BTN_RELEASED:
        if (level == 0) {
            b->state = BTN_PRESSING;
            b->down_us = now;
            b->consumed_by_combo = false;
        }
        break;

    case BTN_PRESSING:
        if (level != 0) {
            /* bounced — drop */
            b->state = BTN_RELEASED;
        } else if (now - b->down_us >= BTN_DEBOUNCE_US) {
            b->state = BTN_HELD_SHORT;
        }
        break;

    case BTN_HELD_SHORT:
        if (level != 0) {
            /* released before long threshold → SHORT_PRESS */
            if (!b->consumed_by_combo) {
                emit(b->id, PK_BTN_EVT_SHORT_PRESS);
            }
            b->state = BTN_RELEASED;
        } else if (now - b->down_us >= BTN_LONG_PRESS_US) {
            /* held past long threshold */
            if (b->emits_long_press && !b->consumed_by_combo) {
                emit(b->id, PK_BTN_EVT_LONG_PRESS);
            }
            /* Either fired (TARE/MODE) or stays quiet (UP/DOWN waiting for combo). */
            b->state = BTN_HELD_LONG;
        }
        break;

    case BTN_HELD_LONG:
        if (level != 0) b->state = BTN_RELEASED;
        break;
    }
}

/* --- Combo detector ------------------------------------------------- *
 *
 * Runs after all per-button FSMs have advanced for this tick. Looks for
 * UP and DOWN both currently held and pressed roughly together; once
 * the joint hold crosses BTN_COMBO_PRESS_US, fires one combo event and
 * marks both buttons consumed so their release doesn't emit a short
 * press. */
static void detect_combo(int64_t now)
{
    button_t *up   = &s_buttons[PK_BTN_UP];
    button_t *down = &s_buttons[PK_BTN_DOWN];

    bool up_held   = (up->state   == BTN_HELD_SHORT || up->state   == BTN_HELD_LONG);
    bool down_held = (down->state == BTN_HELD_SHORT || down->state == BTN_HELD_LONG);

    if (!up_held || !down_held) return;
    if (up->consumed_by_combo || down->consumed_by_combo) return;

    int64_t window = llabs(up->down_us - down->down_us);
    if (window > BTN_COMBO_WINDOW_US) return;

    int64_t joint_start = (up->down_us > down->down_us) ? up->down_us : down->down_us;
    if (now - joint_start < BTN_COMBO_PRESS_US) return;

    emit(PK_BTN_UP, PK_BTN_EVT_COMBO_BLE_PAIR);
    up->consumed_by_combo   = true;
    down->consumed_by_combo = true;
}

/* --- Task ----------------------------------------------------------- */
static void button_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "button task running — TARE=GPIO%d  MODE=GPIO%d  "
                  "UP=GPIO%d  DOWN=GPIO%d (50 Hz polling, 40 ms debounce)",
             s_buttons[PK_BTN_TARE].gpio,
             s_buttons[PK_BTN_MODE].gpio,
             s_buttons[PK_BTN_UP].gpio,
             s_buttons[PK_BTN_DOWN].gpio);

    while (1) {
        int64_t now = esp_timer_get_time();

        for (int i = 0; i < PK_BTN_COUNT; ++i) {
            poll_button(&s_buttons[i], now);
        }
        detect_combo(now);

        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

/* --- Public init ---------------------------------------------------- */
esp_err_t pk_button_init(pk_button_callback_t cb)
{
    s_user_cb = cb;

    /* Configure all four buttons in a single gpio_config call by ORing
     * the pin bit masks — they share the same direction and pull
     * configuration. */
    uint64_t mask = 0;
    for (int i = 0; i < PK_BTN_COUNT; ++i) {
        mask |= (1ULL << s_buttons[i].gpio);
    }

    const gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        button_task, "btn", 3072, NULL, 3, NULL, 0);
    return (ok == pdTRUE) ? ESP_OK : ESP_ERR_NO_MEM;
}
