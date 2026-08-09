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
#include "esp_flash.h"
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
    char     src[64];
    char     type[16];
    char     sha[65];
    char     ptn[16];
    uint32_t addr;
    bool     has_addr;
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
        cJSON *j_ptn = cJSON_GetObjectItem(p, "ptn");
        if (j_ptn && cJSON_IsString(j_ptn))
            strlcpy(dst->ptn, j_ptn->valuestring, sizeof(dst->ptn));
        cJSON *j_addr = cJSON_GetObjectItem(p, "addr");
        if (j_addr && cJSON_IsNumber(j_addr)) {
            dst->addr = (uint32_t)j_addr->valuedouble;
            dst->has_addr = true;
        }
    }
    ok = m->nparts > 0;

out:
    cJSON_Delete(root);
    return ok;
}

/* Stream `size` bytes of a member straight into `part` at `base` and verify its
 * SHA-256. The partition is written raw with per-sector erase-then-write,
 * interleaved with the socket recv, so the single-threaded HTTP task never stops
 * draining the connection for long. `erase_tail` wipes the remainder of the
 * partition after the member (wanted for app/fs, not when staging).
 *
 * The app is written raw too (not via esp_ota_begin/esp_ota_end) on purpose:
 * the stock app is booted by the stock Shelly OS loader (SH0S boot-select) or
 * the stock otadata we restore below -- never through ESP-IDF's own OTA
 * metadata -- so esp_ota's upfront full-partition erase and final full-image
 * re-validation are not needed. Both are multi-second, socket-blocking steps;
 * for the app they land mid-upload (the fs still follows), which stalled the
 * browser at the app->fs boundary until the connection dropped (~79%). The
 * SHA-256 check keeps the same integrity guarantee. */
static esp_err_t write_member(httpd_req_t *req, size_t size,
                              const esp_partition_t *part, size_t base,
                              const char *want_sha, bool erase_tail)
{
    if (!part) return ESP_ERR_NOT_FOUND;
    if (base > part->size || size > part->size - base) return ESP_ERR_INVALID_SIZE;

    /* Heap-allocate the flash block: this runs on the HTTP server task whose
     * stack is only a few KB, so a 4 KB block on the stack (plus the caller's
     * manifest_t) overflows it and trips the stack guard. Receiving straight
     * into this block keeps the restore down to one buffer -- heap is scarce
     * with Matter, Thread, WiFi and BLE all up. */
    uint8_t *block = malloc(FS_WRITE_BLOCK);
    if (!block) return ESP_ERR_NO_MEM;

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    esp_err_t err = ESP_OK;
    size_t off = base, remaining = size;
    while (remaining > 0) {
        size_t blk = remaining < FS_WRITE_BLOCK ? remaining : FS_WRITE_BLOCK;
        if ((err = rx_exact(req, block, blk)) != ESP_OK) goto out;
        mbedtls_sha256_update(&sha, block, blk);

        /* Pad the final block up to a 16-byte boundary (flash-encryption
         * requirement); the sector is erased first, so 0xFF padding is fine. */
        size_t padded = (blk + 15u) & ~15u;
        if (padded > blk) memset(block + blk, 0xFF, padded - blk);

        /* Erase + write one sector, interleaved with recv, so the HTTP socket
         * keeps draining and the TCP upload never stalls. */
        if ((err = esp_partition_erase_range(part, off, FS_WRITE_BLOCK)) != ESP_OK) goto out;
        if ((err = esp_partition_write(part, off, block, padded)) != ESP_OK) goto out;
        off += FS_WRITE_BLOCK;
        remaining -= blk;
    }

    /* Erase any remaining tail now that the whole member has been received (no
     * socket left to stall), so the stock image sees a clean partition beyond
     * the written data. */
    if (erase_tail && off < part->size &&
        (err = esp_partition_erase_range(part, off, part->size - off)) != ESP_OK)
        goto out;

    uint8_t digest[32]; char hex[65];
    mbedtls_sha256_finish(&sha, digest);
    sha_hex(digest, hex);
    if (want_sha[0] && strcasecmp(hex, want_sha) != 0) {
        ESP_LOGE(TAG, "SHA-256 mismatch");
        err = ESP_ERR_INVALID_CRC;
    }
out:
    mbedtls_sha256_free(&sha);
    free(block);
    return err;
}

