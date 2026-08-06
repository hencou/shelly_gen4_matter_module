/*
 * One-time stock Shelly OS loader -> ESP-IDF bootloader migration.
 * See loader_migrate.h for the rationale and safety model.
 */
#include "loader_migrate.h"
#include "shelly_boot.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "loader_migrate";

/* Embedded ESP-IDF bootloader image (build/bootloader/bootloader.bin), produced
 * by embed_bootloader.cmake at build time. */
extern const unsigned char idf_bootloader_bin[];
extern const size_t idf_bootloader_bin_len;

#define NVS_NAMESPACE   "loadermig"
#define NVS_KEY_TRIES   "tries"
#define MAX_TRIES       2   /* give up (keep running under stock loader) after this */

/* Erase + write `len` bytes to raw flash offset `addr`, then verify. */
static esp_err_t raw_write_verify(uint32_t addr, const uint8_t *buf, size_t len)
{
    size_t erase = (len + 4095u) & ~4095u;   /* round up to a flash sector */
    esp_err_t err = esp_flash_erase_region(NULL, addr, erase);
    if (err != ESP_OK) return err;
    if ((err = esp_flash_write(NULL, buf, addr, len)) != ESP_OK) return err;

    uint8_t *chk = malloc(len);
    if (!chk) return ESP_ERR_NO_MEM;
    err = esp_flash_read(NULL, chk, addr, len);
    if (err == ESP_OK && memcmp(chk, buf, len) != 0) err = ESP_ERR_INVALID_CRC;
    free(chk);
    return err;
}

void loader_migrate_maybe(void)
{
    /* Only relevant while the stock Shelly OS loader is still at 0x0. Once we
     * have flashed our ESP-IDF bootloader this returns false, so the migration
     * never runs again. */
    if (!shelly_loader_present()) return;

    if (idf_bootloader_bin_len == 0) {
        ESP_LOGE(TAG, "no embedded bootloader; skipping migration");
        return;
    }

    /* Bound the number of attempts so a persistent write failure cannot trap
     * the device in a reboot loop -- the app still boots fine under the stock
     * loader (SH0S), it just does not gain the ESP-IDF loader end state. */
    nvs_handle_t nvs;
    uint8_t tries = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, NVS_KEY_TRIES, &tries);
        if (tries >= MAX_TRIES) {
            nvs_close(nvs);
            ESP_LOGW(TAG, "loader migration gave up after %u tries; "
                          "running under stock loader", tries);
            return;
        }
        nvs_set_u8(nvs, NVS_KEY_TRIES, (uint8_t)(tries + 1));
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run) {
        ESP_LOGE(TAG, "no running partition");
        return;
    }

    ESP_LOGW(TAG, "stock loader detected; migrating to ESP-IDF bootloader "
                  "(running slot %s @ 0x%08" PRIx32 ", %u byte loader)",
             run->label, run->address, (unsigned)idf_bootloader_bin_len);

    /* 1. Write valid ESP-IDF otadata selecting the currently running slot.
     *    The otadata partition still holds SH0S data, which is not a valid
     *    ESP-IDF select entry, so esp_ota_set_boot_partition() takes the
     *    "both invalid" path and writes a correct entry for `run`. */
    esp_err_t err = esp_ota_set_boot_partition(run);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "otadata write failed: %s -- aborting migration",
                 esp_err_to_name(err));
        return;
    }

    /* 2. Replace the stock loader at 0x0 with our ESP-IDF bootloader. This is
     *    the only irreversible step; if interrupted the device needs UART
     *    recovery from a full flash backup. */
    err = raw_write_verify(0x0, idf_bootloader_bin, idf_bootloader_bin_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bootloader write failed: %s", esp_err_to_name(err));
        return;
    }

    /* Success: clear the attempt counter and reboot into our loader. */
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, NVS_KEY_TRIES);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    ESP_LOGW(TAG, "loader migration complete; rebooting into ESP-IDF bootloader");
    esp_restart();
}
