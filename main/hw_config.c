/*
 * Runtime hardware profile selection. See hw_config.h.
 *
 * Pin assignments per device, recovered from the official Shelly stock
 * firmware images (2.0.0, app codes S1G4 / Mini1G4 / S1PMG4 / S2PMG4) by
 * disassembling the peripheral constructors. STOCK_GPIO.md documents how each
 * value was proven.
 *
 *   Function      | 1 Gen4 | 1 Mini Gen4 | 1PM Gen4              | 2PM Gen4
 *   Relay         | GPIO5  | GPIO10      | GPIO4                | GPIO5 + GPIO3
 *   Switch input  | GPIO10 | GPIO12      | GPIO10               | GPIO11 + GPIO10
 *   Button        | GPIO4  | GPIO22      | GPIO1                | GPIO12
 *   Status LED    | GPIO15 | GPIO5       | GPIO11               | GPIO18  (all active-low)
 *   Power meter   | -      | -           | BL0942 UART1         | ADE7953 I2C
 *                 |        |             | GPIO7 + GPIO6        | IRQ=GPIO19
 *   Add-on        | yes    | no          | yes                  | yes
 *   Add-on Dig IN | GPIO18 | -           | GPIO12               | GPIO1
 *
 * Only the Mini lacks the Shelly Plus Add-on connector (its firmware carries
 * no 1-Wire/DHT code at all); the other three expose it. Analog IN (GPIO17)
 * and 1-Wire (GPIO16/GPIO9) are the same on every model — only Digital IN
 * moves, so it lives in the profile instead of Kconfig.
 *
 * Not taken from stock: the ADE7953 I2C SDA/SCL pins. Stock reads those from
 * the device configuration in NVS rather than hardcoding them, so they cannot
 * be extracted from the image; the values below are unverified.
 */

#include "hw_config.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "hw_config";
static const char *NVS_NS  = "hw";
static const char *NVS_KEY = "dev_type";

static const hw_profile_t s_profiles[HW_TYPE_COUNT] = {
    [HW_1_GEN4] = {
        .type = HW_1_GEN4, .name = "Shelly 1 Gen4",
        .relay_gpio = 5, .relay2_gpio = -1,
        .switch_gpio = 10, .switch2_gpio = -1, .button_gpio = 4,
        .led_gpio = 15, .led_active_high = false,
        .has_addon = true, .addon_digital_gpio = 18,
        .has_pm = false, .pm_type = PM_NONE,
        .pm_uart_tx = -1, .pm_uart_rx = -1,
        .pm_i2c_sda = -1, .pm_i2c_scl = -1, .pm_i2c_irq = -1,
    },
    [HW_1_MINI_GEN4] = {
        .type = HW_1_MINI_GEN4, .name = "Shelly 1 Mini Gen4",
        .relay_gpio = 10, .relay2_gpio = -1,
        .switch_gpio = 12, .switch2_gpio = -1, .button_gpio = 22,
        .led_gpio = 5, .led_active_high = false,
        .has_addon = false, .addon_digital_gpio = -1,
        .has_pm = false, .pm_type = PM_NONE,
        .pm_uart_tx = -1, .pm_uart_rx = -1,
        .pm_i2c_sda = -1, .pm_i2c_scl = -1, .pm_i2c_irq = -1,
    },
    [HW_1PM_GEN4] = {
        .type = HW_1PM_GEN4, .name = "Shelly 1PM Gen4",
        .relay_gpio = 4, .relay2_gpio = -1,
        .switch_gpio = 10, .switch2_gpio = -1, .button_gpio = 1,
        .led_gpio = 11, .led_active_high = false,
        .has_addon = true, .addon_digital_gpio = 12,
        .has_pm = true, .pm_type = PM_BL0942,
        .pm_uart_tx = 6, .pm_uart_rx = 7,
        .pm_i2c_sda = -1, .pm_i2c_scl = -1, .pm_i2c_irq = -1,
    },
    [HW_2PM_GEN4] = {
        /* Relays GPIO5/GPIO3 and switches GPIO11/GPIO10 per the per-model pin
         * table stock keys on "S4SW-002P16EU". The human-readable "GPIO Pinout"
         * table on esphome.io swaps relay/switch on these four pins; stock is
         * the authority here. */
        .type = HW_2PM_GEN4, .name = "Shelly 2PM Gen4",
        .relay_gpio = 5, .relay2_gpio = 3,
        .switch_gpio = 11, .switch2_gpio = 10, .button_gpio = 12,
        .led_gpio = 18, .led_active_high = false,
        .has_addon = true, .addon_digital_gpio = 1,
        .has_pm = true, .pm_type = PM_ADE7953,
        .pm_uart_tx = -1, .pm_uart_rx = -1,
        .pm_i2c_sda = 6, .pm_i2c_scl = 7, .pm_i2c_irq = 19,
    },
};

static const hw_profile_t *s_active = &s_profiles[HW_1_GEN4];

const hw_profile_t *hw_profile_for(hw_device_type_t type)
{
    if ((int)type < 0 || (int)type >= HW_TYPE_COUNT) return &s_profiles[HW_1_GEN4];
    return &s_profiles[type];
}

void hw_config_init(void)
{
    uint8_t v = HW_1_GEN4;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY, &v);
        nvs_close(h);
    }
    if (v >= HW_TYPE_COUNT) v = HW_1_GEN4;
    s_active = &s_profiles[v];
    ESP_LOGI(TAG, "device type = %d (%s): relay=GPIO%d switch=GPIO%d button=GPIO%d led=GPIO%d addon=%d pm=%d",
             s_active->type, s_active->name, s_active->relay_gpio, s_active->switch_gpio,
             s_active->button_gpio, s_active->led_gpio, s_active->has_addon, s_active->has_pm);
}

const hw_profile_t *hw_profile(void)
{
    return s_active;
}

esp_err_t hw_device_type_set(hw_device_type_t type)
{
    if ((int)type < 0 || (int)type >= HW_TYPE_COUNT) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, NVS_KEY, (uint8_t)type);
    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "device type saved: %d (%s)", type, s_profiles[type].name);
    return err;
}
