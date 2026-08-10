/*
 * Button driver for the device inputs. GPIOs come from the active hardware
 * profile (hw_config.c); the values below are the Shelly 1 Gen4 defaults.
 *   INPUT_PUSHBUTTON  (GPIO10) wall-switch / pushbutton input
 *   INPUT_TOUCH       Add-on digital input terminal (Add-on models only, pin per model)
 *   INPUT_DEVICE_BTN  (GPIO4)  onboard pair button
 *   INPUT_SWITCH_2    (GPIO10) 2nd wall-switch input (Shelly 2PM Gen4 only)
 *
 * All inputs have the same behavior (see on_button_event in app_main.cpp):
 *   SHORT_PRESS       -> Matter Toggle + relay tick
 *   DOUBLE_PRESS      -> ColorControl MoveToColorTemperature (2700K)
 *   LONG_PRESS_START  -> dim start (LevelControl MoveWithOnOff — turns on lamp if off)
 *   LONG_PRESS_STOP   -> dim stop (LevelControl Stop)
 *   SHORT_LONG_START  -> color temp adjust (ColorControl MoveColorTemp)
 *   SHORT_LONG_STOP   -> color temp stop (ColorControl StopMoveStep)
 *   6x click          -> MODE_TOGGLE (enable WiFi alongside Thread)
 *
 * Detection:
 *   Single short press has a DOUBLE_CLICK_WINDOW_MS delay to distinguish
 *   from double press. Long press (brightness) fires immediately on hold.
 *   Short-long (color temp) = tap + hold: first tap sets pending_clicks,
 *   second press held > LONG_PRESS_MS triggers SHORT_LONG_START.
 *
 * Polarity per input:
 *   - INPUT_PUSHBUTTON:    active-high (production 230V optocoupler);
 *                       active-low in BENCH_MODE (internal pull-up)
 *   - INPUT_TOUCH:      active-low (Add-on has built-in pull-up to 3V3;
 *                       connecting to GND = pressed). No internal pull
 *                       because it conflicts with the Add-on pull-up.
 *   - INPUT_DEVICE_BTN: always active-low (internal pull-up, button to GND)
 *
 * Driver uses ISR-to-queue with its own FreeRTOS task so that
 * Matter stack calls happen in task context.
 *
 * NOTE on optocoupler debounce:
 *   gpio_get_level() in the ISR sometimes returns the old value for an
 *   optocoupler because the rise time is slower than the ISR latency.
 *   Therefore the level is NOT read in the ISR but in the task, after
 *   a short DEBOUNCE_READ_US delay. This way handle_edge() always reads
 *   the stable final state.
 */

#include "button.h"
#include "app_config.h"
#include "hw_config.h"

#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "button";

/* Wait this duration after an ISR before reading the GPIO level.
 * Optocouplers typically have 1-5 ms rise time; 10 ms is safe. */
#define DEBOUNCE_READ_US    10000   /* 10 ms */

typedef struct {
    input_id_t id;
    int64_t    t_us;    /* timestamp of the ISR */
} btn_isr_msg_t;

#define CLICK_HISTORY 8

typedef struct {
    int     gpio;
    bool    enabled;
    bool    active_low;
    bool    pressed;
    int64_t press_start_us;
    bool    long_fired;
    int64_t click_hist[CLICK_HISTORY];
    uint8_t click_idx;
    /* double-click / short-long detection */
    int64_t pending_short_us;   /* timestamp of last short release (0 = none) */
    uint8_t pending_clicks;     /* number of short presses awaiting dispatch */
    bool    in_combo_long;      /* true when current hold follows a recent tap */
} btn_state_t;

static QueueHandle_t s_evt_q;
static button_cb_t   s_cb;
static btn_state_t   s_state[INPUT_COUNT];

static inline bool level_is_pressed(const btn_state_t *s, int level)
{
    return s->active_low ? (level == 0) : (level == 1);
}

