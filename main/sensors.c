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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_private/periph_ctrl.h"
#include "soc/periph_defs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "sensors";

/* Both Add-on tasks share one bus lock. 1-Wire bit timing is
 * microsecond-critical, so temp_task holds it for a whole transaction and
 * occ_task holds it around its 100 ms busy-sample window. Without the lock the
 * higher-priority occupancy sampling preempts every 1-Wire transaction, so no
 * DS18B20 read ever completes. */
static SemaphoreHandle_t s_addon_bus;

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

static const char *s_temp_err;

const char *sensors_temp_error(void)
{
    return s_temp_err ? s_temp_err : "no reading yet";
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

/* Interrupts stay off for the duration of a time slot: a single preemption
 * stretches the slot past the DS18B20's tolerance and corrupts the byte. */
static portMUX_TYPE s_ow_mux = portMUX_INITIALIZER_UNLOCKED;

static bool ow_reset(void)
{
    /* Wait until bus idle (RX=HIGH) */
    uint8_t retries = 125;
    do {
        if (--retries == 0) return false;
        esp_rom_delay_us(2);
    } while (!ow_rx_read());

    /* 480 µs reset pulse via TX */
    portENTER_CRITICAL(&s_ow_mux);
    ow_tx_low();
    esp_rom_delay_us(480);
    ow_tx_high();
    esp_rom_delay_us(70);
    bool present = !ow_rx_read();
    portEXIT_CRITICAL(&s_ow_mux);
    esp_rom_delay_us(410);
    return present;
}

static void ow_write_bit(int b)
{
    portENTER_CRITICAL(&s_ow_mux);
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
    portEXIT_CRITICAL(&s_ow_mux);
}

static int ow_read_bit(void)
{
    portENTER_CRITICAL(&s_ow_mux);
    ow_tx_low();
    esp_rom_delay_us(3);
    ow_tx_high();
    esp_rom_delay_us(9);
    int v = ow_rx_read();
    esp_rom_delay_us(53);
    portEXIT_CRITICAL(&s_ow_mux);
    return v;
}

/* Wiring check: the RX line must mirror what TX drives, because both sides of
 * the isolator sit on the same open-drain 1-Wire bus. RX staying high while TX
 * is low means our reset pulse never reaches the bus at all. */
static void ow_loopback_probe(int *lo_out, int *hi_out)
{
    ow_tx_low();
    esp_rom_delay_us(200);
    int lo = ow_rx_read();
    ow_tx_high();
    esp_rom_delay_us(200);
    int hi = ow_rx_read();
    ESP_LOGI(TAG, "1-Wire loopback: TX=0 -> RX=%d (expect 0), TX=1 -> RX=%d (expect 1)",
             lo, hi);
    if (lo_out) *lo_out = lo;
    if (hi_out) *hi_out = hi;
}

#define OW_SCAN_SAMPLES   60
#define OW_SCAN_STEP_US    5

/* Sample the bus for 300 us after a reset pulse instead of at the single 70 us
 * point, so a presence pulse that falls outside the standard window is still
 * visible. */
static void ow_presence_scan(int *first_us, int *last_us)
{
    int s[OW_SCAN_SAMPLES];

    portENTER_CRITICAL(&s_ow_mux);
    ow_tx_low();
    esp_rom_delay_us(480);
    ow_tx_high();
    for (int i = 0; i < OW_SCAN_SAMPLES; i++) {
        s[i] = ow_rx_read();
        esp_rom_delay_us(OW_SCAN_STEP_US);
    }
    portEXIT_CRITICAL(&s_ow_mux);
    esp_rom_delay_us(410);

    int first = -1, last = -1;
    for (int i = 0; i < OW_SCAN_SAMPLES; i++) {
        if (!s[i]) {
            if (first < 0) first = i;
            last = i;
        }
    }
    if (first < 0) {
        ESP_LOGW(TAG, "presence scan: RX stayed high for %d us after the reset pulse",
                 OW_SCAN_SAMPLES * OW_SCAN_STEP_US);
    } else {
        ESP_LOGW(TAG, "presence scan: RX low from %d us to %d us after the reset pulse",
                 first * OW_SCAN_STEP_US, (last + 1) * OW_SCAN_STEP_US);
    }
    if (first_us) *first_us = (first < 0) ? -1 : first * OW_SCAN_STEP_US;
    if (last_us)  *last_us  = (last  < 0) ? -1 : (last + 1) * OW_SCAN_STEP_US;
}

/* Who owns the pins: an internal pull-down that still reads high means
 * something drives RX actively (a peripheral that was not released), while
 * following the internal pulls means the line is floating and the Add-on bus
 * never reaches the pin. Reading TX back while driving it low shows whether
 * our own output actually takes effect. */
static void ow_pin_ownership_probe(int *rx_pd, int *rx_pu, int *tx_readback)
{
    gpio_config_t pd = {
        .pin_bit_mask = (1ULL << OW_RX),
        .mode         = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&pd);
    esp_rom_delay_us(200);
    int lvl_pd = ow_rx_read();

    gpio_config_t pu = {
        .pin_bit_mask = (1ULL << OW_RX),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&pu);
    esp_rom_delay_us(200);
    int lvl_pu = ow_rx_read();

    /* Restore the plain input the 1-Wire code expects. */
    gpio_config_t plain = {
        .pin_bit_mask = (1ULL << OW_RX),
        .mode         = GPIO_MODE_INPUT,
    };
    gpio_config(&plain);

    /* Output with the input buffer on, so gpio_get_level() reports the actual
     * pad level rather than the register we wrote. */
    gpio_config_t tx_io = {
        .pin_bit_mask = (1ULL << OW_TX),
        .mode         = GPIO_MODE_INPUT_OUTPUT,
    };
    gpio_config(&tx_io);
    ow_tx_low();
    esp_rom_delay_us(200);
    int tx_lvl = gpio_get_level(OW_TX);
    ow_tx_high();

    ESP_LOGI(TAG, "pin ownership: RX pull-down reads %d (0 = floating, 1 = driven high), "
                  "RX pull-up reads %d, TX reads %d while driven low (1 = pin held by something else)",
             lvl_pd, lvl_pu, tx_lvl);

    if (rx_pd)       *rx_pd = lvl_pd;
    if (rx_pu)       *rx_pu = lvl_pu;
    if (tx_readback) *tx_readback = tx_lvl;
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

static uint8_t ow_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        uint8_t b = data[i];
        for (int j = 0; j < 8; j++) {
            uint8_t mix = (crc ^ b) & 1;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            b >>= 1;
        }
    }
    return crc;
}

