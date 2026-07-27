/*
 * HTTP API handlers + management dashboard.
 * Extracted from ota.c for maintainability.
 */

#include "web_api.h"
#include "ota.h"
#include "app_config.h"
#include "hw_config.h"
#include "power_meter.h"
#include "ade7953.h"
#include "relay.h"
#include "chip_temp.h"
#include "script_engine.h"
#include "dashboard_html.h"
#include "matter_device.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/temperature_sensor.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "lua.h"
#include "lauxlib.h"

#include "esp_private/periph_ctrl.h"
#include "soc/periph_defs.h"
#include "esp_partition.h"
#include "mbedtls/base64.h"


static const char *TAG = "web_api";

/* ---------- DS18B20 sensor read (dual-pin 1-Wire via ISO7221A) ---------- */

#define OTA_OW_TX  PIN_ONEWIRE_TX
#define OTA_OW_RX  PIN_ONEWIRE_RX

static inline void ota_ow_tx_low(void)  { gpio_set_level(OTA_OW_TX, 0); }
static inline void ota_ow_tx_high(void) { gpio_set_level(OTA_OW_TX, 1); }
static inline int  ota_ow_rx_rd(void)   { return gpio_get_level(OTA_OW_RX); }

static bool ota_ow_reset(void)
{
    uint8_t retries = 125;
    do {
        if (--retries == 0) return false;
        esp_rom_delay_us(2);
    } while (!ota_ow_rx_rd());

    ota_ow_tx_low();  esp_rom_delay_us(480);
    ota_ow_tx_high(); esp_rom_delay_us(70);
    bool present = !ota_ow_rx_rd();
    esp_rom_delay_us(410);
    return present;
}

static void ota_ow_write_bit(int b)
{
    ota_ow_tx_low();
    if (b) { esp_rom_delay_us(10); ota_ow_tx_high(); esp_rom_delay_us(55); }
    else   { esp_rom_delay_us(65); ota_ow_tx_high(); esp_rom_delay_us(5);  }
}

static int ota_ow_read_bit(void)
{
    ota_ow_tx_low();  esp_rom_delay_us(3);
    ota_ow_tx_high(); esp_rom_delay_us(9);
    int v = ota_ow_rx_rd();
    esp_rom_delay_us(53);
    return v;
}

static void ota_ow_write_byte(uint8_t b)
{
    for (int i = 0; i < 8; i++) { ota_ow_write_bit(b & 1); b >>= 1; }
}

static uint8_t ota_ow_read_byte(void)
{
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) v |= (ota_ow_read_bit() << i);
    return v;
}

/* ---------- HTTP handlers ---------- */

static esp_err_t form_get(httpd_req_t *req)
{
    return httpd_resp_send(req, MGMT_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t api_settings_get(httpd_req_t *req)
{
    char ssid[33] = {0}, pass[65] = {0}, url[256] = {0};
    bool have = ota_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass),
                                     url, sizeof(url));
#ifdef DEFAULT_WIFI_SSID
    if (!have) {
        strncpy(ssid, DEFAULT_WIFI_SSID, sizeof(ssid) - 1);
#ifdef DEFAULT_WIFI_PASS
        strncpy(pass, DEFAULT_WIFI_PASS, sizeof(pass) - 1);
#endif
#ifdef DEFAULT_OTA_URL
        strncpy(url, DEFAULT_OTA_URL, sizeof(url) - 1);
#endif
    }
#endif
    (void)have;
    static char json[512];
    snprintf(json, sizeof(json),
        "{\"ssid\":\"%s\",\"pass\":\"%s\",\"url\":\"%s\",\"hostname\":\"%s\",\"version\":\"%s\"}",
        ssid, pass, url, ota_hostname_get(), FW_VERSION);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static const char *reset_reason_str(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:  return "Power-on";
        case ESP_RST_SW:      return "Software";
        case ESP_RST_PANIC:   return "Panic/exception";
        case ESP_RST_INT_WDT: return "Interrupt watchdog";
        case ESP_RST_TASK_WDT:return "Task watchdog";
        case ESP_RST_WDT:     return "Other watchdog";
        case ESP_RST_DEEPSLEEP:return "Deep sleep";
        case ESP_RST_BROWNOUT:return "Brownout";
        case ESP_RST_SDIO:    return "SDIO";
        default:              return "Unknown";
    }
}

