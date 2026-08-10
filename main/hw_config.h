#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Runtime hardware profile.
 *
 * The Gen4 line shares one ESP32-C6 module (ESP-Shelly-C68F) but wires the
 * relay, wall-switch input, onboard button and status LED to different GPIOs
 * per device. The pin assignments are recovered from the official Shelly
 * stock firmware images (2.0.0) by disassembly — see hw_config.c for the
 * full table and STOCK_GPIO.md for how each field was proven.
 *
 * The active profile is chosen at runtime (stored in NVS, selectable on the
 * management dashboard) instead of compile time, so one firmware image serves
 * every supported device. Changing it only remaps GPIOs — our Matter data
 * model is a generic switch/relay, so no re-commissioning is required.
 */
typedef enum {
    HW_1_GEN4      = 0,   /* Shelly 1 Gen4 (full size, Add-on connector) */
    HW_1_MINI_GEN4 = 1,   /* Shelly 1 Mini Gen4 */
    HW_1PM_GEN4    = 2,   /* Shelly 1PM Gen4 (BL0942 power meter) */
    HW_2PM_GEN4    = 3,   /* Shelly Plus 2PM Gen4 (2 relays, ADE7953 dual meter) */
    HW_TYPE_COUNT
} hw_device_type_t;

/* Power-meter IC fitted to the device. */
typedef enum {
    PM_NONE    = 0,
    PM_BL0942  = 1,   /* single-channel, UART (1PM Gen4) */
    PM_ADE7953 = 2,   /* dual-channel, I2C (2PM Gen4) */
} pm_kind_t;

typedef struct {
    hw_device_type_t type;
    const char *name;        /* human-readable, for dashboard/logs */
    int relay_gpio;
    int relay2_gpio;         /* 2nd relay (-1 = none, e.g. 2PM) */
    int switch_gpio;         /* wall-switch / pushbutton input */
    int switch2_gpio;        /* 2nd wall-switch input (-1 = none) */
    int button_gpio;         /* onboard pair button (active-low) */
    int led_gpio;            /* status LED (-1 = none) */
    bool led_active_high;
    bool has_addon;          /* Shelly Plus Add-on inputs (1-Wire/digital/analog) */
    int addon_digital_gpio;  /* Add-on Digital IN (-1 = none); differs per model */
    bool has_pm;             /* any power meter present (pm_type != PM_NONE) */
    pm_kind_t pm_type;       /* which meter IC */
    int pm_uart_tx;          /* BL0942 UART TX (PM_BL0942) */
    int pm_uart_rx;          /* BL0942 UART RX (PM_BL0942) */
    int pm_i2c_sda;          /* ADE7953 I2C SDA (PM_ADE7953) */
    int pm_i2c_scl;          /* ADE7953 I2C SCL (PM_ADE7953) */
    int pm_i2c_irq;          /* ADE7953 IRQ (-1 = unused) */
} hw_profile_t;

/* Load the active device type from NVS and select the matching profile.
 * Call once early in app_main, before any driver init. Falls back to
 * HW_1_GEN4 when nothing is stored or the stored value is invalid. */
void hw_config_init(void);

/* The active profile (valid after hw_config_init). */
const hw_profile_t *hw_profile(void);

/* Persist a new device type to NVS. Caller reboots afterwards so the new
 * profile takes effect. */
esp_err_t hw_device_type_set(hw_device_type_t type);

/* Profile table lookup for a given type (for the dashboard listing). */
const hw_profile_t *hw_profile_for(hw_device_type_t type);

#ifdef __cplusplus
}
#endif
