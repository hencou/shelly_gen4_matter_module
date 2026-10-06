/*
 * Onboard NTC: NTC to ground with a 10 kΩ pull-up to 3.3 V on an ADC1 pin.
 *   R_ntc = 10k * V / (3.3 V - V)
 *   T     = 1 / (1/298.15 + ln(R_ntc / 10k) / B) - 273.15,  B = 3350
 */

#include "board_temp.h"
#include "hw_config.h"
#include "relay.h"
#include "matter_device.h"

#include <math.h>
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board_temp";

#define NTC_R_REF      10000.0f
#define NTC_R25        10000.0f
#define NTC_BETA       3350.0f
#define NTC_VSUPPLY_MV 3300
#define SAMPLE_PERIOD_MS 10000
#define SAMPLES        8
#define COOLDOWN_C     10.0f

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static volatile int32_t s_centi;
static volatile bool s_valid;
static volatile bool s_overheat;

static bool sample_mv(int *mv_out)
{
    int sum = 0, n = 0;
    for (int i = 0; i < SAMPLES; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, s_chan, &raw) != ESP_OK) continue;
        if (s_cali) {
            if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) continue;
        } else {
            mv = raw * NTC_VSUPPLY_MV / 4095;
        }
        sum += mv;
        n++;
    }
    if (n == 0) return false;
    *mv_out = sum / n;
    return true;
}

static bool mv_to_celsius(int mv, float *out)
{
    if (mv <= 0 || mv >= NTC_VSUPPLY_MV) return false;
    float r = NTC_R_REF * (float)mv / (float)(NTC_VSUPPLY_MV - mv);
    float inv_t = 1.0f / 298.15f + logf(r / NTC_R25) / NTC_BETA;
    *out = 1.0f / inv_t - 273.15f;
    return true;
}

static void protect(float t)
{
    static int over = 0;
    bool any_on = false;
    for (int ch = 0; ch < relay_channel_count(); ch++) {
        any_on |= relay_get_ch(ch);
    }

    over = (any_on && t > BOARD_TEMP_MAX_C) ? over + 1 : 0;
    if (over >= 2) {
        over = 0;
        s_overheat = true;
        ESP_LOGW(TAG, "board %.1f C > %.0f C: switching relays off", t, BOARD_TEMP_MAX_C);
        for (int ch = 0; ch < relay_channel_count(); ch++) {
            if (!relay_get_ch(ch)) continue;
            relay_set_ch(ch, false);
            matter_update_relay_onoff(ch, false);
        }
    } else if (s_overheat && t < BOARD_TEMP_MAX_C - COOLDOWN_C) {
        s_overheat = false;
        ESP_LOGI(TAG, "board cooled down to %.1f C", t);
    }
}

static void board_temp_task(void *arg)
{
    for (;;) {
        int mv;
        float t;
        if (sample_mv(&mv) && mv_to_celsius(mv, &t)) {
            s_centi = (int32_t)lroundf(t * 100.0f);
            s_valid = true;
            protect(t);
        } else {
            s_valid = false;
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

void board_temp_init(void)
{
    int gpio = hw_profile()->ntc_gpio;
    if (gpio < 0) return;

    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(gpio, &unit, &s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d is not an ADC pin", gpio);
        return;
    }
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed");
        return;
    }
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_config_channel(s_adc, s_chan, &ccfg) != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed");
        return;
    }
    adc_cali_curve_fitting_config_t cal = {
        .unit_id = unit, .chan = s_chan,
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) {
        s_cali = NULL;
        ESP_LOGW(TAG, "no ADC calibration, using raw scaling");
    }

    xTaskCreate(board_temp_task, "board_temp", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "NTC on GPIO%d (ADC%d ch%d), limit %.0f C", gpio, unit + 1, s_chan, BOARD_TEMP_MAX_C);
}

bool board_temp_read(float *out)
{
    if (!s_valid || !out) return false;
    *out = s_centi / 100.0f;
    return true;
}

bool board_temp_overheated(void)
{
    return s_overheat;
}