static esp_err_t api_hardware_get(httpd_req_t *req)
{
    static char json[1536];
    int pos = 0;

    const esp_app_desc_t *app = esp_app_get_description();

    esp_chip_info_t ci;
    esp_chip_info(&ci);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    wifi_mode_t wmode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&wmode);
    const char *wmode_str = (wmode == WIFI_MODE_STA) ? "STA" :
                            (wmode == WIFI_MODE_AP)  ? "SoftAP" :
                            (wmode == WIFI_MODE_APSTA) ? "APSTA" : "N/A";
    char rssi_str[24] = "N/A";
    if (wmode == WIFI_MODE_STA || wmode == WIFI_MODE_APSTA) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            snprintf(rssi_str, sizeof(rssi_str), "%d dBm", ap.rssi);
        }
    }

    int64_t up_us = esp_timer_get_time();
    int up_s  = (int)(up_us / 1000000);
    int up_h  = up_s / 3600;
    int up_m  = (up_s % 3600) / 60;
    int up_ss = up_s % 60;

    uint32_t heap = esp_get_free_heap_size();

    char ctemp_str[32] = "N/A";
    {
        float t;
        if (chip_temp_read(&t)) {
            snprintf(ctemp_str, sizeof(ctemp_str), "%.1f C", t);
        }
    }

    const char *rst = reset_reason_str(esp_reset_reason());

    const hw_profile_t *hw = hw_profile();

    int btn_level = gpio_get_level(hw->switch_gpio);
    int btn_active = g_bench_mode ? !btn_level : btn_level;

    int pcb_level = gpio_get_level(hw->button_gpio);
    int pcb_active = !pcb_level;

    int dig_level = hw->has_addon ? gpio_get_level(PIN_TOUCH_INPUT) : 0;

    char ana_str[32];
    char temp_str[32] = "N/A (bench mode)";
    if (!hw->has_addon) {
        snprintf(ana_str, sizeof(ana_str), "N/A (no Add-on)");
        snprintf(temp_str, sizeof(temp_str), "N/A (no Add-on)");
    } else if (g_bench_mode) {
        snprintf(ana_str, sizeof(ana_str), "N/A (bench mode)");
    } else {
        {
            uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
            uart_driver_delete(UART_NUM_0);
            periph_module_disable(PERIPH_UART0_MODULE);
            gpio_reset_pin(PIN_LD2410_INPUT);
            gpio_config_t cfg = {
                .pin_bit_mask = (1ULL << PIN_LD2410_INPUT),
                .mode         = GPIO_MODE_INPUT,
                .pull_down_en = GPIO_PULLDOWN_ENABLE,
            };
            gpio_config(&cfg);
            int high = 0, total = 0;
            for (int us = 0; us < 100000; us += 100) {
                if (gpio_get_level(PIN_LD2410_INPUT)) high++;
                total++;
                esp_rom_delay_us(100);
            }
            int duty = (high * 100) / total;
            snprintf(ana_str, sizeof(ana_str), "%d%% duty (%s)",
                     duty, duty >= 25 ? "occupied" : "clear");
        }

        strcpy(temp_str, "sensor not found");
        {
            uart_driver_delete(UART_NUM_0);
            gpio_config_t tx_cfg = {
                .pin_bit_mask = (1ULL << OTA_OW_TX),
                .mode         = GPIO_MODE_OUTPUT,
            };
            gpio_config(&tx_cfg);
            ota_ow_tx_high();
            gpio_reset_pin(OTA_OW_RX);
            gpio_config_t rx_cfg = {
                .pin_bit_mask = (1ULL << OTA_OW_RX),
                .mode         = GPIO_MODE_INPUT,
            };
            gpio_config(&rx_cfg);

            if (ota_ow_reset()) {
                ota_ow_write_byte(0xCC);
                ota_ow_write_byte(0x44);
                vTaskDelay(pdMS_TO_TICKS(820));
                if (ota_ow_reset()) {
                    ota_ow_write_byte(0xCC);
                    ota_ow_write_byte(0xBE);
                    uint8_t sc[9];
                    for (int i = 0; i < 9; i++) sc[i] = ota_ow_read_byte();
                    int16_t raw = (int16_t)((sc[1] << 8) | sc[0]);
                    int16_t centi = (int16_t)(((int32_t)raw * 100) / 16);
                    int deg  = centi / 100;
                    int frac = centi < 0 ? -(centi % 100) : (centi % 100);
                    if (frac < 0) frac = -frac;
                    if (raw != 0x0550 && centi > -5500 && centi < 12500) {
                        snprintf(temp_str, sizeof(temp_str), "%d.%02d C", deg, frac);
                    } else if (raw == 0x0550) {
                        snprintf(temp_str, sizeof(temp_str), "85.00 C (power-on default)");
                    } else {
                        snprintf(temp_str, sizeof(temp_str), "error (%d.%02d C)", deg, frac);
                    }
                }
            }
        }
    }

    char dig_str[40];
    if (hw->has_addon) {
        snprintf(dig_str, sizeof(dig_str), "%s (GPIO%d=%d)",
                 dig_level ? "HIGH" : "LOW", PIN_TOUCH_INPUT, dig_level);
    } else {
        snprintf(dig_str, sizeof(dig_str), "N/A (no Add-on)");
    }

    char power_str[200];
    if (hw->pm_type == PM_ADE7953) {
        power_meter_reading_t a, b;
        bool ha = ade7953_get(0, &a), hb = ade7953_get(1, &b);
        if (ha || hb) {
            snprintf(power_str, sizeof(power_str),
                     "A: %.1f V, %.3f A, %.1f W, %.1f Wh | B: %.3f A, %.1f W, %.1f Wh (%.2f Hz)",
                     a.voltage_v, a.current_a, a.power_w, a.energy_wh,
                     b.current_a, b.power_w, b.energy_wh, a.frequency_hz);
        } else {
            snprintf(power_str, sizeof(power_str), "waiting for ADE7953...");
        }
    } else if (hw->pm_type == PM_BL0942) {
        power_meter_reading_t pm;
        if (power_meter_get(&pm)) {
            snprintf(power_str, sizeof(power_str),
                     "%.1f V, %.3f A, %.1f W, %.1f Wh, %.2f Hz",
                     pm.voltage_v, pm.current_a, pm.power_w, pm.energy_wh,
                     pm.frequency_hz);
        } else {
            snprintf(power_str, sizeof(power_str), "waiting for BL0942...");
        }
    } else {
        snprintf(power_str, sizeof(power_str), "N/A (no power meter)");
    }

    /* Relay + 2nd switch state (2nd channel only present on 2PM). */
    char relay_str[48];
    if (relay_channel_count() >= 2) {
        snprintf(relay_str, sizeof(relay_str), "ch1=%s, ch2=%s",
                 relay_get_ch(0) ? "ON" : "OFF", relay_get_ch(1) ? "ON" : "OFF");
    } else {
        snprintf(relay_str, sizeof(relay_str), "%s", relay_get_ch(0) ? "ON" : "OFF");
    }

    char switch2_str[40];
    if (hw->switch2_gpio >= 0) {
        int s2 = gpio_get_level(hw->switch2_gpio);
        snprintf(switch2_str, sizeof(switch2_str), "%s (GPIO%d=%d)",
                 (g_bench_mode ? !s2 : s2) ? "PRESSED" : "RELEASED",
                 hw->switch2_gpio, s2);
    } else {
        snprintf(switch2_str, sizeof(switch2_str), "N/A");
    }

    pos = snprintf(json, sizeof(json),
        "{\"firmware\":\"%s %s (%s %s)\","
        "\"device_type\":\"%s\","
        "\"device_type_id\":%d,"
        "\"chip\":\"ESP32-C6 rev %d, %d core(s)\","
        "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
        "\"wifi_mode\":\"%s\","
        "\"wifi_rssi\":\"%s\","
        "\"uptime\":\"%dh %02dm %02ds\","
        "\"free_heap\":\"%lu bytes\","
        "\"chip_temp\":\"%s\","
        "\"reset_reason\":\"%s\","
        "\"bench_mode\":\"%s\","
        "\"srp_mode\":\"%s\","
        "\"pushbutton\":\"%s (GPIO%d=%d)\","
        "\"pcb_button\":\"%s (GPIO%d=%d)\","
        "\"switch2\":\"%s\","
        "\"relay\":\"%s\","
        "\"digital_in\":\"%s\","
        "\"analog_in\":\"%s\","
        "\"temperature\":\"%s\","
        "\"power\":\"%s\"}",
        FW_VERSION, app->version, app->date, app->time,
        hw->name, (int)hw->type,
        ci.revision, ci.cores,
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
        wmode_str,
        rssi_str,
        up_h, up_m, up_ss,
        (unsigned long)heap,
        ctemp_str,
        rst,
        g_bench_mode ? "ON" : "OFF",
        ota_srp_mode_get() ? "ON" : "OFF",
        btn_active ? "PRESSED" : "RELEASED", hw->switch_gpio, btn_level,
        pcb_active ? "PRESSED" : "RELEASED", hw->button_gpio, pcb_level,
        switch2_str,
        relay_str,
        dig_str,
        ana_str,
        temp_str,
        power_str);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, pos);
}

