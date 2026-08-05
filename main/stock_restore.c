/*
 * Return-to-stock: apply an original Shelly firmware package (.zip) from the
 * management page. See stock_restore.h for the safety model.
 */
#include "stock_restore.h"
#include "shelly_boot.h"

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "stock_restore";

/* ---- ZIP (STORED / no compression) local file header ---- */
#define ZIP_LFH_SIG  0x04034b50u   /* "PK\3\4" local file header      */
#define ZIP_CDH_SIG  0x02014b50u   /* "PK\1\2" central directory      */
#define ZIP_EOCD_SIG 0x06054b50u   /* "PK\5\6" end of central dir     */
#define ZIP_LFH_LEN  30

#define MAX_PARTS       16
#define MAX_MANIFEST    16384
#define FS_WRITE_BLOCK  4096

typedef struct {
    char src[64];
    char type[16];
    char sha[65];
} part_t;

typedef struct {
    part_t parts[MAX_PARTS];
    int    nparts;
    bool   erase_nvs;   /* manifest carries an "nvs" fill part */
} manifest_t;

static uint16_t rd16(const uint8_t *b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t rd32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* Read exactly n bytes from the request body. Returns ESP_OK or an error. */
static esp_err_t rx_exact(httpd_req_t *req, void *dst, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    size_t got = 0;
    while (got < n) {
        int r = httpd_req_recv(req, (char *)p + got, n - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return ESP_FAIL;
        got += (size_t)r;
    }
    return ESP_OK;
}

/* Read and discard n bytes from the request body. */
static esp_err_t rx_skip(httpd_req_t *req, size_t n)
{
    char buf[512];
    while (n > 0) {
        size_t chunk = n < sizeof(buf) ? n : sizeof(buf);
        esp_err_t err = rx_exact(req, buf, chunk);
        if (err != ESP_OK) return err;
        n -= chunk;
    }
    return ESP_OK;
}

static void sha_hex(const uint8_t *digest, char out[65])
{
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = h[digest[i] >> 4];
        out[i * 2 + 1] = h[digest[i] & 0xF];
    }
    out[64] = 0;
}

static const part_t *manifest_lookup(const manifest_t *m, const char *src)
{
    for (int i = 0; i < m->nparts; i++)
        if (strcmp(m->parts[i].src, src) == 0) return &m->parts[i];
    return NULL;
}

/* Parse the buffered manifest.json into part descriptors. */
static bool manifest_parse(const char *json, size_t len, manifest_t *m)
{
    memset(m, 0, sizeof(*m));
    (void)len;
    cJSON *root = cJSON_Parse(json);   /* json is NUL-terminated by the caller */
    if (!root) return false;

    bool ok = false;
    cJSON *parts = cJSON_GetObjectItem(root, "parts");
    if (!parts || !cJSON_IsObject(parts)) goto out;

    for (cJSON *p = parts->child; p; p = p->next) {
        cJSON *j_type = cJSON_GetObjectItem(p, "type");
        const char *type = (j_type && cJSON_IsString(j_type)) ? j_type->valuestring
                                                              : p->string;
        if (type && strcmp(type, "nvs") == 0) {
            m->erase_nvs = true;
            continue;
        }
        cJSON *j_src = cJSON_GetObjectItem(p, "src");
        if (!j_src || !cJSON_IsString(j_src)) continue;   /* fill part, no file */
        if (m->nparts >= MAX_PARTS) goto out;

        part_t *dst = &m->parts[m->nparts++];
        strlcpy(dst->src, j_src->valuestring, sizeof(dst->src));
        strlcpy(dst->type, type ? type : "", sizeof(dst->type));
        cJSON *j_sha = cJSON_GetObjectItem(p, "cs_sha256");
        if (j_sha && cJSON_IsString(j_sha))
            strlcpy(dst->sha, j_sha->valuestring, sizeof(dst->sha));
    }
    ok = m->nparts > 0;

out:
    cJSON_Delete(root);
    return ok;
}

/* Stream `size` bytes of the "app" member into the inactive app slot. */
static esp_err_t write_app(httpd_req_t *req, size_t size,
                           const esp_partition_t *app_part, const char *want_sha)
{
    esp_ota_handle_t h;
    esp_err_t err = esp_ota_begin(app_part, size, &h);
    if (err != ESP_OK) return err;

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    char buf[1024];
    size_t remaining = size;
    while (remaining > 0) {
        size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
        if ((err = rx_exact(req, buf, chunk)) != ESP_OK) { esp_ota_abort(h); goto out; }
        if ((err = esp_ota_write(h, buf, chunk)) != ESP_OK) { esp_ota_abort(h); goto out; }
        mbedtls_sha256_update(&sha, (const uint8_t *)buf, chunk);
        remaining -= chunk;
    }

    uint8_t digest[32]; char hex[65];
    mbedtls_sha256_finish(&sha, digest);
    sha_hex(digest, hex);
    if (want_sha[0] && strcasecmp(hex, want_sha) != 0) {
        ESP_LOGE(TAG, "app SHA-256 mismatch");
        esp_ota_abort(h);
        err = ESP_ERR_INVALID_CRC;
        goto out;
    }

    err = esp_ota_end(h);   /* validates the app image header */
out:
    mbedtls_sha256_free(&sha);
    return err;
}

/* Stream `size` bytes of the "fs" member into the matching filesystem slot. */
static esp_err_t write_fs(httpd_req_t *req, size_t size,
                          const esp_partition_t *fs_part, const char *want_sha)
{
    if (!fs_part) return ESP_ERR_NOT_FOUND;
    if (size > fs_part->size) return ESP_ERR_INVALID_SIZE;

    esp_err_t err = esp_partition_erase_range(fs_part, 0, fs_part->size);
    if (err != ESP_OK) return err;

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    uint8_t block[FS_WRITE_BLOCK];
    size_t blk = 0, off = 0, remaining = size;
    char buf[1024];
    while (remaining > 0) {
        size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
        if ((err = rx_exact(req, buf, chunk)) != ESP_OK) goto out;
        mbedtls_sha256_update(&sha, (const uint8_t *)buf, chunk);
        for (size_t i = 0; i < chunk; i++) {
            block[blk++] = (uint8_t)buf[i];
            if (blk == FS_WRITE_BLOCK) {
                if ((err = esp_partition_write(fs_part, off, block, blk)) != ESP_OK) goto out;
                off += blk; blk = 0;
            }
        }
        remaining -= chunk;
    }
    if (blk > 0) {
        /* Pad the final block up to a 16-byte boundary (flash-encryption
         * requirement); the partition was just erased, so 0xFF padding is fine. */
        size_t padded = (blk + 15u) & ~15u;
        memset(block + blk, 0xFF, padded - blk);
        if ((err = esp_partition_write(fs_part, off, block, padded)) != ESP_OK) goto out;
    }

    uint8_t digest[32]; char hex[65];
    mbedtls_sha256_finish(&sha, digest);
    sha_hex(digest, hex);
    if (want_sha[0] && strcasecmp(hex, want_sha) != 0) {
        ESP_LOGE(TAG, "fs SHA-256 mismatch");
        err = ESP_ERR_INVALID_CRC;
    }
out:
    mbedtls_sha256_free(&sha);
    return err;
}

static void fail(httpd_req_t *req, const char *msg)
{
    ESP_LOGE(TAG, "return-to-stock failed: %s", msg);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
}

esp_err_t stock_restore_handle_upload(httpd_req_t *req)
{
    /* Target the inactive app slot + its matching filesystem partition. The
     * running slot is never overwritten, so an aborted upload is harmless. */
    const esp_partition_t *app_part = esp_ota_get_next_update_partition(NULL);
    if (!app_part) { fail(req, "no OTA app partition"); return ESP_FAIL; }
    int slot = (int)app_part->subtype - (int)ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
    const esp_partition_t *fs_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, slot ? "fs_1" : "fs_0");

    ESP_LOGW(TAG, "return-to-stock: writing stock app to %s (app_%d), fs=%s",
             app_part->label, slot, fs_part ? fs_part->label : "(none)");

    manifest_t man;
    bool have_manifest = false;
    bool wrote_app = false;

    /* Walk the ZIP local file headers sequentially. */
    for (;;) {
        uint8_t lfh[ZIP_LFH_LEN];
        if (rx_exact(req, lfh, ZIP_LFH_LEN) != ESP_OK) { fail(req, "truncated zip"); return ESP_FAIL; }

        uint32_t sig = rd32(lfh);
        if (sig == ZIP_CDH_SIG || sig == ZIP_EOCD_SIG) break;   /* end of members */
        if (sig != ZIP_LFH_SIG) { fail(req, "not a zip package"); return ESP_FAIL; }

        uint16_t method   = rd16(lfh + 8);
        uint32_t comp_sz  = rd32(lfh + 18);
        uint16_t name_len = rd16(lfh + 26);
        uint16_t extra_len= rd16(lfh + 28);
        if (method != 0) { fail(req, "zip must be uncompressed (STORED)"); return ESP_FAIL; }
        if (name_len >= 64) { fail(req, "zip member name too long"); return ESP_FAIL; }

        char name[64] = {0};
        if (rx_exact(req, name, name_len) != ESP_OK) { fail(req, "truncated zip"); return ESP_FAIL; }
        if (extra_len && rx_skip(req, extra_len) != ESP_OK) { fail(req, "truncated zip"); return ESP_FAIL; }

        if (strcmp(name, "manifest.json") == 0) {
            if (comp_sz == 0 || comp_sz > MAX_MANIFEST) { fail(req, "bad manifest size"); return ESP_FAIL; }
            char *json = malloc(comp_sz + 1);
            if (!json) { fail(req, "out of memory"); return ESP_FAIL; }
            if (rx_exact(req, json, comp_sz) != ESP_OK) { free(json); fail(req, "truncated manifest"); return ESP_FAIL; }
            json[comp_sz] = 0;
            bool ok = manifest_parse(json, comp_sz, &man);
            free(json);
            if (!ok) { fail(req, "invalid manifest"); return ESP_FAIL; }
            have_manifest = true;
            continue;
        }

        if (!have_manifest) { fail(req, "manifest.json must come first"); return ESP_FAIL; }

        const part_t *part = manifest_lookup(&man, name);
        const char *type = part ? part->type : "";
        const char *want_sha = part ? part->sha : "";

        if (part && strcmp(type, "app") == 0) {
            esp_err_t err = write_app(req, comp_sz, app_part, want_sha);
            if (err != ESP_OK) { fail(req, "app write/verify failed"); return ESP_FAIL; }
            wrote_app = true;
        } else if (part && strcmp(type, "fs") == 0) {
            esp_err_t err = write_fs(req, comp_sz, fs_part, want_sha);
            if (err != ESP_OK) { fail(req, "fs write/verify failed"); return ESP_FAIL; }
        } else {
            /* boot loader, partition table, or anything else: never written. */
            ESP_LOGW(TAG, "skipping part '%s' (type '%s')", name, type);
            if (rx_skip(req, comp_sz) != ESP_OK) { fail(req, "truncated zip"); return ESP_FAIL; }
        }
    }

    if (!have_manifest || !wrote_app) { fail(req, "package has no app image"); return ESP_FAIL; }

    /* Commit: point the stock loader at the freshly written slot. This is the
     * single irreversible step and only runs after the app verified. */
    esp_err_t err = shelly_boot_switch_slot(slot);
    if (err != ESP_OK) {
        fail(req, "not running the stock Shelly loader — return-to-stock needs a "
                  "web-UI install; restore your full UART backup instead");
        return ESP_FAIL;
    }

    /* Clear our NVS so stock starts clean (the factory `shelly` partition and
     * the loader are left intact). Done last, after the boot slot flipped. */
    if (man.erase_nvs) {
        const esp_partition_t *nvs = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
        if (nvs) {
            nvs_flash_deinit();
            esp_partition_erase_range(nvs, 0, nvs->size);
            ESP_LOGW(TAG, "NVS erased for stock");
        }
    }

    ESP_LOGW(TAG, "return-to-stock complete, rebooting into stock app_%d", slot);
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}
