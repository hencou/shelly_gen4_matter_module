/*
 * OTA + WiFi module for the Shelly 1 Gen4 custom Matter firmware.
 *
 * Two paths, both via WiFi:
 *   1) STA mode with saved creds            -> direct esp_https_ota fetch.
 *   2) SoftAP mode + HTTP upload form       -> user uploads .bin directly from browser.
 *
 * During OTA mode the Matter stack is NOT started. This avoids
 * coexistence overhead and keeps the firmware size during OTA minimal.
 *
 * HTTP handlers and the management dashboard live in web_api.c.
 */

#include "ota.h"
#include "web_api.h"
#include "matter_device.h"
#include "shelly_boot.h"
#include "app_config.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#include "button.h"
#include "sensors.h"
#include "status_led.h"

#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_coexist.h"
#include "esp_coex_i154.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "esp_mac.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

static const char *TAG = "ota";

#define NVS_NS              "ota"
#define NVS_KEY_PENDING     "pending"
#define NVS_KEY_SSID        "ssid"
#define NVS_KEY_PASS        "pass"
#define NVS_KEY_URL         "url"
#define NVS_KEY_BENCH       "bench"
#define NVS_KEY_SRP         "srp"
#define NVS_KEY_HOSTNAME    "hostname"

/* Runtime bench mode — initialised from NVS in bench_mode_init(). */
int g_bench_mode = BENCH_MODE;

/* Runtime flags — initialised from NVS early in boot. */
static bool s_srp_mode = false;
static char s_hostname[32] = {0};

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_evt;
static int                s_retry = 0;
static TimerHandle_t      s_wifi_reconnect_timer = NULL;

/* ---------- NVS helpers ---------- */

static esp_err_t nvs_set_str_safe(nvs_handle_t h, const char *k, const char *v)
{
    return nvs_set_str(h, k, v ? v : "");
}

static bool nvs_load_str(nvs_handle_t h, const char *k, char *buf, size_t buflen)
{
    size_t l = buflen;
    if (nvs_get_str(h, k, buf, &l) != ESP_OK) return false;
    return strlen(buf) > 0;
}

esp_err_t ota_save_credentials(const char *ssid, const char *pass, const char *url)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str_safe(h, NVS_KEY_SSID, ssid);
    nvs_set_str_safe(h, NVS_KEY_PASS, pass);
    nvs_set_str_safe(h, NVS_KEY_URL,  url);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "credentials saved (ssid=%s, url=%s)",
             ssid ? ssid : "?", url ? url : "?");
    return ESP_OK;
}

bool ota_load_credentials(char *ssid, size_t ssidlen,
                          char *pass, size_t passlen,
                          char *url,  size_t urllen)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_load_str(h, NVS_KEY_SSID, ssid, ssidlen);
    if (ok) {
        nvs_load_str(h, NVS_KEY_PASS, pass, passlen);
        nvs_load_str(h, NVS_KEY_URL,  url,  urllen);
    }
    nvs_close(h);
    return ok;
}

static void srp_mode_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, NVS_KEY_SRP, &v) == ESP_OK) {
            s_srp_mode = (v != 0);
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "srp_mode = %d", s_srp_mode);
}

bool ota_srp_mode_get(void)
{
    return s_srp_mode;
}

esp_err_t ota_srp_mode_set(bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, NVS_KEY_SRP, on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
    s_srp_mode = on;
    ESP_LOGI(TAG, "srp_mode saved: %d", on);
    return ESP_OK;
}

/* ---------- Hostname NVS ---------- */

static void hostname_build_default(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, len, "shelly-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static void hostname_init(void)
{
    nvs_handle_t h;
    bool loaded = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        loaded = nvs_load_str(h, NVS_KEY_HOSTNAME, s_hostname, sizeof(s_hostname));
        nvs_close(h);
    }
    if (!loaded || !s_hostname[0]) {
        hostname_build_default(s_hostname, sizeof(s_hostname));
    }
    ESP_LOGI(TAG, "hostname = %s", s_hostname);
}