static esp_err_t api_bench_mode_post(httpd_req_t *req)
{
    int new_val = g_bench_mode ? 0 : 1;
    ota_bench_mode_save(new_val);
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t api_srp_mode_post(httpd_req_t *req)
{
    bool new_val = !ota_srp_mode_get();
    ota_srp_mode_set(new_val);
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t api_hw_config_get(httpd_req_t *req)
{
    char json[512];
    int pos = 0;
    const hw_profile_t *cur = hw_profile();

    pos += snprintf(json + pos, sizeof(json) - pos,
                    "{\"selected\":%d,\"types\":[", (int)cur->type);
    for (int i = 0; i < HW_TYPE_COUNT; i++) {
        const hw_profile_t *p = hw_profile_for((hw_device_type_t)i);
        pos += snprintf(json + pos, sizeof(json) - pos,
                        "%s{\"id\":%d,\"name\":\"%s\",\"pm\":%s,\"addon\":%s}",
                        i ? "," : "", i, p->name,
                        p->has_pm ? "true" : "false",
                        p->has_addon ? "true" : "false");
    }
    pos += snprintf(json + pos, sizeof(json) - pos, "]}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, pos);
}

static esp_err_t api_hw_config_post(httpd_req_t *req)
{
    char body[128] = { 0 };
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }
    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsNumber(type)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing type");
        return ESP_FAIL;
    }
    int t = type->valueint;
    cJSON_Delete(root);

    if (t < 0 || t >= HW_TYPE_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "type out of range");
        return ESP_FAIL;
    }

    esp_err_t err = hw_device_type_set((hw_device_type_t)t);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t api_restart_post(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t api_factory_reset_post(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "OK");
    ESP_LOGW(TAG, "Factory reset via web: wiping nvs");
    nvs_flash_deinit();
    nvs_flash_erase();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static void erase_nvs_namespace(const char *ns)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

static esp_err_t api_commission_post(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "OK");
    ESP_LOGW(TAG, "Commission mode: removing Matter fabrics and rebooting into BLE pairing");
    /* Delete fabrics via the CHIP FabricTable API — partition-agnostic, so it
     * works whether the KVS lives in the "nvs" partition or a dedicated
     * "chip_kvs" partition (old-layout devices). */
    matter_delete_all_fabrics();
    /* Also wipe the residual Matter NVS namespaces so no stale config/counters
     * survive — keeps WiFi/scripts (in the "ota" namespace) untouched. */
    erase_nvs_namespace("chip-kvs");
    erase_nvs_namespace("chip-config");
    erase_nvs_namespace("chip-counters");
    /* Signal smart boot to stay in BLE commissioning mode after reboot
     * instead of falling back to WiFi setup mode. */
    ota_commission_pending_set(true);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* Helper: send a JSON-escaped string via chunked response */
static void send_json_escaped(httpd_req_t *req, const char *s)
{
    char buf[128];
    int pos = 0;
    for (; *s; s++) {
        if (pos > (int)sizeof(buf) - 8) {
            httpd_resp_send_chunk(req, buf, pos);
            pos = 0;
        }
        if (*s == '"' || *s == '\\') { buf[pos++] = '\\'; buf[pos++] = *s; }
        else if (*s == '\n') { buf[pos++] = '\\'; buf[pos++] = 'n'; }
        else if (*s == '\r') { buf[pos++] = '\\'; buf[pos++] = 'r'; }
        else if (*s == '\t') { buf[pos++] = '\\'; buf[pos++] = 't'; }
        else buf[pos++] = *s;
    }
    if (pos > 0) httpd_resp_send_chunk(req, buf, pos);
}

static esp_err_t api_backup_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=\"shelly1gen4_backup.json\"");

    char ssid[33] = {0}, pass[65] = {0}, url[256] = {0};
    ota_load_credentials(ssid, sizeof(ssid), pass, sizeof(pass),
                         url, sizeof(url));

    char hdr[512];
    snprintf(hdr, sizeof(hdr),
        "{\"version\":4,"
        "\"ota\":{\"ssid\":\"%s\",\"pass\":\"%s\",\"url\":\"%s\","
        "\"hostname\":\"%s\","
        "\"srp_mode\":%s},"
        "\"scripts\":[",
        ssid, pass, url, ota_hostname_get(),
        ota_srp_mode_get() ? "true" : "false");
    httpd_resp_send_chunk(req, hdr, strlen(hdr));

    for (int i = 0; i < SCRIPT_MAX_SLOTS; i++) {
        script_slot_config_t cfg;
        script_slot_get((uint8_t)i, &cfg);

        char slot_hdr[128];
        snprintf(slot_hdr, sizeof(slot_hdr),
            "%s{\"slot\":%d,\"type\":%d,\"trigger\":%d,\"period_ms\":%u,\"name\":\"",
            (i > 0) ? "," : "", i, (int)cfg.type, (int)cfg.trigger, cfg.period_ms);
        httpd_resp_send_chunk(req, slot_hdr, strlen(slot_hdr));

        send_json_escaped(req, cfg.name);
        httpd_resp_send_chunk(req, "\",\"script\":\"", 12);
        send_json_escaped(req, cfg.script);
        httpd_resp_send_chunk(req, "\"}", 2);
    }

    httpd_resp_send_chunk(req, "]", 1);

    /* Optionally include raw NVS partition dump when ?nvs=1 is passed */
    char qbuf[16] = {0};
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK &&
        strstr(qbuf, "nvs=1")) {
        const esp_partition_t *nvs_part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
        if (nvs_part) {
            httpd_resp_send_chunk(req, ",\"nvs\":\"", 8);
            uint8_t rbuf[768];
            char b64buf[1024 + 4];
            size_t olen = 0;
            for (size_t off = 0; off < nvs_part->size; off += sizeof(rbuf)) {
                size_t chunk = sizeof(rbuf);
                if (off + chunk > nvs_part->size) chunk = nvs_part->size - off;
                if (esp_partition_read(nvs_part, off, rbuf, chunk) == ESP_OK) {
                    mbedtls_base64_encode((unsigned char *)b64buf, sizeof(b64buf),
                                          &olen, rbuf, chunk);
                    if (olen > 0) httpd_resp_send_chunk(req, b64buf, olen);
                }
            }
            httpd_resp_send_chunk(req, "\"", 1);
        }
    }

    httpd_resp_send_chunk(req, "}", 1);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* /api/restore — import JSON backup (WiFi + scripts) using cJSON */
