#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Event type emitted by the button driver.
 * - SHORT_PRESS      fires on release within LONG_PRESS_MS
 * - LONG_PRESS_START fires when held > LONG_PRESS_MS
 * - LONG_PRESS_STOP  fires on release after a LONG_PRESS_START
 * - MODE_TOGGLE      fires after MODE_TOGGLE_CLICKS (6) taps in MODE_TOGGLE_WINDOW_MS
 *                    Universal across all 3 inputs. Enables WiFi alongside
 *                    Thread for configuration (non-persistent, lost on reboot).
 * - CONTACT_CLOSED   fires on every press/close edge (state-following)
 * - CONTACT_OPEN     fires on every release/open edge (state-following)
 *   These two events enable EP5 (state-follow switch): bind EP5 for a
 *   maintained switch (On/Off follows contact), bind EP1 for momentary (Toggle).
 * - DOUBLE_PRESS     fires after 2 rapid short presses (within DOUBLE_CLICK_WINDOW_MS)
 *                    Used for setting color temperature to default (2700K).
 * - SHORT_LONG_START fires when a short press is followed by a long hold
 *                    Used for continuous color temperature adjustment.
 * - SHORT_LONG_STOP  fires on release after a SHORT_LONG_START
 */
typedef enum {
    BTN_EVT_SHORT_PRESS = 0,
    BTN_EVT_LONG_PRESS_START,
    BTN_EVT_LONG_PRESS_STOP,
    BTN_EVT_MODE_TOGGLE,
    BTN_EVT_CONTACT_CLOSED,
    BTN_EVT_CONTACT_OPEN,
    BTN_EVT_DOUBLE_PRESS,
    BTN_EVT_SHORT_LONG_START,
    BTN_EVT_SHORT_LONG_STOP,
} button_event_t;

typedef void (*button_cb_t)(input_id_t input, button_event_t evt);

void button_driver_init(button_cb_t cb);

#ifdef __cplusplus
}
#endif