const char *ota_hostname_get(void)
{
    return s_hostname;
}

esp_err_t ota_hostname_set(const char *name)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str_safe(h, NVS_KEY_HOSTNAME, name);
    nvs_commit(h);
    nvs_close(h);
    strncpy(s_hostname, name, sizeof(s_hostname) - 1);
    s_hostname[sizeof(s_hostname) - 1] = '\0';
    ESP_LOGI(TAG, "hostname saved: %s", s_hostname);
    return ESP_OK;
}

/* ---------- Bench mode NVS ---------- */

void bench_mode_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0xff;
        if (nvs_get_u8(h, NVS_KEY_BENCH, &v) == ESP_OK) {
            g_bench_mode = (v != 0) ? 1 : 0;
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "bench_mode = %d (compile-time default = %d)",
             g_bench_mode, BENCH_MODE);

    srp_mode_init();
    hostname_init();
}

esp_err_t ota_bench_mode_save(int on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, NVS_KEY_BENCH, on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
    g_bench_mode = on;
    ESP_LOGI(TAG, "bench_mode saved: %d", on);
    return err;
}

/* ---------- OTA pending flag ---------- */

static bool ota_pending_read_and_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t v = 0;
    nvs_get_u8(h, NVS_KEY_PENDING, &v);
    nvs_set_u8(h, NVS_KEY_PENDING, 0);
    nvs_commit(h);
    nvs_close(h);
    return v != 0;
}

void ota_request_at_next_boot(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_PENDING, 1);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "OTA requested -> rebooting");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

void ota_request_ota_reboot(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_PENDING, 1);
        nvs_commit(h);
        nvs_close(h);
    }
    esp_restart();
}

/* ---------- WiFi STA ---------- */