static esp_err_t api_restore_post(httpd_req_t *req)
{
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 128 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large or empty");
        return ESP_FAIL;
    }
    /* Stream the request body. The NVS base64 blob (up to ~85 KB) is decoded
     * straight to the flash partition as it arrives, so we never hold the whole
     * body in RAM — previously a single ~90 KB allocation that often OOMed on
     * the ~200 KB heap while the Matter stack was running. Only the small header
     * JSON (ota + scripts) is buffered for cJSON. The "nvs" field is emitted last
     * by /api/backup, so the buffered header stays small. */
    static const char MARKER[] = "\"nvs\":\"";
    const size_t MARKER_LEN = sizeof(MARKER) - 1;

    size_t hdr_cap = 4096, hdr_len = 0;
    char *hdr = (char *)malloc(hdr_cap);
    if (!hdr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    enum { PH_HEADER, PH_NVS, PH_TAIL } phase = PH_HEADER;
    const esp_partition_t *nvs_part = NULL;
    size_t nvs_dst = 0;
    bool nvs_active = false, nvs_ok = true, failed = false;
    char b64buf[1024];            /* multiple of 4 → base64 chunks stay aligned */
    size_t b64n = 0;

    char rbuf[1024];
    int received = 0;

    while (received < content_len && !failed) {
        int n = httpd_req_recv(req, rbuf, sizeof(rbuf));
        if (n <= 0) { failed = true; break; }
        received += n;

        for (int i = 0; i < n && !failed; i++) {
            char c = rbuf[i];

            if (phase == PH_HEADER || phase == PH_TAIL) {
                if (hdr_len + 1 >= hdr_cap) {
                    char *nh = (char *)realloc(hdr, hdr_cap * 2);
                    if (!nh) { failed = true; break; }
                    hdr = nh; hdr_cap *= 2;
                }
                hdr[hdr_len++] = c;

                if (phase == PH_HEADER && hdr_len >= MARKER_LEN &&
                    memcmp(hdr + hdr_len - MARKER_LEN, MARKER, MARKER_LEN) == 0) {
                    hdr_len -= MARKER_LEN;                              /* drop marker */
                    if (hdr_len > 0 && hdr[hdr_len - 1] == ',') hdr_len--; /* drop separator */
                    nvs_part = esp_partition_find_first(
                        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
                    if (nvs_part) {
                        nvs_flash_deinit();
                        esp_partition_erase_range(nvs_part, 0, nvs_part->size);
                        nvs_active = true;
                    }
                    phase = PH_NVS;
                }
            } else { /* PH_NVS: base64 value, decode straight to flash */
                if (c == '"') {
                    if (nvs_active && b64n > 0) {
                        uint8_t dec[768]; size_t olen = 0;
                        if (mbedtls_base64_decode(dec, sizeof(dec), &olen,
                                (const unsigned char *)b64buf, b64n) == 0 &&
                            olen > 0 && nvs_dst + olen <= nvs_part->size) {
                            esp_partition_write(nvs_part, nvs_dst, dec, olen);
                            nvs_dst += olen;
                        } else {
                            nvs_ok = false;
                        }
                    }
                    b64n = 0;
                    phase = PH_TAIL;
                } else {
                    b64buf[b64n++] = c;
                    if (b64n == sizeof(b64buf)) {
                        if (nvs_active) {
                            uint8_t dec[768]; size_t olen = 0;
                            if (mbedtls_base64_decode(dec, sizeof(dec), &olen,
                                    (const unsigned char *)b64buf, b64n) == 0 &&
                                olen > 0 && nvs_dst + olen <= nvs_part->size) {
                                esp_partition_write(nvs_part, nvs_dst, dec, olen);
                                nvs_dst += olen;
                            } else {
                                nvs_ok = false;
                            }
                        }
                        b64n = 0;
                    }
                }
            }
        }
    }

    if (nvs_active) {
        nvs_flash_init();
        if (nvs_ok && nvs_dst > 0)
            ESP_LOGI(TAG, "restore: NVS written (%u bytes)", (unsigned)nvs_dst);
        else if (!nvs_ok)
            ESP_LOGW(TAG, "restore: NVS decode error after %u bytes", (unsigned)nvs_dst);
    }

    if (failed) {
        free(hdr);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
        return ESP_FAIL;
    }

    hdr[hdr_len] = '\0';
    cJSON *root = cJSON_Parse(hdr);
    free(hdr);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    /* Restore WiFi credentials */
    cJSON *ota = cJSON_GetObjectItem(root, "ota");
    if (ota) {
        cJSON *j_ssid = cJSON_GetObjectItem(ota, "ssid");
        cJSON *j_pass = cJSON_GetObjectItem(ota, "pass");
        cJSON *j_url  = cJSON_GetObjectItem(ota, "url");
        if (j_ssid && cJSON_IsString(j_ssid) && j_ssid->valuestring[0]) {
            ota_save_credentials(
                j_ssid->valuestring,
                (j_pass && cJSON_IsString(j_pass)) ? j_pass->valuestring : "",
                (j_url  && cJSON_IsString(j_url))  ? j_url->valuestring  : "");
        }

        cJSON *j_srp = cJSON_GetObjectItem(ota, "srp_mode");
        if (j_srp && cJSON_IsBool(j_srp)) {
            ota_srp_mode_set(cJSON_IsTrue(j_srp));
        }
        cJSON *j_host = cJSON_GetObjectItem(ota, "hostname");
        if (j_host && cJSON_IsString(j_host) && j_host->valuestring[0]) {
            ota_hostname_set(j_host->valuestring);
        }
    }

    /* Restore scripts */
    cJSON *scripts = cJSON_GetObjectItem(root, "scripts");
    if (scripts && cJSON_IsArray(scripts)) {
        cJSON *item;
        cJSON_ArrayForEach(item, scripts) {
            cJSON *j_slot = cJSON_GetObjectItem(item, "slot");
            if (!j_slot || !cJSON_IsNumber(j_slot)) continue;
            int slot = j_slot->valueint;
            if (slot < 0 || slot >= SCRIPT_MAX_SLOTS) continue;

            script_slot_config_t cfg;
            memset(&cfg, 0, sizeof(cfg));

            cJSON *j_type = cJSON_GetObjectItem(item, "type");
            if (j_type && cJSON_IsNumber(j_type))
                cfg.type = (script_slot_type_t)j_type->valueint;

            cJSON *j_trig = cJSON_GetObjectItem(item, "trigger");
            if (j_trig && cJSON_IsNumber(j_trig))
                cfg.trigger = (script_trigger_t)j_trig->valueint;

            cJSON *j_period = cJSON_GetObjectItem(item, "period_ms");
            if (j_period && cJSON_IsNumber(j_period))
                cfg.period_ms = (uint16_t)j_period->valueint;

            cJSON *j_name = cJSON_GetObjectItem(item, "name");
            if (j_name && cJSON_IsString(j_name))
                strncpy(cfg.name, j_name->valuestring, SCRIPT_NAME_LEN - 1);

            cJSON *j_script = cJSON_GetObjectItem(item, "script");
            if (j_script && cJSON_IsString(j_script))
                strncpy(cfg.script, j_script->valuestring, SCRIPT_MAX_SIZE - 1);

            if (cfg.type != 0 || cfg.name[0] || cfg.script[0]) {
                script_slot_set((uint8_t)slot, &cfg);
                ESP_LOGI(TAG, "restore: slot %d '%s' type=%d", slot, cfg.name, cfg.type);
            }
        }
    }

    cJSON_Delete(root);
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* /upload — receives raw .bin, writes directly to OTA partition */
static esp_err_t upload_post(httpd_req_t *req)
{
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "No OTA partition found");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA begin failed");
        return ESP_FAIL;
    }

    char buf[1024];
    int remaining = req->content_len;
    int total     = remaining;
    ESP_LOGI(TAG, "upload start: %d bytes to partition %s",
             total, update_part->label);

    while (remaining > 0) {
        int to_read = (remaining < (int)sizeof(buf)) ? remaining : (int)sizeof(buf);
        int recv = httpd_req_recv(req, buf, to_read);
        if (recv < 0) {
            ESP_LOGE(TAG, "httpd_req_recv error: %d", recv);
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Receive failed");
            return ESP_FAIL;
        }
        err = esp_ota_write(ota_handle, buf, recv);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Flash write failed");
            return ESP_FAIL;
        }
        remaining -= recv;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "OTA verification failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Setting boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "upload succeeded (%d bytes), rebooting", total);
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

static esp_err_t url_decode(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '+') { *w++ = ' '; r++; }
        else if (*r == '%' && r[1] && r[2]) {
            char h[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(h, NULL, 16); r += 3;
        } else *w++ = *r++;
    }
    *w = 0; return ESP_OK;
}

