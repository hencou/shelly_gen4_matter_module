/*
 * Shelly 1 Gen4 — Matter Switch firmware
 *
 * Entrypoint. Reuses button/relay/sensors/ota modules from the
 * Zigbee project; only the Matter stack part differs.
 */

extern "C" {
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#include "app_config.h"
#include "hw_config.h"
#include "button.h"
#include "relay.h"
#include "sensors.h"
#include "power_meter.h"
#include "ade7953.h"
#include "ota.h"
#include "shelly_boot.h"
#include "loader_migrate.h"
#include "status_led.h"
#include "script_engine.h"
#include "log_buffer.h"
#include "web_api.h"
}

#include "matter_device.h"
#include <app/server/Server.h>
#include <credentials/GroupDataProvider.h>
#include <lib/core/CHIPError.h>
#include <lib/core/DataModelTypes.h>
#include <lib/support/Span.h>
#include <platform/ConnectivityManager.h>
#include <platform/PlatformManager.h>
#include <esp_timer.h>

static const char *TAG = "app";

/* Services that only make sense once the node is part of a fabric and has a
 * Thread interface: the management dashboard over Thread/IPv6, the address
 * logger + _http._tcp advertisement, the connectivity watchdog and the SRP
 * fallback server. Idempotent, so it can run at boot AND on the
 * commissioning-complete event without a reboot in between. */
static void start_commissioned_services(void)
{
    static bool started = false;
    if (started) return;
    started = true;

    if (ota_srp_mode_get()) {
        matter_srp_server_start();
        ESP_LOGI(TAG, "SRP fallback controller started");
    }

    matter_thread_watchdog_start();

    /* Management page over IPv6/Thread (no WiFi needed). Reachable once a
     * border router hands out an OMR address. */
    web_api_start_httpd();
    matter_thread_addr_log_start();
    ESP_LOGI(TAG, "management httpd started over Thread (IPv6)");
}

/* Freshly commissioned: bring the Thread-side services up now instead of at the
 * next boot. Deferred off the CHIP task by a one-shot timer, both to keep the
 * event handler short and to give Thread a moment to attach and pick up its
 * addresses. */
static esp_timer_handle_t s_commissioned_timer = NULL;

static void on_commissioning_complete(const chip::DeviceLayer::ChipDeviceEvent *event,
                                      intptr_t /*arg*/)
{
    if (event->Type != chip::DeviceLayer::DeviceEventType::kCommissioningComplete) return;
    if (s_commissioned_timer) return;

    const esp_timer_create_args_t args = {
        .callback = [](void *) { start_commissioned_services(); },
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "commissioned",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_commissioned_timer) == ESP_OK)
        esp_timer_start_once(s_commissioned_timer, 3 * 1000 * 1000);  /* 3 s */
}

extern "C" void on_button_event(input_id_t id, button_event_t evt)
{
    ESP_LOGI(TAG, "button id=%d evt=%d", id, evt);

    if (evt == BTN_EVT_MODE_TOGGLE) {
        ESP_LOGW(TAG, "MODE_TOGGLE from input %d -> disabling Thread, enabling WiFi", id);
        matter_disable_thread();
        ota_enable_wifi_runtime();
        return;
    }

    /* All button behavior is handled by Lua scripts */
    script_engine_button_event(id, evt);
}

extern "C" void on_temperature(int16_t centi_c)
{
    script_engine_temperature_update(centi_c);
    matter_update_temperature(centi_c);
}

extern "C" void on_occupancy(bool occupied)
{
    script_engine_occupancy_update(occupied);
}

extern "C" void on_analog(uint8_t duty_pct)
{
    script_engine_analog_update(duty_pct);
}

extern "C" void on_power(const power_meter_reading_t *r)
{
    matter_update_power_ch(0, r->voltage_v, r->current_a, r->power_w, r->frequency_hz);
}

/* ADE7953 dual-channel callback (2PM Gen4). */
extern "C" void on_power_ade(const power_meter_reading_t *a, const power_meter_reading_t *b)
{
    matter_update_power_ch(0, a->voltage_v, a->current_a, a->power_w, a->frequency_hz);
    matter_update_power_ch(1, b->voltage_v, b->current_a, b->power_w, b->frequency_hz);
}