static void wifi_reconnect_cb(TimerHandle_t timer)
{
    (void)timer;
    ESP_LOGI(TAG, "wifi_reconnect: retry %d", s_retry);
    esp_wifi_connect();
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_retry++;
        if (s_retry <= 5) {
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_evt, WIFI_FAIL_BIT);
            /* Schedule reconnect via timer — never block the event handler.
             * Backoff: 5 s for the first ~30 attempts, then 15 s. */
            if (s_wifi_reconnect_timer) {
                TickType_t delay = pdMS_TO_TICKS((s_retry < 30) ? 5000 : 15000);
                xTimerChangePeriod(s_wifi_reconnect_timer, delay, 0);
                xTimerStart(s_wifi_reconnect_timer, 0);
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        if (s_wifi_reconnect_timer) xTimerStop(s_wifi_reconnect_timer, 0);
        xEventGroupSetBits(s_wifi_evt, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_sta(const char *ssid, const char *pass)
{
    s_wifi_evt = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_nif = esp_netif_create_default_wifi_sta();
    if (sta_nif) {
        esp_netif_set_hostname(sta_nif, ota_hostname_get());
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t any_id, got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &got_ip));

    wifi_config_t wcfg = { 0 };
    strncpy((char*)wcfg.sta.ssid,     ssid, sizeof(wcfg.sta.ssid));
    strncpy((char*)wcfg.sta.password, pass, sizeof(wcfg.sta.password));
    wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

/* ---------- HTTPS OTA fetch (STA path: fetch URL) ---------- */

static esp_err_t do_ota_from_url(const char *url)
{
    ESP_LOGI(TAG, "starting OTA from %s", url);
    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .crt_bundle_attach = NULL,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };
    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        /* esp_https_ota() already set the ESP-IDF otadata boot slot, which is
         * all the ESP-IDF bootloader needs. Only on devices still carrying the
         * stock Shelly OS loader do we additionally mirror the slot into its
         * SH0S boot-select (the stock loader ignores the IDF otadata format). */
        if (shelly_loader_present()) {
            const esp_partition_t *boot = esp_ota_get_boot_partition();
            if (boot) {
                int slot = (int)boot->subtype - (int)ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
                esp_err_t se = shelly_boot_switch_slot(slot);
                ESP_LOGI(TAG, "OTA OK: SH0S boot-select app_%d -> %s",
                         slot, esp_err_to_name(se));
            }
        }
        ESP_LOGI(TAG, "OTA OK, rebooting");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    return err;
}

/* ---------- OTA timeout ---------- */

#define OTA_TIMEOUT_MS   (10 * 60 * 1000)
#define OTA_TICK_MS      5000

static void wait_for_upload_or_timeout(void)
{
    int32_t remaining_ms = OTA_TIMEOUT_MS;
    while (remaining_ms > 0) {
        int32_t sleep = (remaining_ms < OTA_TICK_MS)
                        ? remaining_ms : OTA_TICK_MS;
        vTaskDelay(pdMS_TO_TICKS(sleep));
        remaining_ms -= sleep;
        if (remaining_ms > 0) {
            ESP_LOGW(TAG, "OTA: %"PRId32" seconds remaining, waiting for upload...",
                     remaining_ms / 1000);
        }
    }

    ESP_LOGW(TAG, "OTA timeout expired — rebooting to Matter mode");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

/* ---------- Start SoftAP ---------- */

static void run_softap(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "shelly-ota-%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    ESP_LOGW(TAG, "SoftAP opening: '%s' (open network)", ssid);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));

    wifi_config_t apc = {
        .ap = {
            .max_connection = 3,
            .authmode = WIFI_AUTH_OPEN,
            .channel = 6,
        },
    };
    strncpy((char*)apc.ap.ssid, ssid, sizeof(apc.ap.ssid));
    apc.ap.ssid_len = strlen(ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apc));
    ESP_ERROR_CHECK(esp_wifi_start());

    web_api_start_httpd();

    ESP_LOGW(TAG, "SoftAP ready. Connect to '%s', open http://192.168.4.1/", ssid);

    wait_for_upload_or_timeout();  /* never returns (reboots) */
}

/* ---------- Button callback during OTA mode ---------- */

static void ota_mode_button_cb(input_id_t id, button_event_t evt)
{
    ESP_LOGI(TAG, "OTA mode: input=%d evt=%d ignored", id, evt);
}

/* ---------- Temporary WiFi alongside Thread/Matter ---------- */

static void build_ap_ssid(char *buf, size_t len)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(buf, len, "shelly-cfg-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

/* NVS credentials, falling back to the compile-time secrets.h defaults.
 * from_compile_time reports that fallback, so a successful connect can persist
 * them to NVS. */
static bool load_sta_credentials(char *ssid, size_t ssidlen,
                                 char *pass, size_t passlen,
                                 char *url,  size_t urllen,
                                 bool *from_compile_time)
{
    bool have_creds = ota_load_credentials(ssid, ssidlen, pass, passlen, url, urllen);
    *from_compile_time = false;

#ifdef DEFAULT_WIFI_SSID
    if (!have_creds) {
#ifndef DEFAULT_WIFI_PASS
#define DEFAULT_WIFI_PASS ""
#endif
        strncpy(ssid, DEFAULT_WIFI_SSID, ssidlen - 1);
        strncpy(pass, DEFAULT_WIFI_PASS, passlen - 1);
        have_creds = strlen(ssid) > 0;
        *from_compile_time = have_creds;
    }
#endif
    return have_creds;
}

/* ---------- Temporary WiFi alongside Thread (no reboot) ---------- */

/* Triggered from the management page (reachable over Thread/IPv6) or by 6x
 * clicking any input — the only WiFi path in this firmware, commissioned or
 * not.
 *
 * WiFi and 802.15.4 share one radio on the ESP32-C6. Espressif's coexistence
 * matrix lists only "WiFi STA + Thread end device" as supported; STA + Thread
 * *router* is documented as unstable and SoftAP + router as unsupported, so the
 * node hands in its router role for the duration. The SRP fallback
 * server requires Router/Leader, so it stands down as well and is resumed
 * together with the router role.
 *
 * Without stored credentials there is nothing to join, so the window opens a
 * SoftAP instead. That is the entry point for a device that is not commissioned
 * yet (no Thread network, hence no dashboard over IPv6).
 *
 * Setup and teardown both happen in this one task, so the window closes on its
 * own without a reboot.
 *
 * The coexistence arbiter between WiFi and 802.15.4 is not started by ESP-IDF
 * itself: the application has to call esp_coex_wifi_i154_enable() (see the
 * ot_br and zigbee_gateway examples). Without it WiFi never gets airtime next
 * to a running Thread stack and even the scan comes up empty. */

#define WIFI_COEX_WINDOW_US    (10ULL * 60 * 1000 * 1000)   /* 10 minutes */
#define WIFI_COEX_CONNECT_MS   60000                        /* give up if never up */
#define WIFI_COEX_TICK_MS      1000

static volatile bool    s_coex_active      = false;
static volatile int64_t s_coex_deadline_us = 0;
static bool s_coex_wifi_inited = false;
static esp_event_handler_instance_t s_coex_evt_any = NULL;
static esp_event_handler_instance_t s_coex_evt_ip  = NULL;

/* Give the radio and the Thread role back. Safe to call after a partial start. */
static void wifi_coex_teardown(void)
{
    /* Unregister first: esp_wifi_stop() emits STA_DISCONNECTED, and the handler
     * would answer that by arming the reconnect timer we are shutting down. */
    if (s_coex_evt_any) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              s_coex_evt_any);
        s_coex_evt_any = NULL;
    }
    if (s_coex_evt_ip) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              s_coex_evt_ip);
        s_coex_evt_ip = NULL;
    }
    if (s_wifi_reconnect_timer) xTimerStop(s_wifi_reconnect_timer, 0);

    if (s_coex_wifi_inited) {
        esp_wifi_stop();
        esp_wifi_deinit();          /* hands the driver RAM back to Matter/Thread */
        s_coex_wifi_inited = false;
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE && CONFIG_SOC_IEEE802154_SUPPORTED
        /* WiFi is gone, so make sure 802.15.4 is registered as a radio client
         * again and gets the whole radio back. */
        esp_coex_ieee802154_status_enable();
#endif
    }

    matter_thread_router_eligible_set(true);
    matter_srp_fallback_pause(false);
    ESP_LOGW(TAG, "wifi_coex: WiFi off, Thread router role and SRP fallback restored");
}

