#include "shelly_boot.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_flash.h"

static const char *TAG = "shelly_boot";

/* Identifying string embedded in the stock "Shelly OS loader" bootloader
 * image. A plain ESP-IDF bootloader does not contain it. */
#define SHELLY_LOADER_SIG   "Shelly OS loader"
#define BOOTLOADER_SCAN_LEN 0x8000u   /* covers the whole 2nd-stage bootloader */

bool shelly_loader_present(void)
{
    static int cached = -1;   /* -1 unknown, 0 IDF loader, 1 stock loader */
    if (cached >= 0) return cached == 1;

    const size_t chunk = 0x1000u;
    const size_t siglen = sizeof(SHELLY_LOADER_SIG) - 1;
    uint8_t *buf = malloc(chunk + siglen);
    if (!buf) return false;   /* do not cache: retry next time */

    bool found = false;
    size_t carry = 0;   /* bytes kept from the previous chunk for overlap */
    for (uint32_t addr = 0; addr < BOOTLOADER_SCAN_LEN && !found; addr += chunk) {
        if (esp_flash_read(NULL, buf + carry, addr, chunk) != ESP_OK) break;
        size_t total = carry + chunk;
        if (total >= siglen) {
            for (size_t i = 0; i + siglen <= total; i++) {
                if (memcmp(buf + i, SHELLY_LOADER_SIG, siglen) == 0) { found = true; break; }
            }
        }
        /* keep the last (siglen-1) bytes so a signature split across the
         * chunk boundary is still matched. */
        carry = siglen - 1;
        memmove(buf, buf + total - carry, carry);
    }
    free(buf);

    cached = found ? 1 : 0;
    ESP_LOGI(TAG, "bootloader at 0x0: %s", found ? "stock Shelly OS loader"
                                                 : "ESP-IDF");
    return found;
}

/* Snapshot of a valid SH0S entry taken at boot; used as a template when the
 * live otadata sectors have been overwritten by an IDF OTA path. */
static uint8_t s_template[0x1000u];
static bool    s_have_template = false;

/*
 * SH0S boot-select structure layout (reverse-engineered from the stock
 * "Shelly OS loader 1.0.1"). Two 0x200-byte copies live in the otadata
 * partition, one at sector offset 0 and one at sector offset 0x1000.
 *
 * The loader picks the valid copy with the highest seq and boots slot `as`.
 * A copy is considered valid when:
 *   - magic (@0x08) == "SH0S"
 *   - seq (@0x00) != 0
 *   - seq == seq2 (@0x0c)
 *   - crc @0x1c matches (see bs_crc_hdr)
 *   - crc @0x1fc matches (see bs_crc_body)
 */
#define BS_SECTOR_SIZE 0x1000u
#define BS_OFF_SEQ     0x00u
#define BS_OFF_MAGIC   0x08u
#define BS_OFF_SEQ2    0x0Cu
#define BS_OFF_CRC_HDR 0x1Cu  /* crc32 over [0:0x1c] chained with 4 zero bytes  */
#define BS_OFF_FLAGS0  0x1D0u /* bit0=as, bit1=rs, bits4-7=ba                   */
#define BS_OFF_FLAGS1  0x1D1u /* low nibble=c (committed), high nibble=mfs      */
#define BS_OFF_CRC_BODY 0x1FCu /* crc32 over [0:0x1fc]                          */
#define BS_MAGIC       0x53304853u /* "SH0S" little-endian */

static uint32_t rd32(const uint8_t *b, size_t off)
{
    uint32_t v;
    memcpy(&v, b + off, sizeof(v));
    return v;
}

static void wr32(uint8_t *b, size_t off, uint32_t v)
{
    memcpy(b + off, &v, sizeof(v));
}

/* crc @0x1c: esp_rom_crc32_le(0xffffffff, buf, 0x1c) then chained over 4 zero bytes */
static uint32_t bs_crc_hdr(const uint8_t *b)
{
    static const uint8_t zero[4] = {0, 0, 0, 0};
    uint32_t c = esp_rom_crc32_le(0xFFFFFFFFu, b, BS_OFF_CRC_HDR);
    c = esp_rom_crc32_le(c, zero, sizeof(zero));
    return c;
}

