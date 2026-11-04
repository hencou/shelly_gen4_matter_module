/*
 * ADE7953 dual-channel metering driver — see ade7953.h.
 *
 * ADE7953 register addressing encodes the register width in the address range:
 *   0x0xx = 8-bit, 0x1xx = 16-bit, 0x2xx = 24-bit, 0x3xx = 32-bit.
 * A read writes the 16-bit register address (MSB first) then reads the data
 * bytes (MSB first). We use the legacy i2c master helper for simple polled I/O.
 *
 * Scaling constants below are placeholders: the Shelly 2PM front-end (shunt,
 * divider, PGA gains) has NOT been verified here, so reported values must be
 * calibrated against a known load before they are trustworthy. The data path
 * (I2C -> decode -> Matter/dashboard) is complete; only the scale factors need
 * tuning. This driver is untested on hardware.
 */

#include "ade7953.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ade7953";

#define ADE_I2C_PORT   I2C_NUM_0
#define ADE_I2C_ADDR   0x38          /* 7-bit ADE7953 address */
#define ADE_I2C_HZ     400000
#define ADE_IO_TIMEOUT_MS 100
#define READ_INTERVAL_MS 1000

/* Registers (width implied by address range). */
#define REG_AWATT   0x212   /* 24-bit signed  active power ch A */
#define REG_BWATT   0x213   /* 24-bit signed  active power ch B */
#define REG_IRMSA   0x21A   /* 24-bit         current RMS  ch A */
#define REG_IRMSB   0x21B   /* 24-bit         current RMS  ch B */
#define REG_VRMS    0x21C   /* 24-bit         voltage RMS (shared) */
#define REG_PERIOD  0x10E   /* 16-bit         line period */
#define REG_CONFIG  0x120   /* 16-bit         config */
#define REG_UNLOCK  0x0FE   /* 8-bit          unlock 0x120 */

/* Placeholder scaling — CALIBRATE on hardware. Full-scale RMS count is
 * 9032007 for the 24-bit registers; without the real front-end values these
 * are only order-of-magnitude estimates. */
#define ADE_VREF   ((float)9032007.0f / 240.0f)   /* counts per volt   */
#define ADE_IREF   ((float)9032007.0f / 30.0f)    /* counts per amp    */
#define ADE_PREF   ((float)2000000.0f)            /* counts per watt   */
#define ADE_FCLK   223750.0f                      /* period counter clk */

static ade7953_cb_t s_cb;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static power_meter_reading_t s_last[2];
static bool s_have[2];

/* Per-channel RAM energy accumulation (Wh); resets on reboot. */
static double s_energy_wh[2];
static int64_t s_last_us[2];

static esp_err_t ade_read(uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    return i2c_master_transmit_receive(s_dev, addr, sizeof(addr), data, len,
                                       ADE_IO_TIMEOUT_MS);
}

static esp_err_t ade_write(uint16_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[6];
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)(reg & 0xFF);
    memcpy(&buf[2], data, len);
    return i2c_master_transmit(s_dev, buf, len + 2, ADE_IO_TIMEOUT_MS);
}

static bool read_u24(uint16_t reg, uint32_t *out)
{
    uint8_t b[3];
    if (ade_read(reg, b, 3) != ESP_OK) return false;
    *out = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
    return true;
}

static bool read_i24(uint16_t reg, int32_t *out)
{
    uint32_t v;
    if (!read_u24(reg, &v)) return false;
    if (v & 0x800000u) v |= 0xFF000000u;   /* sign-extend 24 -> 32 */
    *out = (int32_t)v;
    return true;
}

static bool read_u16(uint16_t reg, uint16_t *out)
{
    uint8_t b[2];
    if (ade_read(reg, b, 2) != ESP_OK) return false;
    *out = ((uint16_t)b[0] << 8) | b[1];
    return true;
}

static void accumulate_energy(int ch, float power_w)
{
    int64_t now = esp_timer_get_time();
    if (s_last_us[ch] != 0 && power_w > 0.0f) {
        double hours = (double)(now - s_last_us[ch]) / 1e6 / 3600.0;
        s_energy_wh[ch] += (double)power_w * hours;
    }
    s_last_us[ch] = now;
}

static void poll_once(void)
{
    uint32_t vraw = 0, ia = 0, ib = 0;
    int32_t  pa = 0, pb = 0;
    uint16_t period = 0;

    bool ok = read_u24(REG_VRMS, &vraw);
    ok = read_u24(REG_IRMSA, &ia) && ok;
    ok = read_u24(REG_IRMSB, &ib) && ok;
    ok = read_i24(REG_AWATT, &pa) && ok;
    ok = read_i24(REG_BWATT, &pb) && ok;
    read_u16(REG_PERIOD, &period);

    if (!ok) {
        ESP_LOGW(TAG, "I2C read failed");
        return;
    }

    float voltage = (float)vraw / ADE_VREF;
    float freq = period ? (ADE_FCLK / (float)(period + 1)) : 0.0f;

    power_meter_reading_t r[2] = {0};
    r[0].voltage_v = voltage;
    r[0].current_a = (float)ia / ADE_IREF;
    r[0].power_w   = (float)pa / ADE_PREF;
    r[1].voltage_v = voltage;
    r[1].current_a = (float)ib / ADE_IREF;
    r[1].power_w   = (float)pb / ADE_PREF;

    for (int ch = 0; ch < 2; ch++) {
        accumulate_energy(ch, r[ch].power_w);
        r[ch].energy_wh    = (float)s_energy_wh[ch];
        r[ch].frequency_hz = freq;
        r[ch].valid = true;
        s_last[ch] = r[ch];
        s_have[ch] = true;
    }

    ESP_LOGI(TAG, "U=%.1fV | A: I=%.3fA P=%.1fW | B: I=%.3fA P=%.1fW f=%.2fHz",
             voltage, r[0].current_a, r[0].power_w,
             r[1].current_a, r[1].power_w, freq);

    if (s_cb) s_cb(&r[0], &r[1]);
}

static void ade_task(void *arg)
{
    while (1) {
        poll_once();
        vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));
    }
}

esp_err_t ade7953_init(int sda, int scl, int irq, ade7953_cb_t cb)
{
    (void)irq;   /* polled mode — IRQ line not used */
    s_cb = cb;

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = ADE_I2C_PORT,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return err;
    }
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADE_I2C_ADDR,
        .scl_speed_hz = ADE_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device: %s", esp_err_to_name(err));
        i2c_del_master_bus(s_bus);
        return err;
    }

    /* Datasheet-mandated unlock of register 0x120 + optimum setting 0x30. */
    vTaskDelay(pdMS_TO_TICKS(100));
    uint8_t unlock = 0xAD;
    ade_write(REG_UNLOCK, &unlock, 1);
    uint8_t optimum[2] = { 0x00, 0x30 };
    ade_write(REG_CONFIG, optimum, 2);

    BaseType_t r = xTaskCreate(ade_task, "ade7953", 3584, NULL, 5, NULL);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "ade_task create failed");
        i2c_master_bus_rm_device(s_dev);
        i2c_del_master_bus(s_bus);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ADE7953 init on I2C SDA=GPIO%d SCL=GPIO%d", sda, scl);
    return ESP_OK;
}

bool ade7953_get(int ch, power_meter_reading_t *out)
{
    if (ch < 0 || ch > 1 || !out || !s_have[ch]) return false;
    *out = s_last[ch];
    return true;
}