static void IRAM_ATTR btn_isr(void *arg)
{
    input_id_t id = (input_id_t)(uintptr_t)arg;
    btn_isr_msg_t msg = {
        .id   = id,
        .t_us = esp_timer_get_time(),
    };
    BaseType_t hpw = pdFALSE;
    xQueueSendFromISR(s_evt_q, &msg, &hpw);
    if (hpw) portYIELD_FROM_ISR();
}

static void handle_edge(btn_isr_msg_t *m)
{
    btn_state_t *s = &s_state[m->id];
    if (!s->enabled) return;

    /* Wait until the pin is stable (optocoupler rise time / debounce) */
    int64_t now = esp_timer_get_time();
    int64_t elapsed = now - m->t_us;
    if (elapsed < DEBOUNCE_READ_US) {
        vTaskDelay(pdMS_TO_TICKS((DEBOUNCE_READ_US - elapsed) / 1000 + 1));
    }

    /* Read level now that the pin is stable */
    int level = gpio_get_level(s->gpio);
    bool pressed_now = level_is_pressed(s, level);

    ESP_LOGD(TAG, "edge id=%d level=%d pressed_now=%d was_pressed=%d",
             m->id, level, pressed_now, s->pressed);

    if (pressed_now && !s->pressed) {
        /* rising edge: button pressed */
        s->pressed        = true;
        s->press_start_us = m->t_us;
        s->long_fired     = false;
        /* If there are pending clicks (recent short press), this press
         * could become a short-long combo (tap + hold). */
        s->in_combo_long  = (s->pending_clicks > 0);
        if (s_cb) s_cb(m->id, BTN_EVT_CONTACT_CLOSED);

    } else if (!pressed_now && s->pressed) {
        /* falling edge: button released */
        int64_t dur_ms = (m->t_us - s->press_start_us) / 1000;
        s->pressed = false;
        if (s_cb) s_cb(m->id, BTN_EVT_CONTACT_OPEN);

        if (s->long_fired) {
            /* Release after a long hold */
            if (s->in_combo_long) {
                if (s_cb) s_cb(m->id, BTN_EVT_SHORT_LONG_STOP);
            } else {
                if (s_cb) s_cb(m->id, BTN_EVT_LONG_PRESS_STOP);
            }
            s->pending_clicks = 0;
            s->pending_short_us = 0;
            s->in_combo_long = false;

        } else if (dur_ms >= LONG_PRESS_MS) {
            /* edge case: long_fired not yet set but duration >= threshold */
            s->long_fired = true;
            if (s->in_combo_long) {
                if (s_cb) s_cb(m->id, BTN_EVT_SHORT_LONG_START);
                if (s_cb) s_cb(m->id, BTN_EVT_SHORT_LONG_STOP);
            } else {
                if (s_cb) s_cb(m->id, BTN_EVT_LONG_PRESS_START);
                if (s_cb) s_cb(m->id, BTN_EVT_LONG_PRESS_STOP);
            }
            s->pending_clicks = 0;
            s->pending_short_us = 0;
            s->in_combo_long = false;

        } else if (dur_ms > 20) {
            /* short press — check 6x mode toggle first (wider window) */
            s->click_hist[s->click_idx] = m->t_us;
            s->click_idx = (s->click_idx + 1) % CLICK_HISTORY;

            uint8_t cnt = 0;
            for (int i = 0; i < CLICK_HISTORY; i++) {
                int64_t t = s->click_hist[i];
                if (t == 0) continue;
                if ((m->t_us - t) / 1000 <= MODE_TOGGLE_WINDOW_MS) cnt++;
            }

            if (cnt >= MODE_TOGGLE_CLICKS) {
                memset(s->click_hist, 0, sizeof(s->click_hist));
                s->pending_clicks = 0;
                s->pending_short_us = 0;
                if (s_cb) s_cb(m->id, BTN_EVT_MODE_TOGGLE);
            } else {
                /* Defer dispatch — wait for possible double-click or short-long */
                s->pending_clicks++;
                s->pending_short_us = m->t_us;
            }
            s->in_combo_long = false;
        }
    }
    /* otherwise: duplicate ISR on same edge or noise -> ignore */
}

