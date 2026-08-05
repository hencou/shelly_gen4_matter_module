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
 * This only works on units that still carry the stock Shelly OS loader (i.e.
 * installed via the web-UI package, which preserves it). The bootloader and
 * the factory `shelly` partition are never touched:
 *   - the stock app is written to the inactive app slot (transparent flash
 *     encryption applies, because the write is done by the running firmware);
 *   - the matching filesystem partition is rewritten;
 *   - the "boot" / "pt" parts in the package are intentionally skipped;
 *   - the stock SH0S boot-select is pointed at the freshly written slot.
 *
 * Nothing is committed until the app image is fully written and its SHA-256
 * verified, so a failed/aborted upload leaves the running slot untouched.
 */
esp_err_t stock_restore_handle_upload(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
