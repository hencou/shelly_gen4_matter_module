/*
 * Local relay(s). Single relay on 1/Mini/1PM Gen4; two relays on 2PM Gen4;
 * none on i4 Gen4.
 * GPIOs come from the active hardware profile. State is persisted in NVS per
 * channel for power-outage recovery.
 */

#include "relay.h"
#include "app_config.h"
#include "hw_config.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "relay";
static const char *NVS_NAMESPACE = "relay";
static const char *NVS_KEY_STATE[RELAY_MAX_CH] = { "state", "state2" };

static bool s_state[RELAY_MAX_CH];
static int  s_gpio[RELAY_MAX_CH] = { -1, -1 };
static int  s_count = 1;

static void persist(int ch)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, NVS_KEY_STATE[ch], s_state[ch] ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

static bool restore(int ch)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    nvs_get_u8(h, NVS_KEY_STATE[ch], &v);
    nvs_close(h);
    return v != 0;
}

void relay_init(void)
{
    const hw_profile_t *hw = hw_profile();
    s_gpio[0] = hw->relay_gpio;
    s_gpio[1] = hw->relay2_gpio;
    s_count = (hw->relay_gpio < 0) ? 0 : (hw->relay2_gpio >= 0) ? 2 : 1;
    if (s_count == 0) {
        ESP_LOGI(TAG, "no relay on this model");
        return;
    }

    uint64_t mask = 0;
    for (int ch = 0; ch < s_count; ch++) {
        mask |= (1ULL << s_gpio[ch]);
    }
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);

    for (int ch = 0; ch < s_count; ch++) {
        s_state[ch] = restore(ch);
        gpio_set_level(s_gpio[ch], s_state[ch] ? 1 : 0);
        ESP_LOGI(TAG, "relay ch%d init on GPIO%d, state=%s",
                 ch, s_gpio[ch], s_state[ch] ? "ON" : "OFF");
    }
}

int relay_channel_count(void)
{
    return s_count;
}

void relay_set_ch(int ch, bool on)
{
    if (ch < 0 || ch >= s_count) {
        return;
    }
    s_state[ch] = on;
    gpio_set_level(s_gpio[ch], on ? 1 : 0);
    persist(ch);
}

bool relay_get_ch(int ch)
{
    if (ch < 0 || ch >= s_count) {
        return false;
    }
    return s_state[ch];
}

void relay_toggle_ch(int ch)
{
    if (ch < 0 || ch >= s_count) {
        return;
    }
    relay_set_ch(ch, !s_state[ch]);
}

void relay_set(bool on)    { relay_set_ch(0, on); }
bool relay_get(void)       { return relay_get_ch(0); }
void relay_toggle(void)    { relay_toggle_ch(0); }