static bool form_field(const char *body, const char *key, char *out, size_t outlen)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(body, needle);
    if (!p) return false;
    p += strlen(needle);
    const char *e = strchr(p, '&');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, p, n); out[n] = 0;
    url_decode(out);
    return true;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    char body[512] = { 0 };
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no body");
        return ESP_FAIL;
    }
    char ssid[33] = {0}, pass[65] = {0}, url[256] = {0};
    char hostname[32] = {0};
    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "pass", pass, sizeof(pass));
    form_field(body, "url",  url,  sizeof(url));
    form_field(body, "hostname", hostname, sizeof(hostname));
    if (!ssid[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_FAIL;
    }
    ota_save_credentials(ssid, pass, url);
    if (hostname[0]) ota_hostname_set(hostname);
    const char *msg = "Saved. Device restarting into OTA mode on your WiFi...\n";
    httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    ota_request_ota_reboot();
    return ESP_OK;
}

/* ---------- Script API handlers (with Lua syntax validation) ---------- */

static esp_err_t api_script_get(httpd_req_t *req)
{
    char query[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing slot param");
        return ESP_FAIL;
    }
    char val[4] = {0};
    httpd_query_key_value(query, "slot", val, sizeof(val));
    int slot = atoi(val);
    if (slot < 0 || slot >= SCRIPT_MAX_SLOTS) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot");
        return ESP_FAIL;
    }

    script_slot_config_t cfg;
    script_slot_get((uint8_t)slot, &cfg);

    static char json[3072];
    int pos = 0;
    pos += snprintf(json + pos, sizeof(json) - pos,
        "{\"slot\":%d,\"type\":%d,\"trigger\":%d,\"period_ms\":%u,\"name\":\"",
        slot, (int)cfg.type, (int)cfg.trigger, cfg.period_ms);

    for (const char *p = cfg.name; *p && pos < (int)sizeof(json) - 10; p++) {
        if (*p == '"' || *p == '\\') json[pos++] = '\\';
        json[pos++] = *p;
    }
    pos += snprintf(json + pos, sizeof(json) - pos, "\",\"script\":\"");

    for (const char *p = cfg.script; *p && pos < (int)sizeof(json) - 10; p++) {
        if (*p == '"' || *p == '\\') { json[pos++] = '\\'; json[pos++] = *p; }
        else if (*p == '\n') { json[pos++] = '\\'; json[pos++] = 'n'; }
        else if (*p == '\r') { json[pos++] = '\\'; json[pos++] = 'r'; }
        else if (*p == '\t') { json[pos++] = '\\'; json[pos++] = 't'; }
        else json[pos++] = *p;
    }
    pos += snprintf(json + pos, sizeof(json) - pos, "\"}");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, pos);
}

