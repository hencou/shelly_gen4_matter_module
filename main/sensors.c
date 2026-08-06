/*
 * Sensor tasks:
 *   1) DS18B20 via dual-pin 1-Wire (PIN_ONEWIRE_TX + PIN_ONEWIRE_RX):
 *      The Shelly Plus Add-on uses an ISO7221A galvanic isolator that
 *      splits the bidirectional 1-Wire protocol into separate TX (output) and
 *      RX (input) lines. TX = GPIO9 (data out), RX = GPIO16 (data in).
 *      Every TEMP_REPORT_INT_S seconds a conversion + ReadScratchpad.
 *      Reports centi-degrees Celsius (ZCL Temperature Measurement format).
 *   2) Analog IN / occupancy (PIN_LD2410_INPUT):
 *      The Add-on encodes the 0–10 V analog input as a PWM duty cycle.
 *      occ_task measures the duty cycle by rapid-sampling over a 100 ms
 *      window and thresholds at 50 % to derive occupied / clear.
 */

#include "sensors.h"
#include "app_config.h"
#include "hw_config.h"

#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_private/periph_ctrl.h"
#include "soc/periph_defs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sensors";

/* Latest values cached by the sensor tasks. The management page reads these
 * instead of driving the 1-Wire bus itself — two masters on the same bus race
 * and make the on-demand probe miss the DS18B20 presence pulse. */
static volatile int16_t s_last_temp_centi;
static volatile bool    s_temp_valid;
static volatile int     s_last_duty = -1;

bool sensors_temp_get_centi(int16_t *out)
{
    if (!s_temp_valid) return false;
    if (out) *out = s_last_temp_centi;
    return true;
}

int sensors_occupancy_duty(void)
{
    return s_last_duty;
}

/* ========================== Dual-pin 1-Wire / DS18B20 ========================== */
/* The Shelly Plus Add-on uses an ISO7221A dual digital isolator.
 * TX pin (GPIO9)  = output: ESP32 sends commands to the DS18B20
 * RX pin (GPIO16) = input:  ESP32 reads responses from the DS18B20
 * Standard 1-Wire (single-pin) does not work due to the galvanic isolation. */

#define OW_TX  PIN_ONEWIRE_TX
#define OW_RX  PIN_ONEWIRE_RX

static inline void ow_tx_low(void)  { gpio_set_level(OW_TX, 0); }
static inline void ow_tx_high(void) { gpio_set_level(OW_TX, 1); }
static inline int  ow_rx_read(void) { return gpio_get_level(OW_RX); }

static bool ow_reset(void)
{
    /* Wait until bus idle (RX=HIGH) */
    uint8_t retries = 125;
    do {
        if (--retries == 0) return false;
        esp_rom_delay_us(2);
    } while (!ow_rx_read());

    /* 480 µs reset pulse via TX */
    ow_tx_low();
    esp_rom_delay_us(480);
    ow_tx_high();
    esp_rom_delay_us(70);
    bool present = !ow_rx_read();
    esp_rom_delay_us(410);
    return present;
}

static void ow_write_bit(int b)
{
    ow_tx_low();
    if (b) {
        esp_rom_delay_us(10);
        ow_tx_high();
        esp_rom_delay_us(55);
    } else {
        esp_rom_delay_us(65);
        ow_tx_high();
        esp_rom_delay_us(5);
    }
}

static int ow_read_bit(void)
{
    ow_tx_low();
    esp_rom_delay_us(3);
    ow_tx_high();
    esp_rom_delay_us(9);
    int v = ow_rx_read();
    esp_rom_delay_us(53);
    return v;
}

static void ow_write_byte(uint8_t b)
{
    for (int i = 0; i < 8; i++) {
        ow_write_bit(b & 1);
        b >>= 1;
    }
}

static uint8_t ow_read_byte(void)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (ow_read_bit() << i);
    }
    return v;
}

static bool ds18b20_read_centi_c(int16_t *out)
{
    if (!ow_reset()) {
        return false;
    }
    ow_write_byte(0xCC);  /* Skip ROM */
    ow_write_byte(0x44);  /* Convert T */
    vTaskDelay(pdMS_TO_TICKS(800));  /* 12-bit max conversion */

    if (!ow_reset()) {
        return false;
    }
    ow_write_byte(0xCC);
    ow_write_byte(0xBE);  /* Read Scratchpad */

    uint8_t lsb = ow_read_byte();
    uint8_t msb = ow_read_byte();
    /* skip remaining bytes */
    for (int i = 0; i < 7; i++) (void)ow_read_byte();

    int16_t raw = (int16_t)((msb << 8) | lsb);
    /* raw is in 1/16 °C. Convert to centi-°C:  raw * 100 / 16 */
    *out = (int16_t)(((int32_t)raw * 100) / 16);
    return true;
}

