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
 * WiFi only ever runs next to Thread/Matter, in the temporary window from
 * ota_wifi_coex_start() (6x clicks, or the button on the management page): no
 * reboot, closes itself after 10 minutes. Firmware updates go over whichever
 * interface is up — Matter OTA over Thread, an upload from the dashboard, or
 * ota_update_from_url() — so there is no dedicated WiFi-only OTA mode that
 * keeps the Matter stack down.
 *
 * On a failed update ESP-IDF rollback does not mark the new app as valid, and
 * after the third failed boot the bootloader reverts to the previous slot.
 */

/* Fetch the saved firmware URL and reboot into it on success. Needs a working
 * route to that URL, so open the temporary WiFi window first unless the server
 * is reachable over Thread. Returns ESP_ERR_INVALID_STATE without a saved
 * URL; the transfer itself runs in its own task. */
esp_err_t ota_update_from_url(void);

/* Temporary WiFi *alongside* Thread, without a reboot. Triggered from the
 * management page (which stays reachable over Thread the whole time) or by 6x
 * clicks on any input. Joins as STA and hands in the Thread router role plus
 * the SRP fallback server for the duration, because WiFi and 802.15.4 share one
 * radio and Espressif only documents "STA + Thread end device" as supported.
 * Without stored credentials, or when they fail to connect within a minute, it
 * opens a SoftAP for the rest of the window instead — the entry point for a
 * device that is not commissioned yet, which would otherwise be unreachable
 * altogether. Closes itself after 10 minutes and restores the Thread role.
 * Pressing again while the window is open extends it to another 10 minutes. */
esp_err_t ota_wifi_coex_start(void);

/* Close the temporary WiFi window early (teardown runs asynchronously). */
esp_err_t ota_wifi_coex_stop(void);

/* Seconds left in the temporary WiFi window; 0 when it is not open. */
int ota_wifi_coex_seconds_left(void);

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

/* Hostname: stored in NVS, used as DHCP hostname.
 * Default: "shelly-XXXXXX" (last 3 bytes of MAC). */
const char *ota_hostname_get(void);
esp_err_t ota_hostname_set(const char *name);

#ifdef __cplusplus
}
#endif
