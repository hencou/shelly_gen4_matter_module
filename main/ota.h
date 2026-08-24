#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * OTA / WiFi module for the Shelly 1 Gen4 custom firmware.
 *
 * Two WiFi paths:
 *
 * A) Runtime WiFi enable (6x clicks in Matter mode):
 *   - WiFi starts alongside Thread (both active, non-persistent).
 *   - If STA connection succeeds: management dashboard reachable.
 *   - If STA fails: WiFi credentials wiped, AP mode started.
 *   - After reboot: WiFi is OFF again (only in RAM, not NVS flag).
 *
 * B) Dedicated OTA mode (via ota_request_at_next_boot):
 *   1) NVS flag set + reboot.
 *   2) At boot: ota_handle_pending() inspects the flag.
 *      - If saved WiFi creds exist -> direct STA OTA.
 *      - Otherwise -> SoftAP "shelly-ota-XXXXXX" with HTTP form on
 *        http://192.168.4.1/ to enter SSID/pass/URL once.
 *   3) 10-minute timeout: if no upload occurs, reboot back to Matter.
 *   4) On success: esp_restart() -> new firmware boots, Thread resumes.
 *   5) On failure: ESP-IDF rollback does not mark new app as valid;
 *      after 3rd failed boot it reverts to the previous slot.
 */

/* Call early in app_main, before the Matter stack or large components. */
void ota_handle_pending(void);

/* Set OTA flag in NVS and reboot the device. */
void ota_request_at_next_boot(void);

/* Set OTA pending flag and reboot (used by web /ota POST handler). */
void ota_request_ota_reboot(void);

/* WiFi management mode: set an NVS flag and reboot. On the next boot the device
 * brings up WiFi (STA if credentials are stored, otherwise a SoftAP) and serves
 * the management dashboard, without starting Matter/Thread. After 10 minutes it
 * reboots back to normal Thread/Matter mode. Meant to be triggered remotely from
 * the management page over Thread, so a hard-to-reach device can be managed over
 * a faster WiFi link without the physical 6x button press. */
void ota_request_wifi_mode_reboot(void);

/* Enable WiFi alongside Thread at runtime (non-persistent, lost on reboot).
 * If STA connection fails, WiFi credentials are wiped and AP mode is started. */
void ota_enable_wifi_runtime(void);

/* Temporary WiFi STA *alongside* Thread, without a reboot. Triggered from the
 * management page (which stays reachable over Thread the whole time). Connects
 * as STA only — no SoftAP — and hands in the Thread router role plus the SRP
 * fallback server for the duration, because WiFi and 802.15.4 share one radio
 * and Espressif only documents "STA + Thread end device" as supported. Closes
 * itself after 10 minutes and restores both. Pressing again while the window is
 * open extends it to another 10 minutes.
 * Returns ESP_ERR_INVALID_STATE when WiFi setup mode already owns the radio. */
esp_err_t ota_wifi_coex_start(void);

/* Close the temporary WiFi window early (teardown runs asynchronously). */
esp_err_t ota_wifi_coex_stop(void);

/* Seconds left in the temporary WiFi window; 0 when it is not open. */
int ota_wifi_coex_seconds_left(void);

/* True while runtime WiFi is active (6x press / WiFi setup mode). In this
 * state Thread is intentionally disabled, so the Thread watchdog must not
 * treat "not attached" as a fault. */
bool ota_wifi_runtime_active(void);

/* Save WiFi creds + URL in NVS (can also be done via web form). */
esp_err_t ota_save_credentials(const char *ssid, const char *password,
                               const char *firmware_url);

/* Load WiFi credentials from NVS. Returns true if ssid is non-empty. */
bool ota_load_credentials(char *ssid, size_t ssidlen,
                          char *pass, size_t passlen,
                          char *url,  size_t urllen);

/* Mark current firmware as valid so ESP-IDF does not roll back.
 * Call after successful boot + Thread/Matter join. */
void ota_mark_app_valid(void);

/* SRP Server mode: enable Thread DNS-SD service discovery without full TBR.
 * When enabled, the Shelly runs an SRP server so other Thread devices can
 * register and resolve services (needed for CASE sessions without a TBR).
 * Stored in NVS. Default: off. */
bool ota_srp_mode_get(void);
esp_err_t ota_srp_mode_set(bool on);

/* Save bench mode value to NVS (used by web API). */
esp_err_t ota_bench_mode_save(int on);

/* Commission pending flag: set by the web API commission endpoint before it
 * clears Matter fabrics and reboots. On the next boot the smart-boot logic
 * uses it to stay in BLE commissioning mode instead of falling back to WiFi
 * setup mode. Stored in NVS. */
bool ota_commission_pending_get(void);
esp_err_t ota_commission_pending_set(bool on);

/* Hostname: stored in NVS, used as DHCP hostname.
 * Default: "shelly-XXXXXX" (last 3 bytes of MAC). */
const char *ota_hostname_get(void);
esp_err_t ota_hostname_set(const char *name);

#ifdef __cplusplus
}
#endif