static void wifi_coex_task(void *arg)
{
    (void)arg;
    char ssid[33] = {0}, pass[65] = {0}, url[256] = {0};
    bool from_compile_time = false;

    bool have_creds = load_sta_credentials(ssid, sizeof(ssid), pass, sizeof(pass),
                                          url, sizeof(url), &from_compile_time);

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_t *sta = NULL;
    if (have_creds) {
        if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"))
            esp_netif_create_default_wifi_sta();
        sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (sta) esp_netif_set_hostname(sta, ota_hostname_get());
    } else if (!esp_netif_get_handle_from_ifkey("WIFI_AP_DEF")) {
        esp_netif_create_default_wifi_ap();
    }

    /* Stand down as router BEFORE the WiFi radio starts competing, so the mesh
     * sees an orderly downgrade instead of a router that stops responding. */
    matter_srp_fallback_pause(true);
    matter_thread_router_eligible_set(false);

    if (!s_coex_wifi_inited) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_wifi_init(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi_coex: esp_wifi_init failed (%s)", esp_err_to_name(err));
            wifi_coex_teardown();
            s_coex_active = false;
            vTaskDelete(NULL);
            return;
        }
        s_coex_wifi_inited = true;
    }

#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE && CONFIG_SOC_IEEE802154_SUPPORTED
    /* Before esp_wifi_start(), because the driver samples the coexistence
     * state while it comes up and the first scan runs immediately after. */
    esp_err_t coex_err = esp_coex_wifi_i154_enable();
    if (coex_err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_coex: coexistence arbiter refused to start (%s) — "
                      "WiFi would get no airtime", esp_err_to_name(coex_err));
        wifi_coex_teardown();
        s_coex_active = false;
        vTaskDelete(NULL);
        return;
    }
