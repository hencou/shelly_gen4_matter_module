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
 * The device is made byte-for-byte stock again. The stock app cannot run under
 * our ESP-IDF bootloader (it needs the Shelly OS loader and an SH0S boot state),
 * so a full stock package is restored:
 *   - the stock app is written to the inactive app slot (never the running one,
 *     so an aborted upload is harmless), and the matching filesystem partition
 *     is rewritten;
 *   - the stock boot state ("otadata"/boot_state.bin) is restored, then the SH0S
 *     boot-select is pointed at the slot the stock app was actually written to;
 *   - the stock partition table ("pt") is restored at 0x10000;
 *   - the stock bootloader ("boot", the Shelly OS loader) is restored at 0x0.
 *
 * The factory `shelly` partition is never touched. All writes are verified by
 * read-back; the units are not flash-encrypted, so the plaintext images from the
 * package reproduce the stock layout exactly.
 *
 * Ordering: the app+fs are written and SHA-256 verified first (nothing outside
 * the inactive slot is touched until then). The commit then writes otadata, the
 * partition table and finally the bootloader at 0x0 — the single irreversible
 * step. If the 0x0 write is interrupted the device needs UART recovery from the
 * full backup; this is documented on the management page.
 *
 * A non-standard package without a "boot"/"otadata" part falls back to a plain
 * boot-select flip (SH0S on stock-loader units, otadata on ESP-IDF units).
 */
esp_err_t stock_restore_handle_upload(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
