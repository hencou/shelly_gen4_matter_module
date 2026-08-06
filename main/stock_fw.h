#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Stock-firmware lookup.
 *
 * The device model (e.g. "S4SW-001X16EU") is read per-module from the factory
 * `shelly` partition, mapped to the Shelly update "app" code (e.g. "S1G4") and
 * resolved through the official lookup endpoint
 *   https://updates.shelly.cloud/update/<app>
 * which returns the current stable version and a direct fwcdn.shelly.cloud
 * download URL. The browser downloads the .zip straight from Shelly's CDN; the
 * device only fetches the small JSON (avoids the content-addressed URL being
 * hard-coded and avoids a browser CORS block against Shelly's endpoint).
 */

/* Read the factory model string from the `shelly` partition. */
esp_err_t stock_fw_read_model(char *out, size_t len);

/* Map a factory model to its Shelly update app code, or NULL if unknown. */
const char *stock_fw_app_for_model(const char *model);

/* GET /api/stock-fw — returns {ok,model,app,version,url} or {ok:false,...}. */
esp_err_t stock_fw_info_get(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