static void check_long_press(int64_t now_us)
{
    for (int i = 0; i < INPUT_COUNT; i++) {
        btn_state_t *s = &s_state[i];
        if (!s->enabled || !s->pressed || s->long_fired) continue;
        int64_t dur_ms = (now_us - s->press_start_us) / 1000;
        if (dur_ms >= LONG_PRESS_MS) {
            s->long_fired = true;
            if (s->in_combo_long) {
                if (s_cb) s_cb((input_id_t)i, BTN_EVT_SHORT_LONG_START);
            } else {
                if (s_cb) s_cb((input_id_t)i, BTN_EVT_LONG_PRESS_START);
            }
        }
    }
}

static void check_pending(int64_t now_us)
{
    for (int i = 0; i < INPUT_COUNT; i++) {
        btn_state_t *s = &s_state[i];
        if (!s->enabled || s->pending_clicks == 0 || s->pressed) continue;

        int64_t elapsed_ms = (now_us - s->pending_short_us) / 1000;
        if (elapsed_ms < DOUBLE_CLICK_WINDOW_MS) continue;

        uint8_t cnt = s->pending_clicks;
        s->pending_clicks = 0;
        s->pending_short_us = 0;

        if (cnt == 2) {
            if (s_cb) s_cb((input_id_t)i, BTN_EVT_DOUBLE_PRESS);
        } else {
            /* 1 click, or 3-5 clicks (fallback to single toggle) */
            if (s_cb) s_cb((input_id_t)i, BTN_EVT_SHORT_PRESS);
        }
    }
}

static void btn_task(void *arg)
{
    btn_isr_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_evt_q, &msg, pdMS_TO_TICKS(50)) == pdTRUE) {
            handle_edge(&msg);
        }
        int64_t now = esp_timer_get_time();
        check_long_press(now);
        check_pending(now);
    }
}