/* POST /api/script — save slot config from JSON body (with cJSON + Lua syntax check) */
static esp_err_t api_script_post(httpd_req_t *req)
{
    static char body[3072];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
        return ESP_FAIL;
    }

    script_slot_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cJSON *j_slot = cJSON_GetObjectItem(root, "slot");
    int slot = (j_slot && cJSON_IsNumber(j_slot)) ? j_slot->valueint : -1;
    if (slot < 0 || slot >= SCRIPT_MAX_SLOTS) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot");
        return ESP_FAIL;
    }

    cJSON *j_type = cJSON_GetObjectItem(root, "type");
    if (j_type && cJSON_IsNumber(j_type))
        cfg.type = (script_slot_type_t)j_type->valueint;

    cJSON *j_trig = cJSON_GetObjectItem(root, "trigger");
    if (j_trig && cJSON_IsNumber(j_trig))
        cfg.trigger = (script_trigger_t)j_trig->valueint;

    cJSON *j_period = cJSON_GetObjectItem(root, "period_ms");
    if (j_period && cJSON_IsNumber(j_period))
        cfg.period_ms = (uint16_t)j_period->valueint;

    cJSON *j_name = cJSON_GetObjectItem(root, "name");
    if (j_name && cJSON_IsString(j_name))
        strncpy(cfg.name, j_name->valuestring, SCRIPT_NAME_LEN - 1);

    cJSON *j_script = cJSON_GetObjectItem(root, "script");
    if (j_script && cJSON_IsString(j_script))
        strncpy(cfg.script, j_script->valuestring, SCRIPT_MAX_SIZE - 1);

    cJSON_Delete(root);

    /* Lua syntax validation: try to compile without executing */
    if (cfg.script[0]) {
        lua_State *L = luaL_newstate();
        if (L) {
            int err = luaL_loadstring(L, cfg.script);
            if (err != LUA_OK) {
                const char *msg = lua_tostring(L, -1);
                static char errbuf[256];
                snprintf(errbuf, sizeof(errbuf), "Lua syntax error: %s",
                         msg ? msg : "unknown");
                lua_close(L);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, errbuf);
                return ESP_FAIL;
            }
            lua_close(L);
        }
    }

    esp_err_t err = script_slot_set((uint8_t)slot, &cfg);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t api_script_delete(httpd_req_t *req)
{
    char query[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing slot param");
        return ESP_FAIL;
    }
    char val[4] = {0};
    httpd_query_key_value(query, "slot", val, sizeof(val));
    int slot = atoi(val);
    if (slot < 0 || slot >= SCRIPT_MAX_SLOTS) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid slot");
        return ESP_FAIL;
    }

    script_slot_clear((uint8_t)slot);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