extern "C" void app_main(void)
{
    /* Mirror ESP_LOG into RAM before anything else logs, so the management page
     * can show the boot sequence when the Add-on occupies UART0. */
    log_buffer_init();

    /* Mark current image as valid immediately so the bootloader does not
     * roll back while the rest of init runs (Matter/sensors can take seconds).
     * Done before the (possibly slow) nvs/Matter init and without an extra
     * reboot, so the rollback window is not left open. */
    ota_mark_app_valid();

    /* Cache the stock loader's SH0S boot-select so OTA (incl. the Matter OTA
     * requestor, which overwrites the live otadata) can still rebuild a valid
     * entry for the stock loader. No-op on IDF-bootloader builds. */
    shelly_boot_snapshot();

    ESP_ERROR_CHECK(nvs_flash_init());

    /* One-time migration to our ESP-IDF bootloader when the device still runs
     * the stock Shelly OS loader (install-from-stock ships no bootloader). This
     * reboots on success, so it must run before the heavy Matter/Thread init.
     * No-op once our loader is in place. */
    loader_migrate_maybe();

    /* Register VFS eventfd early with enough slots for OpenThread.
     * The OT platform uses ~3 eventfds.
     * ESP-IDF v5.4 has no Kconfig for this, so we register explicitly. */
    esp_vfs_eventfd_config_t eventfd_config = { .max_fds = 8 };
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

    /* Select the hardware profile (relay/switch/button/LED GPIOs + power
     * meter) from NVS before any driver init. Default = Shelly 1 Gen4. */
    hw_config_init();

    bench_mode_init();

    status_led_init();
    status_led_set(STATUS_LED_FAST_BLINK);  /* boot/init in progress */

    /* WiFi OTA path takes priority: when flag is set, Matter is NOT started */
    ota_handle_pending();

    relay_init();

    /* Load script slot types from NVS BEFORE matter_start —
     * endpoints are created dynamically based on slot configuration. */
    script_slot_type_t slot_types[SCRIPT_MAX_SLOTS];
    script_engine_load_slot_types(slot_types, SCRIPT_MAX_SLOTS);

    /* Matter MUST start before button_driver_init / sensors_init:
     * those install GPIO ISRs and FreeRTOS tasks that immediately call
     * Matter APIs via callbacks. */
    ESP_ERROR_CHECK(matter_start(slot_types, SCRIPT_MAX_SLOTS));
    ESP_LOGI(TAG, "BOOT-STEP: matter_start() done");

    bool commissioned = chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;

    if (commissioned) {
        start_commissioned_services();
        ESP_LOGI(TAG, "BOOT-STEP: commissioned services started");
    } else {
        /* Not in a fabric yet. Start the same services the moment commissioning
         * completes, so the dashboard is reachable over Thread without the user
         * having to reboot the device first. */
        CHIP_ERROR cerr =
            chip::DeviceLayer::PlatformMgr().AddEventHandler(on_commissioning_complete, 0);
        if (cerr != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "commissioning handler not registered: %" CHIP_ERROR_FORMAT,
                     cerr.Format());
        }
        ESP_LOGI(TAG, "BOOT-STEP: awaiting commissioning to start Thread services");
    }

    // =========================================================================
    // MULTICAST GROUP KEY — install KeySet 1 + GroupKeyMap
    // =========================================================================
    // The IPK (KeySet 0) cannot be used directly for group encryption on this
    // SDK version (returns CHIP_ERROR_INTERNAL).  We install our own KeySet 1
    // with a fixed epoch key directly via the GroupDataProvider API.
    // The same key must be installed on the lamps via the setup script.
    {
        using namespace chip::Credentials;
        GroupDataProvider *provider = GetGroupDataProvider();
        if (provider != nullptr) {
            // Shared 128-bit epoch key — must match the script's --epoch-key
            static const uint8_t kGroupEpochKey[16] = {
                0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
                0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf
            };

            for (const auto & fabricInfo : chip::Server::GetInstance().GetFabricTable()) {
                chip::FabricIndex idx = fabricInfo.GetFabricIndex();

                // GetCompressedFabricIdBytes fills a MutableByteSpan
                uint8_t cfid_buf[sizeof(uint64_t)];
                chip::MutableByteSpan cfid_span(cfid_buf);
                if (fabricInfo.GetCompressedFabricIdBytes(cfid_span) != CHIP_NO_ERROR) {
                    ESP_LOGW(TAG, "GroupKeySet 1: fabric %u cannot get CompressedFabricId", idx);
                    continue;
                }

                // Install KeySet 1 with our epoch key
                GroupDataProvider::KeySet keySet;
                keySet.keyset_id    = 1;
                keySet.policy       = GroupDataProvider::SecurityPolicy::kTrustFirst;
                keySet.num_keys_used = 1;
                keySet.epoch_keys[0].start_time = 1; // 1 µs = always valid
                memcpy(keySet.epoch_keys[0].key, kGroupEpochKey, 16);

                CHIP_ERROR err = provider->SetKeySet(idx, cfid_span, keySet);
                if (err == CHIP_NO_ERROR) {
                    ESP_LOGI(TAG, "GroupKeySet 1: fabric %u installed OK", idx);
                } else {
                    ESP_LOGW(TAG, "GroupKeySet 1: fabric %u FAILED %" CHIP_ERROR_FORMAT, idx, err.Format());
                }

                // GroupKeyMap (group → KeySet mapping) is NOT written here.
                // The setup script (create_matter_cluster_group.py) writes it
                // for the correct group ID.  Persisted in NVS — survives reboot.
            }
        } else {
            ESP_LOGE(TAG, "GroupKeyMap: GroupDataProvider is null");
        }
    }
    // =========================================================================
    
    /* Script engine — must init after Matter (needs endpoints), before buttons */
    script_engine_init();
    script_engine_start();
    ESP_LOGI(TAG, "BOOT-STEP: script_engine started");

    button_driver_init(on_button_event);
    ESP_LOGI(TAG, "BOOT-STEP: button_driver_init done, calling sensors_init");

    sensors_init(on_temperature, on_occupancy, on_analog);
    ESP_LOGI(TAG, "BOOT-STEP: sensors_init done");

    /* Power meter — reports voltage/current/power/frequency to the Electrical
     * Power Measurement Matter endpoint(s). 1PM Gen4 = BL0942 (UART, 1 channel);
     * 2PM Gen4 = ADE7953 (I2C, 2 channels). */
    if (hw_profile()->pm_type == PM_BL0942) {
        power_meter_init(hw_profile()->pm_uart_tx, hw_profile()->pm_uart_rx, on_power);
        ESP_LOGI(TAG, "BOOT-STEP: power_meter_init (BL0942) done");
    } else if (hw_profile()->pm_type == PM_ADE7953) {
        ade7953_init(hw_profile()->pm_i2c_sda, hw_profile()->pm_i2c_scl,
                     hw_profile()->pm_i2c_irq, on_power_ade);
        ESP_LOGI(TAG, "BOOT-STEP: ade7953_init done");
    }

    if (commissioned) {
        status_led_set(STATUS_LED_HEARTBEAT);
        ESP_LOGI(TAG, "BOOT-STEP: status_led -> HEARTBEAT (commissioned)");
    } else {
        status_led_set(STATUS_LED_SLOW_BLINK);
        ESP_LOGI(TAG, "BOOT-STEP: status_led -> SLOW_BLINK (not commissioned)");
    }

    /* Smart boot: decide between WiFi-setup mode and BLE-commissioning mode.
     *
     * Not commissioned + no scripts → WiFi setup mode:
     *   User needs the management dashboard to configure endpoints/scripts.
     *   Disable BLE advertising (radio conflict) and start WiFi.
     *
     * Not commissioned + scripts configured → BLE commissioning mode:
     *   User has set up endpoints via the dashboard and rebooted.
     *   Let BLE advertising run so the phone can discover and commission.
     *
     * Commissioned → normal operation:
     *   6× press enables WiFi temporarily (Thread disabled). */
    if (!commissioned) {
        bool has_slots = false;
        for (int i = 0; i < SCRIPT_MAX_SLOTS; i++) {
            if (slot_types[i] != SLOT_TYPE_NONE) { has_slots = true; break; }
        }
        /* Commission mode just cleared the fabrics and rebooted: the user
         * explicitly wants to re-pair, so keep BLE advertising regardless of
         * whether scripts are configured. Clear the flag so a later reboot
         * without pairing returns to the normal WiFi-setup behaviour. */
        bool commission_pending = ota_commission_pending_get();
        if (commission_pending) {
            ota_commission_pending_set(false);
            ESP_LOGI(TAG, "Not commissioned, commission mode pending — BLE commissioning mode");
        } else if (!has_slots) {
            ESP_LOGI(TAG, "Not commissioned, no scripts — WiFi setup mode (BLE off)");
            CHIP_ERROR cerr =
                chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(false);
            if (cerr != CHIP_NO_ERROR) {
                ESP_LOGE(TAG, "cannot stop BLE advertising: %" CHIP_ERROR_FORMAT, cerr.Format());
            }
            ota_enable_wifi_runtime();
        } else {
            ESP_LOGI(TAG, "Not commissioned, scripts configured — BLE commissioning mode");
        }
    }

    ESP_LOGI(TAG, "Shelly 1 Gen4 Matter Switch running");
}
