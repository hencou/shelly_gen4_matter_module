#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Return-to-stock support.
 *
 * Accepts an original Shelly firmware package (the same .zip the stock web UI
 * consumes) uploaded to the management page and writes it back so the device
 * boots stock Shelly firmware again — without needing a full UART backup.
 *
 * The bootloader, partition table and the factory `shelly` partition are never
 * touched:
 *   - the stock app is written to the inactive app slot (never the running
 *     one, so an aborted upload is harmless);
 *   - the matching filesystem partition is rewritten;
 *   - the "boot" / "pt" parts in the package are intentionally skipped;
 *   - the boot slot is committed via the ESP-IDF bootloader's otadata
 *     (esp_ota_set_boot_partition) on our installs, or via the stock SH0S
 *     boot-select on units still running the stock Shelly OS loader.
 *
 * On our (ESP-IDF bootloader) installs the stock app runs under our bootloader;
 * the stock Shelly OS loader is reinstalled by the stock firmware's own next
 * update, so we never rewrite the bootloader from here.
 *
 * Nothing is committed until the app image is fully written and its SHA-256
 * verified, so a failed/aborted upload leaves the running slot untouched.
 */
esp_err_t stock_restore_handle_upload(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