/* Buffer a small member (bootloader / partition-table / boot_state) into heap
 * and verify its SHA-256. These parts must be held until the app+fs are safely
 * written, so nothing on flash is touched before the app image is verified. */
static esp_err_t capture_part(httpd_req_t *req, size_t size, const char *want_sha,
                              uint8_t **out, size_t *out_len)
{
    uint8_t *b = malloc(size);
    if (!b) {
        ESP_LOGE(TAG, "out of heap for %u byte part", (unsigned)size);
        return ESP_ERR_NO_MEM;
    }
    if (rx_exact(req, b, size) != ESP_OK) {
        ESP_LOGE(TAG, "upload aborted while reading %u byte part", (unsigned)size);
        free(b);
        return ESP_FAIL;
    }

    if (want_sha[0]) {
        uint8_t digest[32]; char hex[65];
        mbedtls_sha256(b, size, digest, 0);
        sha_hex(digest, hex);
        if (strcasecmp(hex, want_sha) != 0) {
            ESP_LOGE(TAG, "SHA-256 mismatch on captured part");
            free(b);
            return ESP_ERR_INVALID_CRC;
        }
    }
    *out = b;
    *out_len = size;
    return ESP_OK;
}

/* Erase + write `len` bytes to raw flash offset `addr` and verify the read-back,
 * one sector at a time so no buffer the size of the image is ever needed. Used
 * for the bootloader (0x0) and the partition table (0x10000), which live outside
 * any partition. The units are not flash-encrypted, so plaintext writes
 * reproduce the stock image exactly.
 *
 * `src` reads the image: either straight from a RAM buffer or from the staging
 * partition, so the caller does not have to hold the bootloader in heap. */
typedef esp_err_t (*src_read_fn)(void *ctx, size_t off, uint8_t *dst, size_t len);

static esp_err_t write_raw_flash(uint32_t addr, src_read_fn read, void *ctx, size_t len)
{
    size_t erase = (len + 4095u) & ~4095u;   /* round up to a flash sector */
    esp_err_t err = esp_flash_erase_region(NULL, addr, erase);
    if (err != ESP_OK) return err;

    uint8_t *buf = malloc(FS_WRITE_BLOCK);
    uint8_t *chk = malloc(FS_WRITE_BLOCK);
    if (!buf || !chk) { free(buf); free(chk); return ESP_ERR_NO_MEM; }

    for (size_t off = 0; off < len; ) {
        size_t n = len - off < FS_WRITE_BLOCK ? len - off : FS_WRITE_BLOCK;
        if ((err = read(ctx, off, buf, n)) != ESP_OK) break;
        size_t padded = (n + 3u) & ~3u;   /* esp_flash_write wants 4-byte lengths */
        if (padded > n) memset(buf + n, 0xFF, padded - n);
        if ((err = esp_flash_write(NULL, buf, addr + off, padded)) != ESP_OK) break;
        if ((err = esp_flash_read(NULL, chk, addr + off, n)) != ESP_OK) break;
        if (memcmp(chk, buf, n) != 0) { err = ESP_ERR_INVALID_CRC; break; }
        off += n;
    }

    free(buf);
    free(chk);
    return err;
}

typedef struct { const uint8_t *buf; } ram_src_t;
typedef struct { const esp_partition_t *part; size_t base; } flash_src_t;

static esp_err_t ram_src_read(void *ctx, size_t off, uint8_t *dst, size_t len)
{
    memcpy(dst, ((ram_src_t *)ctx)->buf + off, len);
    return ESP_OK;
}

static esp_err_t flash_src_read(void *ctx, size_t off, uint8_t *dst, size_t len)
{
    const flash_src_t *s = (const flash_src_t *)ctx;
    return esp_partition_read(s->part, s->base + off, dst, len);
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

    ESP_LOGW(TAG, "return-to-stock: writing stock app to %s (app_%d), fs=%s, %u B heap free",
             app_part->label, slot, fs_part ? fs_part->label : "(none)",
             (unsigned)esp_get_free_heap_size());

    manifest_t man;
    bool have_manifest = false;
    bool wrote_app = false;

    /* The stock bootloader is staged in the (unused) zb_storage partition rather
     * than in RAM: with Matter, Thread, WiFi and BLE up there is not reliably
     * 21 KB of heap left, and a failed malloc aborted the restore right after the
     * upload started. The partition table and boot state are small enough to keep
     * in RAM. Nothing outside the inactive app slot and this staging area is
     * touched until the app image has verified. */
    const esp_partition_t *stage = esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, "zb_storage");
    bool staged_boot = false;
    uint8_t *pt_buf = NULL, *ota_buf = NULL;
    size_t   boot_len = 0,   pt_len = 0, ota_len = 0;
    uint32_t boot_addr = 0,  pt_addr = 0;

