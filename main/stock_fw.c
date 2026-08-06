/*
 * Stock-firmware lookup — see stock_fw.h.
 */

#include "stock_fw.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "stock_fw";

/* The factory JSON lives at offset 0 of the `shelly` partition. It carries the
 * Matter credentials too, so only read the small head we need for the model. */
#define SHELLY_HEAD_LEN 3072

/* Model (device label) -> Shelly update app code. Only the Shelly 1 Gen4 entry
 * is confirmed against real hardware; unknown models fall back to querying the
 * model string itself, which the endpoint may or may not accept. */
static const struct { const char *model; const char *app; } MODEL_APP[] = {
    { "S4SW-001X16EU", "S1G4" },   /* Shelly 1 Gen4 */
};

const char *stock_fw_app_for_model(const char *model)
{
    if (!model || !model[0]) return NULL;
    for (size_t i = 0; i < sizeof(MODEL_APP) / sizeof(MODEL_APP[0]); i++) {
        if (strcmp(model, MODEL_APP[i].model) == 0) return MODEL_APP[i].app;
    }
    return NULL;
}

esp_err_t stock_fw_read_model(char *out, size_t len)
{
    if (!out || len == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';

    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "shelly");
    if (!p) {
        ESP_LOGW(TAG, "no `shelly` partition");
        return ESP_ERR_NOT_FOUND;
    }

    char *buf = malloc(SHELLY_HEAD_LEN + 1);
    if (!buf) return ESP_ERR_NO_MEM;

    size_t rd = p->size < SHELLY_HEAD_LEN ? p->size : SHELLY_HEAD_LEN;
    esp_err_t err = esp_partition_read(p, 0, buf, rd);
    if (err != ESP_OK) { free(buf); return err; }
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return ESP_ERR_INVALID_STATE;

    cJSON *factory = cJSON_GetObjectItem(root, "factory");
    cJSON *model = factory ? cJSON_GetObjectItem(factory, "model") : NULL;
    if (cJSON_IsString(model) && model->valuestring[0]) {
        strncpy(out, model->valuestring, len - 1);
        out[len - 1] = '\0';
        err = ESP_OK;
    } else {
        err = ESP_ERR_NOT_FOUND;
    }
    cJSON_Delete(root);
    return err;
}

/* Fetch the update-lookup JSON for an app code into a caller buffer. */
static esp_err_t fetch_lookup(const char *app, char *body, size_t body_len, int *out_status)
{
    char url[128];
    snprintf(url, sizeof(url), "https://updates.shelly.cloud/update/%s", app);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) { esp_http_client_cleanup(c); return err; }

    esp_http_client_fetch_headers(c);
    int total = 0;
    while (total < (int)body_len - 1) {
        int r = esp_http_client_read(c, body + total, body_len - 1 - total);
        if (r <= 0) break;
        total += r;
    }
    body[total] = '\0';
    *out_status = esp_http_client_get_status_code(c);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ESP_OK;
}

esp_err_t stock_fw_info_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    char model[48] = {0};
    esp_err_t err = stock_fw_read_model(model, sizeof(model));
    if (err != ESP_OK) {
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"could not read model from factory partition\"}");
    }

    const char *app = stock_fw_app_for_model(model);
    /* Unknown model: try the model string itself as a best-effort app code. */
    const char *query = app ? app : model;

    char *body = malloc(2048);
    if (!body) {
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"out of memory\"}");
    }
    int status = 0;
    err = fetch_lookup(query, body, 2048, &status);

    char resp[1024];
    if (err != ESP_OK || status != 200) {
        snprintf(resp, sizeof(resp),
            "{\"ok\":false,\"model\":\"%s\",\"app\":\"%s\",\"error\":"
            "\"lookup failed (device needs internet; enable WiFi). status=%d\"}",
            model, query, status);
        free(body);
        return httpd_resp_sendstr(req, resp);
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"error\":\"could not parse lookup response\"}");
    }

    cJSON *stable = cJSON_GetObjectItem(root, "stable");
    cJSON *ver = stable ? cJSON_GetObjectItem(stable, "version") : NULL;
    cJSON *url = stable ? cJSON_GetObjectItem(stable, "url") : NULL;
    if (cJSON_IsString(ver) && cJSON_IsString(url)) {
        snprintf(resp, sizeof(resp),
            "{\"ok\":true,\"model\":\"%s\",\"app\":\"%s\","
            "\"version\":\"%s\",\"url\":\"%s\"}",
            model, query, ver->valuestring, url->valuestring);
    } else {
        snprintf(resp, sizeof(resp),
            "{\"ok\":false,\"model\":\"%s\",\"app\":\"%s\","
            "\"error\":\"no stable build in lookup response\"}",
            model, query);
    }
    cJSON_Delete(root);
    return httpd_resp_sendstr(req, resp);
}