#endif

    if (!have_creds) {
        char ap_ssid[32];
        build_ap_ssid(ap_ssid, sizeof(ap_ssid));

        wifi_config_t apc = {
            .ap = {
                .max_connection = 3,
                .authmode = WIFI_AUTH_OPEN,
                .channel = 6,
            },
        };
        strncpy((char*)apc.ap.ssid, ap_ssid, sizeof(apc.ap.ssid));
        apc.ap.ssid_len = strlen(ap_ssid);

        esp_err_t aerr = esp_wifi_set_mode(WIFI_MODE_AP);
        if (aerr == ESP_OK) aerr = esp_wifi_set_config(WIFI_IF_AP, &apc);
        if (aerr == ESP_OK) aerr = esp_wifi_start();
        if (aerr != ESP_OK) {
            ESP_LOGE(TAG, "wifi_coex: SoftAP start failed (%s)", esp_err_to_name(aerr));
            wifi_coex_teardown();
            s_coex_active = false;
            vTaskDelete(NULL);
            return;
        }
        web_api_start_httpd();
        ESP_LOGW(TAG, "wifi_coex: no credentials — SoftAP '%s' open, "
                      "dashboard on http://192.168.4.1/", ap_ssid);
    } else {
        if (!s_wifi_evt) s_wifi_evt = xEventGroupCreate();
        xEventGroupClearBits(s_wifi_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        s_retry = 0;
        if (!s_wifi_reconnect_timer)
            s_wifi_reconnect_timer = xTimerCreate("wifi_rc", pdMS_TO_TICKS(5000),
                                                  pdFALSE, NULL, wifi_reconnect_cb);

        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            &wifi_event_handler, NULL, &s_coex_evt_any);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            &wifi_event_handler, NULL, &s_coex_evt_ip);

        wifi_config_t wcfg = { 0 };
        strncpy((char*)wcfg.sta.ssid,     ssid, sizeof(wcfg.sta.ssid));
        strncpy((char*)wcfg.sta.password, pass, sizeof(wcfg.sta.password));
        wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        /* Sharing the radio means beacons get missed, so scan every channel and
         * pick the strongest AP instead of the first hit. */
        wcfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wcfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
        if (err == ESP_OK) err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi_coex: WiFi start failed (%s)", esp_err_to_name(err));
            wifi_coex_teardown();
            s_coex_active = false;
            vTaskDelete(NULL);
            return;
        }
        /* Modem sleep leaves the radio to Thread between WiFi beacons. */
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        ESP_LOGW(TAG, "wifi_coex: STA-only connecting to '%s' (source: %s), Thread stays up",
                 ssid, from_compile_time ? "compile-time" : "NVS");

        EventBits_t bits = xEventGroupWaitBits(s_wifi_evt,
                                               WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                               pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(WIFI_COEX_CONNECT_MS));
        if (!(bits & WIFI_CONNECTED_BIT)) {
            ESP_LOGE(TAG, "wifi_coex: no connection within %d s — closing the window early",
                     WIFI_COEX_CONNECT_MS / 1000);
            wifi_coex_teardown();
            s_coex_active = false;
            vTaskDelete(NULL);
            return;
        }

        esp_netif_ip_info_t ip = { 0 };
        if (sta && esp_netif_get_ip_info(sta, &ip) == ESP_OK)
            ESP_LOGW(TAG, "wifi_coex: connected, dashboard also on http://" IPSTR "/",
                     IP2STR(&ip.ip));
        if (from_compile_time) ota_save_credentials(ssid, pass, url);
        /* Over Thread the dashboard is normally already up; on a device that is
         * not commissioned yet this is its first start. */
        web_api_start_httpd();
    }

    int last_logged = -1;
    while (esp_timer_get_time() < s_coex_deadline_us) {
        vTaskDelay(pdMS_TO_TICKS(WIFI_COEX_TICK_MS));
        int left = (int)((s_coex_deadline_us - esp_timer_get_time()) / 1000000);
        if (left > 0 && left % 60 == 0 && left != last_logged) {
            last_logged = left;
            ESP_LOGI(TAG, "wifi_coex: %d s remaining", left);
        }
    }

    ESP_LOGW(TAG, "wifi_coex: window expired");
    wifi_coex_teardown();
    s_coex_active = false;
    vTaskDelete(NULL);
}