#define RESTORE_FAIL(msg) do { fail(req, (msg)); goto cleanup; } while (0)

    /* Walk the ZIP local file headers sequentially. The stock package streams
     * the small parts (boot, pt, otadata) before the large app+fs, so they are
     * captured first and only flashed once the app image has verified. */
    for (;;) {
        uint8_t lfh[ZIP_LFH_LEN];
        if (rx_exact(req, lfh, ZIP_LFH_LEN) != ESP_OK) RESTORE_FAIL("truncated zip");

        uint32_t sig = rd32(lfh);
        if (sig == ZIP_CDH_SIG || sig == ZIP_EOCD_SIG) break;   /* end of members */
        if (sig != ZIP_LFH_SIG) RESTORE_FAIL("not a zip package");

        uint16_t method   = rd16(lfh + 8);
        uint32_t comp_sz  = rd32(lfh + 18);
        uint16_t name_len = rd16(lfh + 26);
        uint16_t extra_len= rd16(lfh + 28);
        if (method != 0) RESTORE_FAIL("zip must be uncompressed (STORED)");
        if (name_len >= 64) RESTORE_FAIL("zip member name too long");

        char name[64] = {0};
        if (rx_exact(req, name, name_len) != ESP_OK) RESTORE_FAIL("truncated zip");
        if (extra_len && rx_skip(req, extra_len) != ESP_OK) RESTORE_FAIL("truncated zip");

        if (strcmp(name, "manifest.json") == 0) {
            if (comp_sz == 0 || comp_sz > MAX_MANIFEST) RESTORE_FAIL("bad manifest size");
            char *json = malloc(comp_sz + 1);
            if (!json) RESTORE_FAIL("out of memory");
            if (rx_exact(req, json, comp_sz) != ESP_OK) { free(json); RESTORE_FAIL("truncated manifest"); }
            json[comp_sz] = 0;
            bool ok = manifest_parse(json, comp_sz, &man);
            free(json);
            if (!ok) RESTORE_FAIL("invalid manifest");
            have_manifest = true;
            continue;
        }

        if (!have_manifest) RESTORE_FAIL("manifest.json must come first");

        const part_t *part = manifest_lookup(&man, name);
        const char *type = part ? part->type : "";
        const char *want_sha = part ? part->sha : "";

        if (part && strcmp(type, "app") == 0) {
            if (write_member(req, comp_sz, app_part, 0, want_sha, true) != ESP_OK) RESTORE_FAIL("app write/verify failed");
            wrote_app = true;
        } else if (part && strcmp(type, "fs") == 0) {
            if (write_member(req, comp_sz, fs_part, 0, want_sha, true) != ESP_OK) RESTORE_FAIL("fs write/verify failed");
        } else if (part && strcmp(type, "boot") == 0) {
            if (!stage) RESTORE_FAIL("no staging partition for the bootloader");
            if (comp_sz > stage->size) RESTORE_FAIL("bootloader too large");
            if (write_member(req, comp_sz, stage, 0, want_sha, false) != ESP_OK) RESTORE_FAIL("bootloader staging failed");
            staged_boot = true;
            boot_len = comp_sz;
            boot_addr = part->has_addr ? part->addr : 0x0u;
        } else if (part && strcmp(type, "pt") == 0) {
            if (comp_sz > 0x1000) RESTORE_FAIL("partition table too large");
            if (capture_part(req, comp_sz, want_sha, &pt_buf, &pt_len) != ESP_OK) RESTORE_FAIL("partition table capture failed");
            pt_addr = part->has_addr ? part->addr : 0x10000u;
        } else if (part && strcmp(type, "otadata") == 0) {
            if (comp_sz > 0x2000) RESTORE_FAIL("boot state too large");
            if (capture_part(req, comp_sz, want_sha, &ota_buf, &ota_len) != ESP_OK) RESTORE_FAIL("boot state capture failed");
        } else {
            ESP_LOGW(TAG, "skipping part '%s' (type '%s')", name, type);
            if (rx_skip(req, comp_sz) != ESP_OK) RESTORE_FAIL("truncated zip");
        }
    }

    if (!have_manifest || !wrote_app) RESTORE_FAIL("package has no app image");

    esp_err_t err;

    /*
     * Commit. Everything below only runs after the app image verified, and the
     * running slot is never overwritten. A full stock package (boot + otadata,
     * as every official Shelly .zip contains) is restored so the device becomes
     * byte-for-byte stock again: the Shelly OS loader boots the stock app which
     * expects its own bootloader and an SH0S boot state.
     *
     * Write order is chosen so the irreversible bootloader write at 0x0 is last:
     *   1. otadata  <- stock boot_state.bin, patched to select the slot the
     *                  stock app was actually written to
     *   2. pt       <- stock partition-table.bin @ 0x10000
     *   3. bootloader <- stock bootloader.bin @ 0x0  (Shelly OS loader)
     * If the write at 0x0 is interrupted the device needs UART recovery from the
     * full backup; this is documented on the management page.
     */
    if (ota_buf && staged_boot) {
        const esp_partition_t *ota = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
        if (!ota || ota_len > ota->size) RESTORE_FAIL("otadata partition mismatch");

        /* The stock boot state hard-codes app_1, but the stock app went to
         * whichever slot was inactive. Patch the SH0S entries in RAM so a single
         * write lands the correct boot state: re-reading it back from flash and
         * flipping the slot afterwards cannot work here, because the stock
         * loader (which owns that format) is only written in step 3. */
        if ((err = shelly_boot_patch_state(ota_buf, ota_len, slot)) != ESP_OK)
            RESTORE_FAIL("stock boot state has no usable SH0S entry");

        if ((err = esp_partition_erase_range(ota, 0, ota->size)) != ESP_OK) RESTORE_FAIL("otadata erase failed");
        if ((err = esp_partition_write(ota, 0, ota_buf, ota_len)) != ESP_OK) RESTORE_FAIL("otadata write failed");

        ram_src_t pt_src = { pt_buf };
        if (pt_buf && write_raw_flash(pt_addr, ram_src_read, &pt_src, pt_len) != ESP_OK)
            RESTORE_FAIL("partition table write failed — restore your full UART backup instead");

        flash_src_t boot_src = { stage, 0 };
        if (write_raw_flash(boot_addr, flash_src_read, &boot_src, boot_len) != ESP_OK)
            RESTORE_FAIL("bootloader write failed — restore your full UART backup instead");

        ESP_LOGW(TAG, "return-to-stock: restored stock bootloader + pt + boot state");
    } else {
        /* Package without a bootloader/boot-state (non-standard): fall back to a
         * boot-select flip only. Works when the current loader still matches. */
        ESP_LOGW(TAG, "package has no boot/otadata part — boot-select flip only");
        if (shelly_loader_present())
            err = shelly_boot_switch_slot(slot);
        else
            err = esp_ota_set_boot_partition(app_part);
        if (err != ESP_OK) RESTORE_FAIL("boot-select failed — restore your full UART backup instead");
    }

    /* Clear our NVS so stock starts clean (the factory `shelly` partition is
     * left intact). Done last, after the boot path is committed. */
    if (man.erase_nvs) {
        const esp_partition_t *nvs = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
        if (nvs) {
            nvs_flash_deinit();
            esp_partition_erase_range(nvs, 0, nvs->size);
            ESP_LOGW(TAG, "NVS erased for stock");
        }
    }

    /* The staging copy has served its purpose; leave zb_storage erased so stock
     * initialises it from scratch, like the NVS wipe above. */
    if (staged_boot) esp_partition_erase_range(stage, 0, stage->size);

    free(pt_buf); free(ota_buf);
    ESP_LOGW(TAG, "return-to-stock complete, rebooting into stock app_%d", slot);
    httpd_resp_sendstr(req, "OK");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;

cleanup:
    free(pt_buf); free(ota_buf);
    return ESP_FAIL;
#undef RESTORE_FAIL
}