static temp_cb_t s_temp_cb;
static void temp_task(void *arg)
{
    /* TX pin: output, idle high */
    gpio_config_t tx_cfg = {
        .pin_bit_mask = (1ULL << OW_TX),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&tx_cfg);
    ow_tx_high();

    /* RX pin: input (gpio_reset_pin already called in sensors_init;
     * Add-on has its own 4.7kΩ pull-up via isolator) */
    gpio_config_t rx_cfg = {
        .pin_bit_mask = (1ULL << OW_RX),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&rx_cfg);

    while (1) {
        int16_t centi = 0;
        if (ds18b20_read_centi_c(&centi)) {
            s_last_temp_centi = centi;
            s_temp_valid = true;
            if (s_temp_cb) s_temp_cb(centi);
            ESP_LOGI(TAG, "temp = %d.%02d °C", centi / 100, abs(centi % 100));
        } else {
            s_temp_valid = false;
            ESP_LOGW(TAG, "DS18B20 read failed");
        }
        vTaskDelay(pdMS_TO_TICKS(TEMP_REPORT_INT_S * 1000));
    }
}

/* ========================== Occupancy (Analog IN as PWM duty cycle) ====== */

/* The Add-on encodes the 0–10 V Analog IN voltage as a PWM duty cycle on
 * GPIO17.  A single gpio_get_level() catches a random point in the PWM
 * waveform, so we sample rapidly over a 100 ms window and compute the
 * percentage of HIGH samples.  A duty cycle above OCC_DUTY_THRESHOLD_PCT
 * means "occupied". */

#define OCC_SAMPLE_WINDOW_US  100000   /* 100 ms measurement window */
#define OCC_SAMPLE_INTERVAL_US   100   /* 100 µs between samples   */
#define OCC_DUTY_THRESHOLD_PCT    25   /* ≥25 % duty (≈2.5 V) → occupied */

static occupancy_cb_t s_occ_cb;
static analog_cb_t s_analog_cb;

static int measure_duty_pct(void)
{
    int high = 0, total = 0;
    for (int us = 0; us < OCC_SAMPLE_WINDOW_US; us += OCC_SAMPLE_INTERVAL_US) {
        if (gpio_get_level(PIN_LD2410_INPUT)) high++;
        total++;
        esp_rom_delay_us(OCC_SAMPLE_INTERVAL_US);
    }
    return (high * 100) / total;
}

static void occ_task(void *arg)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_LD2410_INPUT),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    int last_occ = -1;
    for (;;) {
        int duty = measure_duty_pct();
        s_last_duty = duty;
        if (s_analog_cb) s_analog_cb((uint8_t)duty);
        int occ  = (duty >= OCC_DUTY_THRESHOLD_PCT) ? 1 : 0;
        if (occ != last_occ) {
            last_occ = occ;
            if (s_occ_cb) s_occ_cb(occ == 1);
            ESP_LOGI(TAG, "occupancy = %s (duty %d%%)", occ ? "occupied" : "clear", duty);
        }
        vTaskDelay(pdMS_TO_TICKS(OCC_DEBOUNCE_MS));
    }
}

/* ========================== init ========================== */

void sensors_init(temp_cb_t temp_cb, occupancy_cb_t occ_cb, analog_cb_t analog_cb)
{
    s_temp_cb   = temp_cb;
    s_occ_cb    = occ_cb;
    s_analog_cb = analog_cb;

    if (!hw_profile()->has_addon) {
        /* Add-on inputs (DS18B20 1-Wire + analog occupancy) only exist on the
         * full-size Shelly 1 Gen4. Mini/PM have no Add-on connector — skip the
         * sensor tasks so their GPIOs are left untouched. */
        ESP_LOGI(TAG, "no Add-on on this device — sensor tasks skipped");
        return;
    }

    if (g_bench_mode) {
        /* Bench mode: skip sensor tasks so GPIO16 (U0TXD) and GPIO17 (U0RXD)
         * remain available for UART0 serial debugging via J6 header.
         * On the ESP32-C6, GPIO16/17 are the default UART0 pins; temp_task and
         * occ_task reconfigure them as 1-Wire RX and GPIO input respectively,
         * which kills serial output. GPIO9 (1-Wire TX) is also kept free. */
        ESP_LOGW(TAG, "bench_mode ON: sensor tasks skipped (GPIO9/16/17 kept free)");
    } else {
        /* GPIO16/17 are UART0 TX/RX by default on the ESP32-C6.
         *
         * The UART0 peripheral clock is enabled twice before we get here:
         *   1) ESP-IDF console init at boot  (ref_count 0→1)
         *   2) uart_driver_install below     (ref_count 1→2)
         * uart_driver_delete only decrements once (2→1), so the clock stays
         * on and the UART0 module keeps its internal pull-up active on GPIO17,
         * making gpio_get_level() always return 1.
         *
         * Fix: after the driver teardown, call periph_module_disable() once
         * more to drain the remaining ref_count from console init and truly
         * shut off the UART0 clock. */
        uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
        uart_driver_delete(UART_NUM_0);
        periph_module_disable(PERIPH_UART0_MODULE);
        gpio_reset_pin(PIN_ONEWIRE_RX);    /* GPIO16 — 1-Wire RX / UART0 TX */
        gpio_reset_pin(PIN_LD2410_INPUT);   /* GPIO17 — occupancy / UART0 RX */

        xTaskCreate(temp_task, "temp_task", 3072, NULL, 5, NULL);
        xTaskCreate(occ_task,  "occ_task",  2560, NULL, 6, NULL);
    }
}