static bool ds18b20_read_centi_c(int16_t *out)
{
    if (!ow_reset()) {
        s_temp_err = "no presence pulse";
        return false;
    }
    ow_write_byte(0xCC);  /* Skip ROM */
    ow_write_byte(0x44);  /* Convert T */
    vTaskDelay(pdMS_TO_TICKS(800));  /* 12-bit max conversion */

    if (!ow_reset()) {
        s_temp_err = "no presence pulse after conversion";
        return false;
    }
    ow_write_byte(0xCC);
    ow_write_byte(0xBE);  /* Read Scratchpad */

    uint8_t sc[9];
    for (int i = 0; i < 9; i++) sc[i] = ow_read_byte();

    if (ow_crc8(sc, 8) != sc[8]) {
        ESP_LOGW(TAG, "scratchpad CRC mismatch: %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                 sc[0], sc[1], sc[2], sc[3], sc[4], sc[5], sc[6], sc[7], sc[8]);
        s_temp_err = "scratchpad CRC mismatch";
        return false;
    }

    int16_t raw = (int16_t)((sc[1] << 8) | sc[0]);
    if (raw == 0x0550) {
        s_temp_err = "85.00 C power-on default, conversion did not run";
        return false;
    }

    /* raw is in 1/16 °C. Convert to centi-°C:  raw * 100 / 16 */
    *out = (int16_t)(((int32_t)raw * 100) / 16);
    return true;
}

/* One transaction while holding the Add-on bus, so the occupancy sampling
 * cannot run in between, with a few retries for transient bus glitches. */
static bool ds18b20_read_locked(int16_t *out)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        xSemaphoreTake(s_addon_bus, portMAX_DELAY);
        bool ok = ds18b20_read_centi_c(out);
        xSemaphoreGive(s_addon_bus);
        if (ok) return true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
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

    ESP_LOGI(TAG, "1-Wire TX=GPIO%d RX=GPIO%d, RX idle level %d (expect 1)",
             OW_TX, OW_RX, ow_rx_read());
    ow_loopback_probe(NULL, NULL);

    while (1) {
        int16_t centi = 0;
        if (ds18b20_read_locked(&centi)) {
            s_last_temp_centi = centi;
            s_temp_valid = true;
            if (s_temp_cb) s_temp_cb(centi);
            ESP_LOGI(TAG, "temp = %d.%02d °C", centi / 100, abs(centi % 100));
        } else {
            s_temp_valid = false;
            ESP_LOGW(TAG, "DS18B20 read failed: %s (RX level %d)", s_temp_err, ow_rx_read());
            xSemaphoreTake(s_addon_bus, portMAX_DELAY);
            ow_loopback_probe(NULL, NULL);
            ow_presence_scan(NULL, NULL);
            xSemaphoreGive(s_addon_bus);
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
        xSemaphoreTake(s_addon_bus, portMAX_DELAY);
        int duty = measure_duty_pct();
        xSemaphoreGive(s_addon_bus);
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

        s_addon_bus = xSemaphoreCreateMutex();
        if (!s_addon_bus) {
            ESP_LOGE(TAG, "cannot create Add-on bus mutex — sensor tasks not started");
            return;
        }

        xTaskCreate(temp_task, "temp_task", 3072, NULL, 5, NULL);
        xTaskCreate(occ_task,  "occ_task",  2560, NULL, 5, NULL);
    }
}

/* ========================== On-demand 1-Wire diagnostics ================= */

size_t sensors_ow_probe(char *out, size_t out_size)
{
    if (!out || out_size == 0) return 0;
    out[0] = '\0';

    if (!hw_profile()->has_addon) {
        return snprintf(out, out_size, "no Add-on on this hardware profile\n");
    }
    if (g_bench_mode) {
        return snprintf(out, out_size,
                        "bench mode is ON: sensor tasks are not running and "
                        "GPIO%d/%d are left to UART0\n", OW_TX, OW_RX);
    }
    if (!s_addon_bus) {
        return snprintf(out, out_size, "Add-on bus mutex missing — sensor tasks never started\n");
    }

    int rx_pd = -1, rx_pu = -1, tx_rb = -1, lo = -1, hi = -1, first = -1, last = -1;
    int16_t centi = 0;
    bool read_ok;

    xSemaphoreTake(s_addon_bus, portMAX_DELAY);
    int idle = ow_rx_read();
    ow_pin_ownership_probe(&rx_pd, &rx_pu, &tx_rb);
    ow_loopback_probe(&lo, &hi);
    ow_presence_scan(&first, &last);
    read_ok = ds18b20_read_centi_c(&centi);
    xSemaphoreGive(s_addon_bus);

    size_t n = 0;
    n += snprintf(out + n, out_size - n, "TX=GPIO%d RX=GPIO%d, RX idle level %d (expect 1)\n",
                  OW_TX, OW_RX, idle);
    n += snprintf(out + n, out_size - n,
                  "pin ownership: RX with pull-down reads %d, with pull-up reads %d, "
                  "TX reads %d while driven low\n", rx_pd, rx_pu, tx_rb);
    n += snprintf(out + n, out_size - n,
                  "  %s\n",
                  rx_pd == 1 ? "RX is driven high by something else — not the Add-on bus"
                             : (rx_pu == 1 ? "RX follows the internal pulls — line floating, bus not reaching the pin"
                                           : "RX is held low"));
    n += snprintf(out + n, out_size - n,
                  "  %s\n",
                  tx_rb == 1 ? "TX does not go low when driven — pin held by something else"
                             : "TX goes low when driven");
    n += snprintf(out + n, out_size - n,
                  "loopback: TX=0 -> RX=%d (expect 0), TX=1 -> RX=%d (expect 1)\n", lo, hi);
    if (first < 0) {
        n += snprintf(out + n, out_size - n,
                      "presence scan: RX stayed high for %d us after the reset pulse\n",
                      OW_SCAN_SAMPLES * OW_SCAN_STEP_US);
    } else {
        n += snprintf(out + n, out_size - n,
                      "presence scan: RX low from %d us to %d us after the reset pulse\n",
                      first, last);
    }
    if (read_ok) {
        n += snprintf(out + n, out_size - n, "read: %d.%02d C\n", centi / 100, abs(centi % 100));
    } else {
        n += snprintf(out + n, out_size - n, "read failed: %s\n", sensors_temp_error());
    }
    return n;
}

static bool ow1_read_centi_c(int pin, int16_t *out, const char **err);

/* Candidate MCU pins for the Add-on data-out line. Excludes the SPI flash
 * (GPIO24-30), USB (GPIO12/13), the 1-Wire RX pin itself and whatever the
 * active hardware profile drives (relay/switch/button/LED). */
static const int s_scan_pins[] = { 0, 1, 2, 3, 6, 7, 8, 9, 11, 14, 17, 18, 19, 20, 21, 22, 23 };

static bool scan_pin_allowed(int pin)
{
    const hw_profile_t *p = hw_profile();
    if (pin == OW_RX) return false;
    if (pin == p->relay_gpio || pin == p->relay2_gpio) return false;
    if (pin == p->switch_gpio || pin == p->switch2_gpio) return false;
    if (pin == p->button_gpio || pin == p->led_gpio) return false;
    if (p->has_pm) {
        if (pin == p->pm_uart_tx || pin == p->pm_uart_rx) return false;
        if (pin == p->pm_i2c_sda || pin == p->pm_i2c_scl || pin == p->pm_i2c_irq) return false;
    }
    return true;
}

size_t sensors_ow_pin_scan(char *out, size_t out_size)
{
    if (!out || out_size == 0) return 0;
    out[0] = '\0';

    if (!hw_profile()->has_addon) {
        return snprintf(out, out_size, "no Add-on on this hardware profile\n");
    }
    if (g_bench_mode || !s_addon_bus) {
        return snprintf(out, out_size, "sensor tasks are not running (bench mode or no Add-on bus)\n");
    }

    size_t n = 0;
    n += snprintf(out + n, out_size - n,
                  "pulling each candidate pin low (open-drain) and watching RX=GPIO%d\n", OW_RX);

    xSemaphoreTake(s_addon_bus, portMAX_DELAY);
    int hits = 0;
    for (size_t i = 0; i < sizeof(s_scan_pins) / sizeof(s_scan_pins[0]); i++) {
        int pin = s_scan_pins[i];
        if (!scan_pin_allowed(pin)) continue;

        /* Open-drain so a pin that turns out to be driven high externally is
         * pulled down rather than fought with a push-pull output. */
        gpio_config_t od = {
            .pin_bit_mask = (1ULL << pin),
            .mode         = GPIO_MODE_OUTPUT_OD,
        };
        gpio_config(&od);
        gpio_set_level(pin, 1);
        esp_rom_delay_us(200);
        int before = ow_rx_read();
        gpio_set_level(pin, 0);
        esp_rom_delay_us(200);
        int during = ow_rx_read();
        gpio_set_level(pin, 1);
        esp_rom_delay_us(200);
        gpio_reset_pin(pin);

        if (before == 1 && during == 0) {
            hits++;
            n += snprintf(out + n, out_size - n, "  GPIO%d pulls RX low  <-- this is the data-out line\n", pin);
        }
    }

    /* Restore the pins the Add-on tasks own. */
    gpio_reset_pin(OW_RX);
    gpio_reset_pin(PIN_LD2410_INPUT);
    gpio_config_t rx_cfg = { .pin_bit_mask = (1ULL << OW_RX), .mode = GPIO_MODE_INPUT };
    gpio_config(&rx_cfg);
    gpio_config_t occ_cfg = { .pin_bit_mask = (1ULL << PIN_LD2410_INPUT), .mode = GPIO_MODE_INPUT };
    gpio_config(&occ_cfg);
    gpio_config_t tx_cfg = { .pin_bit_mask = (1ULL << OW_TX), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&tx_cfg);
    ow_tx_high();
    xSemaphoreGive(s_addon_bus);

    if (hits == 0) {
        n += snprintf(out + n, out_size - n,
                      "no pin changes RX: nothing on this MCU reaches the bus, so RX=GPIO%d is "
                      "driven by the Add-on side alone (isolator powered, bus not shared)\n", OW_RX);
    }

    /* Second theory: the line is not a split TX/RX pair at all but one
     * bidirectional open-drain wire on the RX pin. */
    int16_t centi = 0;
    const char *err = "unknown";
    xSemaphoreTake(s_addon_bus, portMAX_DELAY);
    bool single_ok = ow1_read_centi_c(OW_RX, &centi, &err);
    xSemaphoreGive(s_addon_bus);

    if (single_ok) {
        n += snprintf(out + n, out_size - n,
                      "single-pin 1-Wire on GPIO%d: %d.%02d C  <-- the Add-on is single-wire, "
                      "not a split TX/RX pair\n", OW_RX, centi / 100, abs(centi % 100));
    } else {
        n += snprintf(out + n, out_size - n,
                      "single-pin 1-Wire on GPIO%d: %s\n", OW_RX, err);
    }

    err = "unknown";
    xSemaphoreTake(s_addon_bus, portMAX_DELAY);
    single_ok = ow1_read_centi_c(OW_TX, &centi, &err);
    /* ow1_read_centi_c leaves the pin an input; TX must drive again. */
    gpio_config_t tx_out = { .pin_bit_mask = (1ULL << OW_TX), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&tx_out);
    ow_tx_high();
    xSemaphoreGive(s_addon_bus);

    if (single_ok) {
        n += snprintf(out + n, out_size - n,
                      "single-pin 1-Wire on GPIO%d: %d.%02d C  <-- the sensor sits on the TX pin\n",
                      OW_TX, centi / 100, abs(centi % 100));
    } else {
        n += snprintf(out + n, out_size - n,
                      "single-pin 1-Wire on GPIO%d: %s\n", OW_TX, err);
    }
    return n;
}

/* Classic single-pin 1-Wire on the RX line: some Add-on wiring uses one
 * bidirectional open-drain pin instead of the isolator's split TX/RX pair.
 * Worth one attempt before concluding the bus is dead. */
static bool ow1_reset(int pin)
{
    bool present;
    portENTER_CRITICAL(&s_ow_mux);
    gpio_set_level(pin, 0);
    esp_rom_delay_us(480);
    gpio_set_level(pin, 1);
    esp_rom_delay_us(70);
    present = (gpio_get_level(pin) == 0);
    portEXIT_CRITICAL(&s_ow_mux);
    esp_rom_delay_us(410);
    return present;
}

static void ow1_write_bit(int pin, int b)
{
    portENTER_CRITICAL(&s_ow_mux);
    gpio_set_level(pin, 0);
    esp_rom_delay_us(b ? 10 : 65);
    gpio_set_level(pin, 1);
    esp_rom_delay_us(b ? 55 : 5);
    portEXIT_CRITICAL(&s_ow_mux);
}

static void ow1_write_byte(int pin, uint8_t b)
{
    for (int i = 0; i < 8; i++) { ow1_write_bit(pin, b & 1); b >>= 1; }
}

static int ow1_read_bit(int pin)
{
    int v;
    portENTER_CRITICAL(&s_ow_mux);
    gpio_set_level(pin, 0);
    esp_rom_delay_us(3);
    gpio_set_level(pin, 1);
    esp_rom_delay_us(9);
    v = gpio_get_level(pin);
    esp_rom_delay_us(53);
    portEXIT_CRITICAL(&s_ow_mux);
    return v;
}

static uint8_t ow1_read_byte(int pin)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) v |= (ow1_read_bit(pin) << i);
    return v;
}