/* crc @0x1fc: esp_rom_crc32_le(0xffffffff, buf, 0x1fc) */
static uint32_t bs_crc_body(const uint8_t *b)
{
    return esp_rom_crc32_le(0xFFFFFFFFu, b, BS_OFF_CRC_BODY);
}

static bool bs_valid(const uint8_t *b)
{
    uint32_t seq  = rd32(b, BS_OFF_SEQ);
    uint32_t seq2 = rd32(b, BS_OFF_SEQ2);
    if (rd32(b, BS_OFF_MAGIC) != BS_MAGIC) return false;
    if (seq == 0)                          return false;
    if (seq != seq2)                       return false;
    if (rd32(b, BS_OFF_CRC_HDR)  != bs_crc_hdr(b))  return false;
    if (rd32(b, BS_OFF_CRC_BODY) != bs_crc_body(b)) return false;
    return true;
}

esp_err_t shelly_boot_switch_slot(int slot)
{
    if (slot != 0 && slot != 1) return ESP_ERR_INVALID_ARG;

    const esp_partition_t *ota = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (!ota) {
        ESP_LOGE(TAG, "no otadata partition");
        return ESP_ERR_NOT_FOUND;
    }
    if (ota->size < 2 * BS_SECTOR_SIZE) {
        ESP_LOGE(TAG, "otadata too small (0x%" PRIx32 ")", ota->size);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *s0 = malloc(BS_SECTOR_SIZE);
    uint8_t *s1 = malloc(BS_SECTOR_SIZE);
    uint8_t *nb = malloc(BS_SECTOR_SIZE);
    esp_err_t err = ESP_OK;
    if (!s0 || !s1 || !nb) { err = ESP_ERR_NO_MEM; goto out; }

    if ((err = esp_partition_read(ota, 0, s0, BS_SECTOR_SIZE)) != ESP_OK) goto out;
    if ((err = esp_partition_read(ota, BS_SECTOR_SIZE, s1, BS_SECTOR_SIZE)) != ESP_OK) goto out;

    bool v0 = bs_valid(s0);
    bool v1 = bs_valid(s1);

    /* Winner = valid entry with highest seq. On a tie the loader keeps BS0.
     * The new entry is written to the OTHER sector (tgt) so a valid copy always
     * survives a power failure mid-write. */
    const uint8_t *tmpl;
    size_t tgt;
    if (v0 || v1) {
        int winner;
        if (v0 && v1) winner = (rd32(s0, BS_OFF_SEQ) >= rd32(s1, BS_OFF_SEQ)) ? 0 : 1;
        else          winner = v0 ? 0 : 1;
        tmpl = winner ? s1 : s0;
        tgt  = winner ? 0 : BS_SECTOR_SIZE;
    } else if (s_have_template) {
        /* Live sectors were clobbered by an IDF OTA path (e.g. Matter OTA).
         * Rebuild from the boot-time snapshot; only our written sector will be
         * a valid SH0S, so the loader boots it. */
        ESP_LOGW(TAG, "no live SH0S entry, rebuilding from boot snapshot");
        tmpl = s_template;
        tgt  = 0;
    } else {
        /* Not the stock Shelly loader layout (e.g. running the IDF bootloader). */
        ESP_LOGW(TAG, "no valid SH0S boot-select entry found");
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }

    /* Build the new entry from the template so all body bytes stay valid. */
    memcpy(nb, tmpl, BS_SECTOR_SIZE);

    uint32_t old_as  = (uint32_t)(nb[BS_OFF_FLAGS0] & 0x01u);
    uint32_t new_seq = rd32(nb, BS_OFF_SEQ) + 1u;
    wr32(nb, BS_OFF_SEQ,  new_seq);
    wr32(nb, BS_OFF_SEQ2, new_seq);

    uint8_t f0 = nb[BS_OFF_FLAGS0];
    f0 = (uint8_t)((f0 & ~0x01u) | (uint32_t)slot);        /* as = target slot          */
    f0 = (uint8_t)((f0 & ~0x02u) | (old_as << 1));         /* rs = previous active slot  */
    f0 = (uint8_t)(f0 & ~0xF0u);                           /* ba = 0 (boot attempts)     */
    nb[BS_OFF_FLAGS0] = f0;

    /* flags1: low nibble = committed, high nibble = mfs. The stock loader
     * rolls back to the reserve slot when the booted entry is NOT committed,
     * so we must set committed=1 (matches the factory-fresh state 0x01). */
    nb[BS_OFF_FLAGS1] = 0x01u;                             /* c = 1, mfs = 0 */

    wr32(nb, BS_OFF_CRC_HDR,  bs_crc_hdr(nb));
    wr32(nb, BS_OFF_CRC_BODY, bs_crc_body(nb));

    if ((err = esp_partition_erase_range(ota, tgt, BS_SECTOR_SIZE)) != ESP_OK) goto out;
    if ((err = esp_partition_write(ota, tgt, nb, BS_SECTOR_SIZE)) != ESP_OK) goto out;

    /* Verify read-back. */
    uint8_t *chk = s0; /* reuse buffer */
    if ((err = esp_partition_read(ota, tgt, chk, BS_SECTOR_SIZE)) != ESP_OK) goto out;
    if (memcmp(chk, nb, BS_SECTOR_SIZE) != 0 || !bs_valid(chk)) {
        ESP_LOGE(TAG, "boot-select verify failed");
        err = ESP_ERR_INVALID_CRC;
        goto out;
    }

    ESP_LOGI(TAG, "SH0S boot-select -> app_%d (seq %" PRIu32 ", sector 0x%x)",
             slot, new_seq, (unsigned)tgt);

out:
    free(s0);
    free(s1);
    free(nb);
    return err;
}

esp_err_t shelly_boot_patch_state(uint8_t *state, size_t len, int slot)
{
    if (!state || (slot != 0 && slot != 1)) return ESP_ERR_INVALID_ARG;

    int patched = 0;
    for (size_t off = 0; off + BS_SECTOR_SIZE <= len; off += BS_SECTOR_SIZE) {
        uint8_t *s = state + off;
        if (rd32(s, BS_OFF_MAGIC) != BS_MAGIC) continue;

        uint8_t f0 = s[BS_OFF_FLAGS0];
        f0 = (uint8_t)((f0 & ~0x01u) | (uint32_t)slot);              /* as = target slot   */
        f0 = (uint8_t)((f0 & ~0x02u) | ((uint32_t)(slot ? 0 : 1) << 1)); /* rs = other slot */
        f0 = (uint8_t)(f0 & ~0xF0u);                                 /* ba = 0             */
        s[BS_OFF_FLAGS0] = f0;
        s[BS_OFF_FLAGS1] = 0x01u;   /* committed = 1, mfs = 0: no rollback on first boot */

        wr32(s, BS_OFF_CRC_HDR,  bs_crc_hdr(s));
        wr32(s, BS_OFF_CRC_BODY, bs_crc_body(s));
        if (!bs_valid(s)) return ESP_ERR_INVALID_CRC;
        patched++;
    }

    if (patched == 0) {
        ESP_LOGE(TAG, "stock boot state carries no SH0S entry");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "stock boot state patched -> app_%d (%d SH0S entries)", slot, patched);
    return ESP_OK;
}

void shelly_boot_snapshot(void)
{
    const esp_partition_t *ota = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (!ota || ota->size < 2 * BS_SECTOR_SIZE) return;

    uint8_t *s0 = malloc(BS_SECTOR_SIZE);
    uint8_t *s1 = malloc(BS_SECTOR_SIZE);
    if (!s0 || !s1) { free(s0); free(s1); return; }

    if (esp_partition_read(ota, 0, s0, BS_SECTOR_SIZE) == ESP_OK &&
        esp_partition_read(ota, BS_SECTOR_SIZE, s1, BS_SECTOR_SIZE) == ESP_OK) {
        bool v0 = bs_valid(s0);
        bool v1 = bs_valid(s1);
        const uint8_t *win = NULL;
        if (v0 && v1) win = (rd32(s0, BS_OFF_SEQ) >= rd32(s1, BS_OFF_SEQ)) ? s0 : s1;
        else if (v0)  win = s0;
        else if (v1)  win = s1;
        if (win) {
            memcpy(s_template, win, BS_SECTOR_SIZE);
            s_have_template = true;
            ESP_LOGI(TAG, "SH0S snapshot cached (seq %" PRIu32 ")",
                     rd32(win, BS_OFF_SEQ));
        }
    }
    free(s0);
    free(s1);
}