void button_driver_init(button_cb_t cb)
{
    s_cb = cb;

    const hw_profile_t *hw = hw_profile();
    const int switch_gpio = hw->switch_gpio;
    const int touch_gpio  = hw->addon_digital_gpio;
    const bool touch_on   = hw->has_addon && touch_gpio >= 0;

    /* INPUT_PUSHBUTTON: wall-switch input, active-high in production
     * (optocoupler 230V), active-low in BENCH_MODE (internal pull-up). */
    s_state[INPUT_PUSHBUTTON].gpio       = switch_gpio;
    s_state[INPUT_PUSHBUTTON].enabled    = true;
    s_state[INPUT_PUSHBUTTON].active_low = (g_bench_mode != 0);

    /* INPUT_TOUCH: Add-on digital input terminal. The pin differs per model
     * (1 Gen4 GPIO18, 1PM GPIO12, 2PM GPIO1) and the Mini has no Add-on.
     * Connecting to GND = pressed -> active-low. Internal pull-up provides
     * a definite idle-HIGH state; the Add-on isolator does not supply a
     * pull-up on the ESP32 side of this line. */
    s_state[INPUT_TOUCH].gpio       = touch_gpio;
    s_state[INPUT_TOUCH].enabled    = touch_on;
    s_state[INPUT_TOUCH].active_low = true;

    /* INPUT_DEVICE_BTN: onboard pair button, always active-low. */
    s_state[INPUT_DEVICE_BTN].gpio       = hw->button_gpio;
    s_state[INPUT_DEVICE_BTN].enabled    = true;
    s_state[INPUT_DEVICE_BTN].active_low = true;

    /* INPUT_SWITCH_2: 2nd wall-switch input, only on the 2PM (switch2_gpio >= 0).
     * Same polarity semantics as INPUT_PUSHBUTTON. */
    const int switch2_gpio = hw->switch2_gpio;
    s_state[INPUT_SWITCH_2].gpio       = switch2_gpio;
    s_state[INPUT_SWITCH_2].enabled    = (switch2_gpio >= 0);
    s_state[INPUT_SWITCH_2].active_low = (g_bench_mode != 0);

    /* ---------- GPIO-config ---------- */

    ESP_LOGI(TAG, "BD-STEP-1: gpio_config pushbutton GPIO%d bench=%d (active_low=%d)",
             switch_gpio, g_bench_mode, s_state[INPUT_PUSHBUTTON].active_low);

    gpio_config_t pushbutton_cfg = {
        .pin_bit_mask = (1ULL << switch_gpio),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = g_bench_mode ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&pushbutton_cfg);
    ESP_LOGI(TAG, "BD-STEP-2: gpio_config pushbutton done");

    if (touch_on) {
        /* Add-on digital in: internal pull-up so the pin idles HIGH when
         * nothing drives it (the isolator does not pull up the ESP32 side). */
        gpio_config_t touch_cfg = {
            .pin_bit_mask = (1ULL << touch_gpio),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_ANYEDGE,
        };
        gpio_config(&touch_cfg);
        ESP_LOGI(TAG, "BD-STEP-2b: gpio_config touch/addon-digital-in done");
    } else {
        ESP_LOGI(TAG, "BD-STEP-2b: touch/addon-digital-in skipped (no Add-on)");
    }

    gpio_config_t devbtn_cfg = {
        .pin_bit_mask = (1ULL << s_state[INPUT_DEVICE_BTN].gpio),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&devbtn_cfg);
    ESP_LOGI(TAG, "BD-STEP-2c: gpio_config device_btn done");

    if (s_state[INPUT_SWITCH_2].enabled) {
        gpio_config_t switch2_cfg = {
            .pin_bit_mask = (1ULL << switch2_gpio),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = g_bench_mode ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_ANYEDGE,
        };
        gpio_config(&switch2_cfg);
        ESP_LOGI(TAG, "BD-STEP-2d: gpio_config switch_2 (GPIO%d) done", switch2_gpio);
    }

    /* ---------- queue + ISR-service ---------- */

    s_evt_q = xQueueCreate(16, sizeof(btn_isr_msg_t));
    ESP_LOGI(TAG, "BD-STEP-3: xQueueCreate done q=%p", s_evt_q);

    esp_err_t isr_svc_err = gpio_install_isr_service(0);
    ESP_LOGI(TAG, "BD-STEP-4: gpio_install_isr_service -> %d (0=OK)", isr_svc_err);

    gpio_isr_handler_add(switch_gpio, btn_isr,
                         (void *)(uintptr_t)INPUT_PUSHBUTTON);
    ESP_LOGI(TAG, "BD-STEP-5a: isr_handler_add pushbutton (GPIO%d) done", switch_gpio);

    if (touch_on) {
        gpio_isr_handler_add(touch_gpio, btn_isr,
                             (void *)(uintptr_t)INPUT_TOUCH);
        ESP_LOGI(TAG, "BD-STEP-5b: isr_handler_add touch/addon-digital-in (GPIO%d) done",
                 touch_gpio);
    }

    gpio_isr_handler_add(s_state[INPUT_DEVICE_BTN].gpio,
                         btn_isr, (void *)(uintptr_t)INPUT_DEVICE_BTN);
    ESP_LOGI(TAG, "BD-STEP-5c: isr_handler_add device_btn (GPIO%d) done",
             s_state[INPUT_DEVICE_BTN].gpio);

    if (s_state[INPUT_SWITCH_2].enabled) {
        gpio_isr_handler_add(switch2_gpio, btn_isr,
                             (void *)(uintptr_t)INPUT_SWITCH_2);
        ESP_LOGI(TAG, "BD-STEP-5d: isr_handler_add switch_2 (GPIO%d) done", switch2_gpio);
    }

    BaseType_t btn_r = xTaskCreate(btn_task, "btn_task", 3072, NULL, 10, NULL);
    ESP_LOGI(TAG, "BD-STEP-6: xTaskCreate btn_task -> %s",
             btn_r == pdPASS ? "OK" : "FAIL");

    ESP_LOGI(TAG, "button driver init (pushbutton=GPIO%d addon-digital-in=%s device_btn=GPIO%d)",
             switch_gpio, touch_on ? "on" : "off",
             s_state[INPUT_DEVICE_BTN].gpio);
}