/* One full DS18B20 transaction over a single open-drain pin. */
static bool ow1_read_centi_c(int pin, int16_t *out, const char **err)
{
    gpio_config_t od = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&od);
    gpio_set_level(pin, 1);
    esp_rom_delay_us(1000);

    bool ok = false;
    if (!ow1_reset(pin)) {
        *err = "no presence pulse";
    } else {
        ow1_write_byte(pin, 0xCC);
        ow1_write_byte(pin, 0x44);
        vTaskDelay(pdMS_TO_TICKS(800));
        if (!ow1_reset(pin)) {
            *err = "no presence pulse after conversion";
        } else {
            ow1_write_byte(pin, 0xCC);
            ow1_write_byte(pin, 0xBE);
            uint8_t sc[9];
            for (int i = 0; i < 9; i++) sc[i] = ow1_read_byte(pin);
            if (ow_crc8(sc, 8) != sc[8]) {
                *err = "scratchpad CRC mismatch";
            } else {
                *out = (int16_t)(((int32_t)(int16_t)((sc[1] << 8) | sc[0]) * 100) / 16);
                ok = true;
            }
        }
    }

    /* Hand the pin back as a plain input for the split TX/RX code. */
    gpio_reset_pin(pin);
    gpio_config_t plain = { .pin_bit_mask = (1ULL << pin), .mode = GPIO_MODE_INPUT };
    gpio_config(&plain);
    return ok;
}
