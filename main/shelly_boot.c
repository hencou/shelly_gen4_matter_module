#include "shelly_boot.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

static const char *TAG = "shelly_boot";

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
    if (!v0 && !v1) {
        /* Not the stock Shelly loader layout (e.g. running the IDF bootloader). */
        ESP_LOGW(TAG, "no valid SH0S boot-select entry found");
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }

    /* Winner = valid entry with highest seq. On a tie the loader keeps BS0. */
    int winner;
    if (v0 && v1) winner = (rd32(s0, BS_OFF_SEQ) >= rd32(s1, BS_OFF_SEQ)) ? 0 : 1;
    else          winner = v0 ? 0 : 1;

    const uint8_t *win = winner ? s1 : s0;

    /* Build the new entry from the current winner so all body bytes stay valid. */
    memcpy(nb, win, BS_SECTOR_SIZE);

    uint32_t old_as  = (uint32_t)(nb[BS_OFF_FLAGS0] & 0x01u);
    uint32_t new_seq = rd32(nb, BS_OFF_SEQ) + 1u;
    wr32(nb, BS_OFF_SEQ,  new_seq);
    wr32(nb, BS_OFF_SEQ2, new_seq);

    uint8_t f0 = nb[BS_OFF_FLAGS0];
    f0 = (uint8_t)((f0 & ~0x01u) | (uint32_t)slot);        /* as = target slot          */
    f0 = (uint8_t)((f0 & ~0x02u) | (old_as << 1));         /* rs = previous active slot  */
    f0 = (uint8_t)(f0 & ~0xF0u);                           /* ba = 0 (boot attempts)     */
    nb[BS_OFF_FLAGS0] = f0;

    uint8_t f1 = nb[BS_OFF_FLAGS1];
    f1 = (uint8_t)((f1 & ~0x0Fu) | 0x01u);                 /* c = 1 (committed)          */
    nb[BS_OFF_FLAGS1] = f1;

    wr32(nb, BS_OFF_CRC_HDR,  bs_crc_hdr(nb));
    wr32(nb, BS_OFF_CRC_BODY, bs_crc_body(nb));

    /* Write to the non-winner sector so the old valid entry survives a power
     * failure mid-write; the new (higher seq) entry then wins on next boot. */
    size_t tgt = winner ? 0 : BS_SECTOR_SIZE;
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