/* GET /api/diag — diagnostics: NVS usage of the "nvs" partition + the CHIP
 * binding table as the firmware actually holds it. Used to tell whether a
 * binding write persisted and whether the nvs partition is out of space. */
static esp_err_t api_diag_get(httpd_req_t *req)
{
    static char out[2048];
    int pos = 0;

    nvs_stats_t st;
    esp_err_t r = nvs_get_stats(NULL, &st);
    if (r == ESP_OK) {
        pos += snprintf(out + pos, sizeof(out) - pos,
            "NVS (nvs partition):\n"
            "  used_entries  = %d\n"
            "  free_entries  = %d\n"
            "  total_entries = %d\n"
            "  namespaces    = %d\n\n",
            (int) st.used_entries, (int) st.free_entries,
            (int) st.total_entries, (int) st.namespace_count);
    } else {
        pos += snprintf(out + pos, sizeof(out) - pos,
            "NVS stats unavailable: %s\n\n", esp_err_to_name(r));
    }

    /* Enumerate NVS namespaces + per-namespace entry counts. Reveals leftover
     * namespaces on modules upgraded without a factory reset. */
    pos += snprintf(out + pos, sizeof(out) - pos, "Namespaces:\n");
    {
        char names[24][16];
        int  counts[24];
        int  ns = 0;
        nvs_iterator_t it = NULL;
        esp_err_t ir = nvs_entry_find(NVS_DEFAULT_PART_NAME, NULL, NVS_TYPE_ANY, &it);
        while (ir == ESP_OK && it != NULL) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            int idx = -1;
            for (int i = 0; i < ns; i++) {
                if (strncmp(names[i], info.namespace_name, sizeof(names[0])) == 0) { idx = i; break; }
            }
            if (idx < 0 && ns < 24) {
                idx = ns++;
                strncpy(names[idx], info.namespace_name, sizeof(names[0]) - 1);
                names[idx][sizeof(names[0]) - 1] = '\0';
                counts[idx] = 0;
            }
            if (idx >= 0) counts[idx]++;
            ir = nvs_entry_next(&it);
        }
        nvs_release_iterator(it);
        for (int i = 0; i < ns; i++) {
            pos += snprintf(out + pos, sizeof(out) - pos,
                "  %-16s %d entries\n", names[i], counts[i]);
        }
        pos += snprintf(out + pos, sizeof(out) - pos, "\n");
    }

    char bind[1024];
    int n = matter_binding_dump(bind, sizeof(bind));
    pos += snprintf(out + pos, sizeof(out) - pos,
        "Binding table (%d entr%s):\n%s\n",
        n, (n == 1 ? "y" : "ies"), (n > 0 ? bind : "  <empty>"));

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, out);
    return ESP_OK;
}

