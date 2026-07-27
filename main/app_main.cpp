/*
 * Shelly 1 Gen4 — Matter Switch firmware
 *
 * Entrypoint. Reuses button/relay/sensors/ota modules from the
 * Zigbee project; only the Matter stack part differs.
 */

extern "C" {
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_flash.h"
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
#include "status_led.h"
#include "script_engine.h"
}

#include "matter_device.h"
#include <app/server/Server.h>
#include <credentials/GroupDataProvider.h>
#include <lib/core/CHIPError.h>
#include <lib/core/DataModelTypes.h>
#include <lib/support/Span.h>
#include <platform/ConnectivityManager.h>

static const char *TAG = "app";

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

/* Stock Shelly 1 Gen4 stores its partition table at 0x10000.
 * Old custom firmware builds used 0x8000 (ESP-IDF default).
 * After OTA from stock Shelly, the PT lives at 0x10000 but this
 * firmware is compiled with CONFIG_PARTITION_TABLE_OFFSET=0x8000.
 * Copy the PT to the compiled offset so esp_partition finds it. */
#define STOCK_PT_OFFSET 0x10000
#define PT_MAGIC_0 0xAA
#define PT_MAGIC_1 0x50
#define PT_SECTOR_SIZE 4096

static void migrate_partition_table_if_needed(void)
{
    if (CONFIG_PARTITION_TABLE_OFFSET == STOCK_PT_OFFSET)
        return;

    uint8_t magic[2];
    if (esp_flash_read(NULL, magic, CONFIG_PARTITION_TABLE_OFFSET, 2) != ESP_OK)
        return;
    if (magic[0] == PT_MAGIC_0 && magic[1] == PT_MAGIC_1)
        return; /* PT already at compiled offset */

    if (esp_flash_read(NULL, magic, STOCK_PT_OFFSET, 2) != ESP_OK)
        return;
    if (magic[0] != PT_MAGIC_0 || magic[1] != PT_MAGIC_1)
        return; /* no valid PT at stock offset either */

    ESP_LOGW("boot", "PT at 0x%x invalid, found at 0x%x — migrating",
             CONFIG_PARTITION_TABLE_OFFSET, STOCK_PT_OFFSET);

    uint8_t *buf = (uint8_t *)malloc(PT_SECTOR_SIZE);
    if (!buf) {
        ESP_LOGE("boot", "PT migrate: malloc failed");
        return;
    }
    esp_err_t err;
    if ((err = esp_flash_read(NULL, buf, STOCK_PT_OFFSET, PT_SECTOR_SIZE)) != ESP_OK) {
        ESP_LOGE("boot", "PT migrate: read 0x%x failed (%s)", STOCK_PT_OFFSET, esp_err_to_name(err));
        free(buf); return;
    }
    if ((err = esp_flash_erase_region(NULL, CONFIG_PARTITION_TABLE_OFFSET, PT_SECTOR_SIZE)) != ESP_OK) {
        ESP_LOGE("boot", "PT migrate: erase 0x%x failed (%s)", CONFIG_PARTITION_TABLE_OFFSET, esp_err_to_name(err));
        free(buf); return;
    }
    if ((err = esp_flash_write(NULL, buf, CONFIG_PARTITION_TABLE_OFFSET, PT_SECTOR_SIZE)) != ESP_OK) {
        ESP_LOGE("boot", "PT migrate: write 0x%x failed (%s)", CONFIG_PARTITION_TABLE_OFFSET, esp_err_to_name(err));
        free(buf); return;
    }
    free(buf);
    ESP_LOGW("boot", "PT migrated to 0x%x, rebooting", CONFIG_PARTITION_TABLE_OFFSET);
    esp_restart();
}

extern "C" void app_main(void)
{
    migrate_partition_table_if_needed();
    ESP_ERROR_CHECK(nvs_flash_init());

    /* Mark current image as valid immediately so the bootloader does not
     * roll back while the rest of init runs (Matter/sensors can take seconds). */
    ota_mark_app_valid();

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

    /* SRP fallback server: provides DNS-SD service discovery on the Thread mesh
     * only when no real border router is present, so local device-to-device
     * bindings survive a TBR/HA outage. It yields to any border router the
     * moment one appears (a BR has the LAN advertising proxy our node lacks). */
    if (ota_srp_mode_get() && commissioned) {
        matter_srp_server_start();
        ESP_LOGI(TAG, "BOOT-STEP: SRP fallback controller started");
    }

    /* Thread connectivity watchdog: auto-recover a node that gets stuck
     * detached (unreachable over Thread) instead of requiring a manual reboot. */
    if (commissioned) {
        matter_thread_watchdog_start();
        ESP_LOGI(TAG, "BOOT-STEP: Thread connectivity watchdog started");
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
            chip::DeviceLayer::ConnectivityMgr().SetBLEAdvertisingEnabled(false);
            ota_enable_wifi_runtime();
        } else {
            ESP_LOGI(TAG, "Not commissioned, scripts configured — BLE commissioning mode");
        }
    }

    ESP_LOGI(TAG, "Shelly 1 Gen4 Matter Switch running");
}