esp_err_t ota_wifi_coex_start(void)
{
    /* Pressing again while the window is open simply extends it. */
    s_coex_deadline_us = esp_timer_get_time() + (int64_t)WIFI_COEX_WINDOW_US;
    if (s_coex_active) {
        ESP_LOGI(TAG, "wifi_coex: window extended to 10 min from now");
        return ESP_OK;
    }

    s_coex_active = true;
    if (xTaskCreate(wifi_coex_task, "wifi_coex", 4096, NULL, 5, NULL) != pdPASS) {
        s_coex_active = false;
        ESP_LOGE(TAG, "wifi_coex: task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ota_wifi_coex_stop(void)
{
    if (!s_coex_active) return ESP_ERR_INVALID_STATE;
    /* The task owns setup and teardown; expire its window instead of tearing
     * WiFi down from another context. */
    s_coex_deadline_us = 0;
    ESP_LOGW(TAG, "wifi_coex: stop requested");
    return ESP_OK;
}

int ota_wifi_coex_seconds_left(void)
{
    if (!s_coex_active) return 0;
    int64_t left = s_coex_deadline_us - esp_timer_get_time();
    return (left > 0) ? (int)(left / 1000000) : 0;
}

/* ---------- Public entrypoints ---------- */

void ota_handle_pending(void)
{
    if (!ota_pending_read_and_clear()) {
        return;
    }
    ESP_LOGW(TAG, "OTA pending — entering DEDICATED OTA-mode (Matter NOT started)");
    ESP_LOGW(TAG, "OTA mode: sensor tasks not started, Hardware tab shows no Add-on readings");

    button_driver_init(ota_mode_button_cb);
    status_led_set(STATUS_LED_FAST_BLINK);

    char ssid[33] = {0}, pass[65] = {0}, url[256] = {0};
    bool have_creds = ota_load_credentials(ssid, sizeof(ssid),
                                           pass, sizeof(pass),
                                           url,  sizeof(url));

#ifdef DEFAULT_WIFI_SSID
    if (!have_creds) {
        ESP_LOGI(TAG, "No saved WiFi creds — using compile-time defaults");
#ifndef DEFAULT_WIFI_PASS
#define DEFAULT_WIFI_PASS ""
#endif
#ifndef DEFAULT_OTA_URL
#define DEFAULT_OTA_URL   ""
#endif
        ota_save_credentials(DEFAULT_WIFI_SSID,
                             DEFAULT_WIFI_PASS,
                             DEFAULT_OTA_URL);
        strncpy(ssid, DEFAULT_WIFI_SSID, sizeof(ssid) - 1);
        strncpy(pass, DEFAULT_WIFI_PASS, sizeof(pass) - 1);
        strncpy(url,  DEFAULT_OTA_URL,   sizeof(url) - 1);
        have_creds = strlen(ssid) > 0;
    }
#endif
    if (have_creds) {
        ESP_LOGI(TAG, "trying STA OTA -> ssid=%s url=%s", ssid, url);
        if (wifi_init_sta(ssid, pass) == ESP_OK) {
            web_api_start_httpd();
            ESP_LOGW(TAG, "STA connected. OTA web interface available on local IP");

            if (url[0] && do_ota_from_url(url) == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
            ESP_LOGW(TAG, "URL fetch skipped/failed, waiting for manual upload...");
            wait_for_upload_or_timeout();
        }
        ESP_LOGW(TAG, "STA connection failed, falling back to SoftAP");
        esp_wifi_stop();
        esp_wifi_deinit();
    }
    run_softap();
}

void ota_mark_app_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "current app marked valid (rollback canceled)");
        }
    }
}