/* ---------- HTTP server ---------- */

void web_api_start_httpd(void)
{
    httpd_handle_t srv = NULL;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size         = 8192;
    hc.recv_wait_timeout  = 30;
    hc.send_wait_timeout  = 10;
    hc.max_open_sockets   = 3;
    hc.max_uri_handlers   = 19;
    ESP_ERROR_CHECK(httpd_start(&srv, &hc));

    httpd_uri_t get_root          = { "/",                  HTTP_GET,    form_get,              NULL };
    httpd_uri_t post_upload       = { "/upload",            HTTP_POST,   upload_post,           NULL };
    httpd_uri_t post_ota          = { "/ota",               HTTP_POST,   ota_post,              NULL };
    httpd_uri_t get_settings      = { "/api/settings",      HTTP_GET,    api_settings_get,      NULL };
    httpd_uri_t get_hardware      = { "/api/hardware",      HTTP_GET,    api_hardware_get,      NULL };
    httpd_uri_t post_bench        = { "/api/bench-mode",    HTTP_POST,   api_bench_mode_post,   NULL };
    httpd_uri_t post_srp          = { "/api/srp-mode",     HTTP_POST,   api_srp_mode_post,     NULL };
    httpd_uri_t get_hw_config     = { "/api/hw-config",     HTTP_GET,    api_hw_config_get,     NULL };
    httpd_uri_t post_hw_config    = { "/api/hw-config",     HTTP_POST,   api_hw_config_post,    NULL };
    httpd_uri_t post_restart      = { "/api/restart",       HTTP_POST,   api_restart_post,      NULL };
    httpd_uri_t post_factory      = { "/api/factory-reset", HTTP_POST,   api_factory_reset_post,NULL };
    httpd_uri_t post_commission   = { "/api/commission",    HTTP_POST,   api_commission_post,   NULL };
    httpd_uri_t get_backup        = { "/api/backup",        HTTP_GET,    api_backup_get,        NULL };
    httpd_uri_t post_restore      = { "/api/restore",       HTTP_POST,   api_restore_post,      NULL };
    httpd_uri_t get_script        = { "/api/script",        HTTP_GET,    api_script_get,        NULL };
    httpd_uri_t post_script       = { "/api/script",        HTTP_POST,   api_script_post,       NULL };
    httpd_uri_t del_script        = { "/api/script",        HTTP_DELETE, api_script_delete,     NULL };
    httpd_uri_t get_diag          = { "/api/diag",          HTTP_GET,    api_diag_get,          NULL };

    httpd_register_uri_handler(srv, &get_root);
    httpd_register_uri_handler(srv, &post_upload);
    httpd_register_uri_handler(srv, &post_ota);
    httpd_register_uri_handler(srv, &get_settings);
    httpd_register_uri_handler(srv, &get_hardware);
    httpd_register_uri_handler(srv, &post_bench);
    httpd_register_uri_handler(srv, &post_srp);
    httpd_register_uri_handler(srv, &get_hw_config);
    httpd_register_uri_handler(srv, &post_hw_config);
    httpd_register_uri_handler(srv, &post_restart);
    httpd_register_uri_handler(srv, &post_factory);
    httpd_register_uri_handler(srv, &post_commission);
    httpd_register_uri_handler(srv, &get_backup);
    httpd_register_uri_handler(srv, &post_restore);
    httpd_register_uri_handler(srv, &get_script);
    httpd_register_uri_handler(srv, &post_script);
    httpd_register_uri_handler(srv, &del_script);
    httpd_register_uri_handler(srv, &get_diag);
}
